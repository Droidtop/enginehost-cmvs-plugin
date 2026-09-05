package dev.enginehost.plugin.cmvs;

import java.io.Closeable;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.Charset;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Reader for CMVS `CPZ` resource archives, which is how a real CMVS game ships
 * everything: scripts, images, sound. Only CPZ5 and CPZ6 are handled; CPZ7 adds
 * a Huffman-packed index key and a per-archive key lifted out of start.ps3,
 * neither of which has been tested against a real game here, so it is refused
 * by name rather than half-implemented.
 *
 * Ported from morkt/GARbro's ArcFormats/Cmvs (MIT). Every length is checked
 * before use: these archives are game data, and a truncated or hostile one must
 * fail with an exception rather than read outside its buffer.
 */
final class CmvsArchive implements Closeable {
    /** One file inside the archive. */
    static final class Entry {
        final String name;
        final long offset;
        final int size;
        final int key;

        Entry(String name, long offset, int size, int key) {
            this.name = name;
            this.offset = offset;
            this.size = size;
            this.key = key;
        }
    }

    /** No single member of a game archive is expected anywhere near this large. */
    private static final int MAX_ENTRY = 256 * 1024 * 1024;
    private static final int MAX_INDEX = 32 * 1024 * 1024;
    private static final int MAX_DIRECTORIES = 4096;
    private static final int INIT_CHECKSUM = 0x923A564C;
    private static final Charset CP932 = Charset.isSupported("windows-31j")
        ? Charset.forName("windows-31j") : Charset.forName("Shift_JIS");

    private final RandomAccessFile file;
    private final long fileLength;
    private final Header header;
    private final CmvsScheme scheme;
    private final Decoder entryDecoder;
    private final Map<String, Entry> entries;

    private CmvsArchive(RandomAccessFile file, long fileLength, Header header, CmvsScheme scheme,
                        Decoder entryDecoder, Map<String, Entry> entries) {
        this.file = file;
        this.fileLength = fileLength;
        this.header = header;
        this.scheme = scheme;
        this.entryDecoder = entryDecoder;
        this.entries = entries;
    }

    /** Opens the archive, trying each known scheme until one produces a sane index. */
    static CmvsArchive open(java.io.File path) throws IOException {
        RandomAccessFile raf = new RandomAccessFile(path, "r");
        boolean opened = false;
        try {
            long length = raf.length();
            Header head = Header.parse(raf, length);
            byte[] index = new byte[head.indexSize];
            raf.seek(head.indexOffset);
            raf.readFully(index);
            if (!Arrays.equals(md5(index, 0, index.length), head.indexMd5)) {
                throw new IOException("CPZ index does not match its own MD5; the archive is damaged");
            }
            for (CmvsScheme candidate : CmvsScheme.KNOWN) {
                Header attempt = head.copy();
                Attempt result = readIndex(attempt, candidate, index.clone(), length);
                if (result != null) {
                    CmvsArchive archive =
                        new CmvsArchive(raf, length, attempt, candidate, result.entryDecoder, result.entries);
                    opened = true;
                    return archive;
                }
            }
            throw new IOException("No known CMVS encryption scheme opens " + path.getName()
                + "; this game's scheme has not been added yet");
        } finally {
            if (!opened) {
                try {
                    raf.close();
                } catch (IOException ignored) {
                    // The caller is already being told why the archive did not open.
                }
            }
        }
    }

    /** The scheme that turned out to fit, for logging. */
    String schemeName() {
        return scheme.name;
    }

    /** Entry names, in archive order, using '/' as the separator. */
    List<String> names() {
        return Collections.unmodifiableList(new ArrayList<>(entries.keySet()));
    }

    Entry find(String name) {
        return entries.get(name.toLowerCase(Locale.ROOT).replace('\\', '/'));
    }

    /**
     * Reads one entry, decrypted, and unpacked if it is a PS2A container.
     * The returned data keeps the PS2A header, whose compressed-size fields
     * still describe the packed form; {@link CmvsScript} is told not to unpack
     * again.
     */
    byte[] read(Entry entry) throws IOException {
        byte[] data = new byte[entry.size];
        file.seek(entry.offset);
        file.readFully(data);
        if (header.encrypted) {
            int key = (header.masterKey ^ entry.key) + header.dirCount;
            key -= scheme.entrySubKey;
            key ^= header.entryKey;
            entryDecoder.decryptEntry(data, header.cmvsMd5, key);
        }
        if (data.length > 0x30 && data[0] == 'P' && data[1] == 'S' && data[2] == '2' && data[3] == 'A') {
            return unpackPs2(data);
        }
        return data;
    }

    @Override public void close() throws IOException {
        file.close();
    }

    // ---------------------------------------------------------------- header

    private static final class Header {
        int version;
        int dirCount;
        int dirEntriesSize;
        int fileEntriesSize;
        int[] cmvsMd5;
        int masterKey;
        boolean encrypted;
        int entryKey;
        int entryNameOffset;
        int indexOffset;
        int indexSize;
        byte[] indexMd5;

        static Header parse(RandomAccessFile raf, long fileLength) throws IOException {
            if (fileLength < 0x40) throw new IOException("File is too small to be a CPZ archive");
            byte[] raw = new byte[0x40];
            raf.seek(0);
            raf.readFully(raw);
            if (raw[0] != 'C' || raw[1] != 'P' || raw[2] != 'Z') throw new IOException("Not a CPZ archive");
            Header h = new Header();
            h.version = raw[3] - '0';
            if (h.version != 5 && h.version != 6) {
                throw new IOException("CPZ" + h.version + " archives are not supported yet");
            }
            ByteBuffer b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
            if (h.version < 6) {
                h.dirCount = 0xFE3A53D9 ^ b.getInt(4);
                h.dirEntriesSize = 0x37F298E7 ^ b.getInt(8);
                h.fileEntriesSize = 0x7A6F3A2C ^ b.getInt(0x0C);
                h.masterKey = 0xAE7D39BF ^ b.getInt(0x30);
                h.encrypted = 0 != (0xFB73A955 ^ b.getInt(0x34));
                h.entryKey = 0;
                h.cmvsMd5 = new int[] {
                    0x43DE7C19 ^ b.getInt(0x20), 0xCC65F415 ^ b.getInt(0x24),
                    0xD016A93C ^ b.getInt(0x28), 0x97A3BA9A ^ b.getInt(0x2C)};
            } else {
                int seed = 0x37ACF832 ^ b.getInt(0x38);
                h.dirCount = 0xFE3A53DA ^ b.getInt(4);
                h.dirEntriesSize = 0x37F298E8 ^ b.getInt(8);
                h.fileEntriesSize = 0x7A6F3A2D ^ b.getInt(0x0C);
                h.masterKey = 0xAE7D39B7 ^ b.getInt(0x30);
                h.encrypted = 0 != (0xFB73A956 ^ b.getInt(0x34));
                h.entryKey = 0x7DA8F173 * Integer.rotateRight(seed, 5) + 0x13712765;
                h.cmvsMd5 = new int[] {
                    0x43DE7C1A ^ b.getInt(0x20), 0xCC65F416 ^ b.getInt(0x24),
                    0xD016A93D ^ b.getInt(0x28), 0x97A3BA9B ^ b.getInt(0x2C)};
            }
            h.entryNameOffset = 0x18;
            h.indexOffset = 0x40;
            if (h.dirCount <= 0 || h.dirCount > MAX_DIRECTORIES
                || h.dirEntriesSize <= 0 || h.fileEntriesSize <= 0
                || h.dirEntriesSize > MAX_INDEX || h.fileEntriesSize > MAX_INDEX) {
                throw new IOException("CPZ header describes an index that cannot be right");
            }
            h.indexSize = h.dirEntriesSize + h.fileEntriesSize;
            if (h.indexOffset + (long) h.indexSize > fileLength) {
                throw new IOException("CPZ index runs past the end of the archive");
            }
            if (b.getInt(0x3C) != checksum(raw, 0, 0x3C, INIT_CHECKSUM)) {
                throw new IOException("CPZ header checksum is wrong; the archive is damaged");
            }
            h.indexMd5 = Arrays.copyOfRange(raw, 0x10, 0x20);
            return h;
        }

        Header copy() {
            Header h = new Header();
            h.version = version;
            h.dirCount = dirCount;
            h.dirEntriesSize = dirEntriesSize;
            h.fileEntriesSize = fileEntriesSize;
            h.cmvsMd5 = cmvsMd5.clone();
            h.masterKey = masterKey;
            h.encrypted = encrypted;
            h.entryKey = entryKey;
            h.entryNameOffset = entryNameOffset;
            h.indexOffset = indexOffset;
            h.indexSize = indexSize;
            h.indexMd5 = indexMd5;
            return h;
        }
    }

    // ----------------------------------------------------------------- index

    private static final class Attempt {
        Map<String, Entry> entries;
        Decoder entryDecoder;
    }

    /** Returns null when this scheme clearly does not fit, so the next one can be tried. */
    private static Attempt readIndex(Header cpz, CmvsScheme scheme, byte[] index, long fileLength) {
        try {
            cpz.cmvsMd5 = CmvsMd5.compute(scheme.md5Variant, cpz.cmvsMd5);
            decryptIndexStage1(index, cpz.masterKey ^ 0x3795B39A, scheme);
            Decoder decoder = new Decoder(scheme, cpz.masterKey, cpz.cmvsMd5[1]);
            decoder.decode(index, 0, cpz.dirEntriesSize, (byte) 0x3A);

            int[] key = {
                cpz.cmvsMd5[0] ^ (cpz.masterKey + 0x76A3BF29),
                cpz.cmvsMd5[1] ^ cpz.masterKey,
                cpz.cmvsMd5[2] ^ (cpz.masterKey + 0x10000000),
                cpz.cmvsMd5[3] ^ cpz.masterKey};
            decryptIndexDirectory(index, cpz.dirEntriesSize, key);

            decoder.init(cpz.masterKey, cpz.cmvsMd5[2]);
            long baseOffset = cpz.indexOffset + (long) cpz.indexSize;
            Map<String, Entry> found = new LinkedHashMap<>();
            int dirOffset = 0;
            for (int i = 0; i < cpz.dirCount; i++) {
                if (dirOffset + 0x10 > cpz.dirEntriesSize) return null;
                int dirSize = readInt(index, dirOffset);
                if (dirSize <= 0x10 || dirOffset + dirSize > cpz.dirEntriesSize) return null;
                int fileCount = readInt(index, dirOffset + 4);
                if (fileCount < 0 || fileCount >= 0x10000) return null;
                int entriesOffset = readInt(index, dirOffset + 8);
                int dirKey = readInt(index, dirOffset + 0x0C);
                String dirName = cString(index, dirOffset + 0x10, dirSize - 0x10);
                if (dirName == null) return null;

                int nextOffset;
                if (i + 1 == cpz.dirCount) {
                    nextOffset = cpz.fileEntriesSize;
                } else {
                    if (dirOffset + dirSize + 12 > cpz.dirEntriesSize) return null;
                    nextOffset = readInt(index, dirOffset + dirSize + 8);
                }
                int size = nextOffset - entriesOffset;
                if (entriesOffset < 0 || size <= 0 || entriesOffset + size > cpz.fileEntriesSize) return null;

                int cursor = cpz.dirEntriesSize + entriesOffset;
                int end = cursor + size;
                decoder.decode(index, cursor, size, (byte) 0x7E);
                int[] entryKey = new int[4];
                for (int j = 0; j < 4; j++) entryKey[j] = cpz.cmvsMd5[j] ^ (dirKey + scheme.dirKeyAddend[j]);
                decryptIndexEntry(index, cursor, size, entryKey, scheme.indexSeed);

                boolean root = "root".equals(dirName);
                for (int j = 0; j < fileCount; j++) {
                    if (cursor + cpz.entryNameOffset > end) return null;
                    int entrySize = readInt(index, cursor);
                    if (entrySize <= cpz.entryNameOffset || cursor + entrySize > end) return null;
                    String name = cString(index, cursor + cpz.entryNameOffset, end - cursor - cpz.entryNameOffset);
                    if (name == null || name.isEmpty()) return null;
                    long offset = readLong(index, cursor + 4) + baseOffset;
                    int entryLength = readInt(index, cursor + 0x0C);
                    if (entryLength < 0 || entryLength > MAX_ENTRY
                        || offset < baseOffset || offset + entryLength > fileLength) {
                        return null;
                    }
                    String full = root ? name : dirName + '/' + name;
                    found.put(full.toLowerCase(Locale.ROOT).replace('\\', '/'),
                        new Entry(full, offset, entryLength, readInt(index, cursor + 0x14) + dirKey));
                    cursor += entrySize;
                }
                dirOffset += dirSize;
            }
            if (found.isEmpty()) return null;
            Attempt attempt = new Attempt();
            attempt.entries = found;
            attempt.entryDecoder = decoder;
            if (cpz.encrypted) decoder.init(cpz.cmvsMd5[3], cpz.masterKey);
            return attempt;
        } catch (RuntimeException wrongScheme) {
            // A scheme that does not fit produces nonsense offsets; that is a
            // rejected candidate, not a failure of the archive.
            return null;
        }
    }

    private static void decryptIndexStage1(byte[] data, int key, CmvsScheme scheme) {
        int[] secret = new int[24];
        for (int i = 0; i < 24 && i < scheme.secret.length; i++) secret[i] = scheme.secret[i] - key;
        int shift = (((key >>> 24) ^ (key >>> 16) ^ (key >>> 8) ^ key ^ 0xB) & 0xF) + 7;
        int words = data.length / 4;
        int s = 5;
        for (int i = 0; i < words; i++) {
            int at = i * 4;
            int value = Integer.rotateRight((secret[s] ^ readInt(data, at)) + scheme.indexAddend, shift)
                + 0x01010101;
            writeInt(data, at, value);
            s = (s + 1) % 24;
        }
        int tail = data.length & 3;
        for (int n = tail; n > 0; n--) {
            int at = data.length - n;
            data[at] = (byte) ((data[at] ^ (secret[s] >>> (n * 4))) - scheme.indexSubtrahend);
            s = (s + 1) % 24;
        }
    }

    private static void decryptIndexDirectory(byte[] data, int length, int[] key) {
        int seed = 0x76548AEF;
        int words = length / 4;
        int i = 0;
        for (; i < words; i++) {
            int at = i * 4;
            writeInt(data, at, Integer.rotateLeft((readInt(data, at) ^ key[i & 3]) - 0x4A91C262, 3) - seed);
            seed += 0x10FB562A;
        }
        for (int j = length & 3; j > 0; j--) {
            int at = length - j;
            data[at] = (byte) ((data[at] ^ (key[i++ & 3] >>> 6)) + 0x37);
        }
    }

    private static void decryptIndexEntry(byte[] data, int offset, int length, int[] key, int seed) {
        int words = length / 4;
        int i = 0;
        for (; i < words; i++) {
            int at = offset + i * 4;
            writeInt(data, at, Integer.rotateLeft((readInt(data, at) ^ key[i & 3]) - seed, 2) + 0x37A19E8B);
            seed -= 0x139FA9B;
        }
        for (int j = length & 3; j > 0; j--) {
            int at = offset + length - j;
            data[at] = (byte) ((data[at] ^ (key[i++ & 3] >>> 4)) + 5);
        }
    }

    // --------------------------------------------------------------- decoder

    /** The substitution table CMVS derives from the archive keys. */
    private static final class Decoder {
        private final CmvsScheme scheme;
        private final byte[] table = new byte[0x100];

        Decoder(CmvsScheme scheme, int key, int summand) {
            this.scheme = scheme;
            init(key, summand);
        }

        void init(int key, int summand) {
            for (int i = 0; i < 0x100; i++) table[i] = (byte) i;
            for (int i = 0; i < 0x100; i++) {
                int a = (key >>> 16) & 0xFF;
                int b = key & 0xFF;
                byte t = table[a];
                table[a] = table[b];
                table[b] = t;
                a = (key >>> 8) & 0xFF;
                b = key >>> 24;
                t = table[a];
                table[a] = table[b];
                table[b] = t;
                key = summand + scheme.decoderFactor * Integer.rotateRight(key, 2);
            }
        }

        void decode(byte[] data, int offset, int length, byte key) {
            for (int i = 0; i < length; i++) {
                data[offset + i] = table[(key ^ data[offset + i]) & 0xFF];
            }
        }

        void decryptEntry(byte[] data, int[] cmvsMd5, int seed) {
            byte[] keyBytes = new byte[0x40];
            for (int i = 0; i < 0x10; i++) writeInt(keyBytes, i * 4, scheme.secret[i]);
            int mangle = cmvsMd5[1] >>> 2;
            for (int i = 0; i < keyBytes.length; i++) {
                keyBytes[i] = (byte) (mangle ^ table[keyBytes[i] & 0xFF]);
            }
            int[] secretKey = new int[0x10];
            for (int i = 0; i < 0x10; i++) secretKey[i] = readInt(keyBytes, i * 4) ^ seed;

            int words = data.length / 4;
            int key = scheme.entryInitKey;
            int k = scheme.entryKeyPos & 0xF;
            for (int i = 0; i < words; i++) {
                int at = i * 4;
                int value = cmvsMd5[key & 3]
                    ^ ((readInt(data, at) ^ secretKey[(key >>> 6) & 0xF] ^ (secretKey[k] >>> 1)) - seed);
                writeInt(data, at, value);
                k = (k + 1) & 0xF;
                key += seed + value;
            }
            for (int i = words * 4; i < data.length; i++) {
                data[i] = table[(data[i] ^ scheme.entryTailKey) & 0xFF];
            }
        }
    }

    // ------------------------------------------------------------- PS2A body

    /**
     * Decrypts and LZSS-expands a PS2A container. The 0x30-byte header is
     * copied through untouched, which is what the rest of the engine expects.
     */
    static byte[] unpackPs2(byte[] data) throws IOException {
        int seed = readInt(data, 12);
        int shift = ((seed >>> 20) % 5) + 1;
        int key = ((seed >>> 24) + (seed >>> 3)) & 0xFF;
        for (int i = 0x30; i < data.length; i++) {
            int value = (key ^ ((data[i] & 0xFF) - 0x7C)) & 0xFF;
            data[i] = (byte) (((value >>> shift) | (value << (8 - shift))) & 0xFF);
        }
        int unpacked = readInt(data, 0x28);
        if (unpacked < 0 || unpacked > MAX_ENTRY) throw new IOException("PS2A unpacked size is out of range");
        byte[] out = new byte[0x30 + unpacked];
        System.arraycopy(data, 0, out, 0, 0x30);
        byte[] frame = new byte[0x800];
        int framePos = 0x7DF;
        int src = 0x30;
        int dst = 0x30;
        int control = 1;
        while (dst < out.length && src < data.length) {
            if (control == 1) control = (data[src++] & 0xFF) | 0x100;
            if ((control & 1) != 0) {
                byte value = data[src++];
                out[dst++] = value;
                frame[framePos++ & 0x7FF] = value;
            } else {
                if (src + 1 >= data.length) break;
                int lo = data[src++] & 0xFF;
                int hi = data[src++] & 0xFF;
                int offset = lo | ((hi & 0xE0) << 3);
                int count = (hi & 0x1F) + 2;
                for (int i = 0; i < count && dst < out.length; i++) {
                    byte value = frame[(offset + i) & 0x7FF];
                    out[dst++] = value;
                    frame[framePos++ & 0x7FF] = value;
                }
            }
            control >>= 1;
        }
        if (dst != out.length) throw new IOException("PS2A stream ended before it was fully expanded");
        return out;
    }

    // --------------------------------------------------------------- helpers

    private static int checksum(byte[] data, int offset, int length, int crc) {
        for (int i = 0; i < length / 4; i++) crc += readInt(data, offset + i * 4);
        for (int i = length & ~3; i < length; i++) crc += data[offset + i] & 0xFF;
        return crc;
    }

    private static byte[] md5(byte[] data, int offset, int length) throws IOException {
        try {
            MessageDigest digest = MessageDigest.getInstance("MD5");
            digest.update(data, offset, length);
            return digest.digest();
        } catch (NoSuchAlgorithmException impossible) {
            throw new IOException("MD5 is unavailable", impossible);
        }
    }

    private static int readInt(byte[] data, int at) {
        return (data[at] & 0xFF) | ((data[at + 1] & 0xFF) << 8)
            | ((data[at + 2] & 0xFF) << 16) | ((data[at + 3] & 0xFF) << 24);
    }

    private static long readLong(byte[] data, int at) {
        return (readInt(data, at) & 0xFFFFFFFFL) | ((long) readInt(data, at + 4) << 32);
    }

    private static void writeInt(byte[] data, int at, int value) {
        data[at] = (byte) value;
        data[at + 1] = (byte) (value >>> 8);
        data[at + 2] = (byte) (value >>> 16);
        data[at + 3] = (byte) (value >>> 24);
    }

    /** cp932 name up to the first NUL, or null if it is not one. */
    private static String cString(byte[] data, int at, int limit) {
        if (at < 0 || limit < 0 || at + limit > data.length) return null;
        int end = at;
        while (end < at + limit && data[end] != 0) end++;
        for (int i = at; i < end; i++) {
            int c = data[i] & 0xFF;
            if (c < 0x20) return null;
        }
        return new String(data, at, end - at, CP932);
    }
}
