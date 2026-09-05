package dev.enginehost.plugin.cmvs;

import java.io.IOException;

/**
 * Decoder for PB3B, the image format every CMVS graphic is stored in.
 *
 * <p>ChronoClock's 13,348 images use four of the format's variants:
 * type 1 (per-channel 16x16 block coding over an LZSS plane), type 3 (a
 * JPEG-like "JBP1" transform codec with a run-length alpha channel), type 5
 * (four delta-accumulated LZSS channels) and type 6 (an 8x8 block overlay
 * patched onto a named base image, which is how the game stores expression
 * variants without repeating the whole portrait). Types 4 and 7 exist in the
 * format but appear in none of this game's archives, so they are rejected
 * rather than guessed at.
 *
 * <p>Pixels come out as BGRA, top-down, with a stride of 4 * width, which is
 * the layout the format itself works in.
 */
final class CmvsImage {

    /** Supplies a sibling image by name, for the type 6/8 overlay variants. */
    interface BaseLoader {
        byte[] load(String name) throws IOException;
    }

    final int width;
    final int height;
    final boolean hasAlpha;
    final byte[] pixels;

    private CmvsImage(int width, int height, boolean hasAlpha, byte[] pixels) {
        this.width = width;
        this.height = height;
        this.hasAlpha = hasAlpha;
        this.pixels = pixels;
    }

    static CmvsImage decode(byte[] data, BaseLoader loader) throws IOException {
        return new Reader(data, loader, 0).run();
    }

    /** ARGB rows, ready for {@code Bitmap.createBitmap(int[], int, int, Config)}. */
    int[] toArgb() {
        int[] out = new int[width * height];
        int src = 0;
        for (int i = 0; i < out.length; i++) {
            int b = pixels[src] & 0xFF;
            int g = pixels[src + 1] & 0xFF;
            int r = pixels[src + 2] & 0xFF;
            int a = hasAlpha ? (pixels[src + 3] & 0xFF) : 0xFF;
            src += 4;
            out[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        return out;
    }

    // ------------------------------------------------------------- little end

    private static int u8(byte[] b, int o) { return b[o] & 0xFF; }

    private static int u16(byte[] b, int o) {
        return (b[o] & 0xFF) | ((b[o + 1] & 0xFF) << 8);
    }

    private static int i32(byte[] b, int o) {
        return (b[o] & 0xFF) | ((b[o + 1] & 0xFF) << 8)
             | ((b[o + 2] & 0xFF) << 16) | ((b[o + 3] & 0xFF) << 24);
    }

    /** Guards every offset the file itself supplies, so a corrupt image throws. */
    private static void bounds(byte[] b, int offset, int length, String what) throws IOException {
        if (offset < 0 || length < 0 || offset + length > b.length) {
            throw new IOException("PB3 " + what + " lies outside the file");
        }
    }

    // ------------------------------------------------------------ the reader

    private static final int MAX_BASE_DEPTH = 4;

    private static final byte[] NAME_KEY_V6 = {
        (byte) 0xA6, (byte) 0x75, (byte) 0xF3, (byte) 0x9C,
        (byte) 0xC5, (byte) 0x69, (byte) 0x78, (byte) 0xA3,
        (byte) 0x3E, (byte) 0xA5, (byte) 0x4F, (byte) 0x79,
        (byte) 0x59, (byte) 0xFE, (byte) 0x3A, (byte) 0xC7,
    };

    private static final class Reader {
        final byte[] in;
        final BaseLoader loader;
        final int depth;

        final int type;
        final int subType;
        final int width;
        final int height;
        final int bpp;
        final int channels;
        final int stride;

        boolean hasAlpha;
        byte[] out;
        final byte[] frame = new byte[0x800];

        Reader(byte[] data, BaseLoader loader, int depth) throws IOException {
            if (data.length < 0x40 || data[0] != 'P' || data[1] != 'B' || data[2] != '3' || data[3] != 'B') {
                throw new IOException("Not a PB3B image");
            }
            this.in = data;
            this.loader = loader;
            this.depth = depth;
            this.subType = i32(data, 0x18);
            this.type = u16(data, 0x1C);
            this.width = u16(data, 0x1E);
            this.height = u16(data, 0x20);
            this.bpp = u16(data, 0x22);
            if (width <= 0 || height <= 0) throw new IOException("PB3 image has no extent");
            if (bpp != 8 && bpp != 24 && bpp != 32) throw new IOException("PB3 bit depth " + bpp);
            if (type == 1 && subType != 0x10) {
                throw new IOException("PB3 type 1 subtype 0x" + Integer.toHexString(subType) + " is unknown");
            }
            this.channels = bpp / 8;
            this.stride = 4 * width;
            this.hasAlpha = channels >= 4;
        }

        CmvsImage run() throws IOException {
            switch (type) {
                case 1: unpackV1(); break;
                case 5: unpackV5(); break;
                case 6: case 8: unpackV6(); break;
                case 2: case 3: unpackJbp(0x34, i32(in, 0x2C)); break;
                default: throw new IOException("PB3 type " + type + " images are not supported");
            }
            return new CmvsImage(width, height, hasAlpha, out);
        }

        // ------------------------------------------------------------- LZSS

        void resetFrame() {
            java.util.Arrays.fill(frame, 0, 0x7DE, (byte) 0);
        }

        void lzssUnpack(int bitSrc, int dataSrc, byte[] output, int outputSize) throws IOException {
            int dst = 0;
            int bitMask = 0x80;
            int fp = 0x7DE;
            while (dst < outputSize) {
                if (0 == bitMask) {
                    bitMask = 0x80;
                    bitSrc++;
                }
                bounds(in, bitSrc, 1, "LZSS control");
                if (0 != (u8(in, bitSrc) & bitMask)) {
                    bounds(in, dataSrc, 2, "LZSS match");
                    int v = u16(in, dataSrc);
                    dataSrc += 2;
                    int count = (v & 0x1F) + 3;
                    int offset = v >> 5;
                    for (int i = 0; i < count && dst < outputSize; i++) {
                        byte b = frame[(i + offset) & 0x7FF];
                        output[dst++] = b;
                        frame[fp] = b;
                        fp = (fp + 1) & 0x7FF;
                    }
                } else {
                    bounds(in, dataSrc, 1, "LZSS literal");
                    byte b = in[dataSrc++];
                    output[dst++] = b;
                    frame[fp] = b;
                    fp = (fp + 1) & 0x7FF;
                }
                bitMask >>= 1;
            }
        }

        // --------------------------------------------- type 1: block channels

        void unpackV1() throws IOException {
            out = new byte[stride * height];
            int xBlocks = (width + 15) >> 4;
            int yBlocks = (height + 15) >> 4;
            byte[] plane = new byte[width * height];

            int data1 = i32(in, 0x2C);
            int data2 = i32(in, 0x30);

            for (int channel = 0; channel < channels; channel++) {
                int channelOffset = 4 * channels;
                for (int i = 0; i < channel; i++) channelOffset += i32(in, data1 + 4 * i);
                int head = data1 + channelOffset;
                bounds(in, head, 12, "type 1 channel header");
                int bitSrc = head + 12 + i32(in, head) + i32(in, head + 4);
                int channelSize = i32(in, head + 8);
                if (channelSize < 0 || channelSize > plane.length) {
                    throw new IOException("PB3 type 1 channel size is out of range");
                }

                channelOffset = 4 * channels;
                for (int i = 0; i < channel; i++) channelOffset += i32(in, data2 + 4 * i);
                int dataSrc = data2 + channelOffset;

                resetFrame();
                lzssUnpack(bitSrc, dataSrc, plane, channelSize);

                int planeSrc = 0;
                bitSrc = head + 12;
                int bitMask = 0x80;
                dataSrc = bitSrc + i32(in, head);
                int bottom = 16;
                for (int y = 0; y < yBlocks; y++) {
                    int row = 16 * y;
                    int right = 16;
                    int dstOrigin = stride * row + channel;
                    for (int x = 0; x < xBlocks; x++) {
                        int dst = dstOrigin;
                        int blockWidth = right > width ? width - 16 * x : 16;
                        int blockHeight = bottom > height ? height - row : 16;
                        if (0 == bitMask) {
                            bitSrc++;
                            bitMask = 0x80;
                        }
                        bounds(in, bitSrc, 1, "type 1 block control");
                        if (0 != (u8(in, bitSrc) & bitMask)) {
                            bounds(in, dataSrc, 1, "type 1 flat block");
                            byte b = in[dataSrc++];
                            for (int j = 0; j < blockHeight; j++) {
                                int p = dst;
                                for (int i = 0; i < blockWidth; i++) { out[p] = b; p += 4; }
                                dst += stride;
                            }
                        } else {
                            for (int j = 0; j < blockHeight; j++) {
                                int p = dst;
                                for (int i = 0; i < blockWidth; i++) { out[p] = plane[planeSrc++]; p += 4; }
                                dst += stride;
                            }
                        }
                        bitMask >>= 1;
                        right += 16;
                        dstOrigin += 64;
                    }
                    bottom += 16;
                }
            }
        }

        // ------------------------------------------ type 5: delta LZSS planes

        void unpackV5() throws IOException {
            out = new byte[stride * height];
            for (int i = 0; i < 4; i++) {
                int bitSrc = 0x54 + i32(in, 8 * i + 0x34);
                int dataSrc = 0x54 + i32(in, 8 * i + 0x38);
                resetFrame();
                int fp = 0x7DE;
                int accum = 0;
                int bitMask = 0x80;
                int dst = i;
                while (dst < out.length) {
                    if (0 == bitMask) {
                        bitSrc++;
                        bitMask = 0x80;
                    }
                    bounds(in, bitSrc, 1, "type 5 control");
                    if (0 != (u8(in, bitSrc) & bitMask)) {
                        bounds(in, dataSrc, 2, "type 5 match");
                        int v = u16(in, dataSrc);
                        dataSrc += 2;
                        int count = (v & 0x1F) + 3;
                        int offset = v >> 5;
                        for (int k = 0; k < count && dst < out.length; k++) {
                            byte b = frame[(k + offset) & 0x7FF];
                            frame[fp] = b;
                            fp = (fp + 1) & 0x7FF;
                            accum = (accum + (b & 0xFF)) & 0xFF;
                            out[dst] = (byte) accum;
                            dst += 4;
                        }
                    } else {
                        bounds(in, dataSrc, 1, "type 5 literal");
                        byte b = in[dataSrc++];
                        frame[fp] = b;
                        fp = (fp + 1) & 0x7FF;
                        accum = (accum + (b & 0xFF)) & 0xFF;
                        out[dst] = (byte) accum;
                        dst += 4;
                    }
                    bitMask >>= 1;
                }
            }
        }

        // ------------------------------------------- type 6/8: overlay a base

        String baseImageName() {
            byte[] name = new byte[0x20];
            int n = 0;
            while (n < 0x20) {
                int c = (in[0x34 + n] & 0xFF) ^ (NAME_KEY_V6[n & 0xF] & 0xFF);
                if (c == 0) break;
                name[n++] = (byte) c;
            }
            return new String(name, 0, n, java.nio.charset.Charset.forName("Shift_JIS")) + ".pb3";
        }

        void unpackV6() throws IOException {
            if (loader == null) throw new IOException("PB3 type " + type + " needs its base image");
            if (depth >= MAX_BASE_DEPTH) throw new IOException("PB3 base images nest too deeply");
            String name = baseImageName();
            byte[] base = loader.load(name);
            if (base == null) throw new IOException("PB3 base image " + name + " is missing");
            byte[] pixels;
            if (base.length > 4 && base[0] == 'P' && base[1] == 'B' && base[2] == '3' && base[3] == 'B') {
                pixels = new Reader(base, loader, depth + 1).run().pixels;
            } else {
                pixels = base;
            }
            int need = stride * height;
            out = new byte[need];
            System.arraycopy(pixels, 0, out, 0, Math.min(pixels.length, need));
            blendOverlay();
        }

        void blendOverlay() throws IOException {
            int bitSrc = 0x20 + i32(in, 0xC);
            int dataSrc = bitSrc + i32(in, 0x2C);
            int overlaySize = i32(in, 0x18);
            if (overlaySize < 8 || overlaySize > 0x8000000) {
                throw new IOException("PB3 overlay size is out of range");
            }
            byte[] overlay = new byte[overlaySize];
            resetFrame();
            lzssUnpack(bitSrc, dataSrc, overlay, overlaySize);

            int oBitSrc = 8;
            int oDataSrc = 8 + i32(overlay, 0);
            int bitMask = 0x80;
            int xBlocks = (width + 7) >> 3;
            int yBlocks = (height + 7) >> 3;
            int h = 0;
            int dstOrigin = 0;
            while (yBlocks > 0) {
                int w = 0;
                for (int x = 0; x < xBlocks; x++) {
                    if (0 == bitMask) {
                        oBitSrc++;
                        bitMask = 0x80;
                    }
                    bounds(overlay, oBitSrc, 1, "overlay control");
                    if (0 == (u8(overlay, oBitSrc) & bitMask)) {
                        int dst = 8 * (dstOrigin + 4 * x);
                        int xCount = Math.min(8, width - w);
                        int yCount = Math.min(8, height - h);
                        for (int j = 0; j < yCount; j++) {
                            int count = 4 * xCount;
                            bounds(overlay, oDataSrc, count, "overlay block");
                            System.arraycopy(overlay, oDataSrc, out, dst, count);
                            oDataSrc += count;
                            dst += stride;
                        }
                    }
                    bitMask >>= 1;
                    w += 8;
                }
                dstOrigin += stride;
                h += 8;
                yBlocks--;
            }
        }

        // -------------------------------------------- type 2/3: JBP + RLE alpha

        void unpackJbp(int jbpPos, int alphaPos) throws IOException {
            Jbp jbp = new Jbp(in, jbpPos);
            out = jbp.unpack();
            if (stride != jbp.stride) {
                int src = jbp.stride;
                int dst = stride;
                for (int y = 1; y < height; y++) {
                    System.arraycopy(out, src, out, dst, stride);
                    src += jbp.stride;
                    dst += stride;
                }
            }
            if (32 == bpp && alphaPos > 0) {
                int dst = 3;
                int end = stride * height;
                while (dst < end) {
                    bounds(in, alphaPos, 1, "alpha run");
                    int alpha = u8(in, alphaPos++);
                    if (alpha != 0 && alpha != 0xFF) {
                        out[dst] = (byte) alpha;
                        dst += 4;
                    } else {
                        bounds(in, alphaPos, 1, "alpha run length");
                        int count = u8(in, alphaPos++);
                        while (count-- > 0 && dst < end) {
                            out[dst] = (byte) alpha;
                            dst += 4;
                        }
                    }
                }
            } else {
                hasAlpha = false;
            }
        }
    }

    // ------------------------------------------------------------------- JBP

    /**
     * "JBP1": a JPEG-shaped codec with 16x16 macroblocks of four luma and two
     * chroma 8x8 blocks, Huffman-coded DC deltas and AC runs, and its own
     * fixed-point IDCT.
     */
    private static final class Jbp {
        private static final int MAX_FREQ = 2100000000;

        private static final byte[] ZIGZAG = {
             1,  8, 16,  9,  2,  3, 10, 17,
            24, 32, 25, 18, 11,  4,  5, 12,
            19, 26, 33, 40, 48, 41, 34, 27,
            20, 13,  6,  7, 14, 21, 28, 35,
            42, 49, 56, 57, 50, 43, 36, 29,
            22, 15, 23, 30, 37, 44, 51, 58,
            59, 52, 45, 38, 31, 39, 46, 53,
            60, 61, 54, 47, 55, 62, 63,  0,
        };

        private static final byte[] REVERSE = new byte[256];
        static {
            for (int i = 0; i < 256; i++) {
                int x = ((i & 0xAA) >> 1) | ((i & 0x55) << 1);
                x = ((x & 0xCC) >> 2) | ((x & 0x33) << 2);
                REVERSE[i] = (byte) (((x >> 4) | (x << 4)) & 0xFF);
            }
        }

        final byte[] in;
        final int dataPos;
        final int format;
        final int alignedWidth;
        final int alignedHeight;
        final int blocksX;
        final int blocksY;
        final int stride;
        final int dcBits;
        final int acBits;
        final byte[] out;

        final short[] quantY = new short[0x40];
        final short[] quantC = new short[0x40];
        Huffman treeDc;
        Huffman treeAc;
        BitStream bitsDc;
        BitStream bitsAc;

        Jbp(byte[] input, int offset) throws IOException {
            bounds(input, offset, 0x24, "JBP header");
            this.in = input;
            this.dataPos = i32(input, offset + 4) + offset;
            this.format = i32(input, offset + 8);
            int w = u16(input, offset + 0x10);
            int h = u16(input, offset + 0x12);
            this.dcBits = i32(input, offset + 0x1C);
            this.acBits = i32(input, offset + 0x20);
            if (w <= 0 || h <= 0 || dcBits < 0 || acBits < 0) throw new IOException("Bad JBP header");
            switch ((format >>> 28) & 3) {
                case 0: alignedWidth = (w + 7) & ~7;    alignedHeight = (h + 7) & ~7;    break;
                case 1: alignedWidth = (w + 0xF) & ~0xF; alignedHeight = (h + 0xF) & ~0xF; break;
                case 2: alignedWidth = (w + 0x1F) & ~0x1F; alignedHeight = (h + 0xF) & ~0xF; break;
                default: throw new IOException("Bad JBP alignment");
            }
            this.blocksX = alignedWidth >> 4;
            this.blocksY = alignedHeight >> 4;
            this.stride = 4 * alignedWidth;
            this.out = new byte[stride * alignedHeight];
        }

        byte[] unpack() throws IOException {
            int treePos = dataPos + 0x80;
            bounds(in, dataPos, 0x90 + 0x80, "JBP tables");
            byte[] treeData = new byte[0x10];
            for (int i = 0; i < 0x10; i++) treeData[i] = (byte) ((in[treePos + i] & 0xFF) + 1);

            int[] freq = new int[0x20];
            for (int i = 0; i < 16; i++) freq[i] = i32(in, dataPos + 4 * i);
            treeDc = new Huffman(treeData, freq);
            freq = new int[0x20];
            for (int i = 0; i < 16; i++) freq[i] = i32(in, dataPos + 0x40 + 4 * i);
            treeAc = new Huffman(treeData, freq);

            int quantPos = treePos + 0x10;
            if (0 != (format & 0x8000000)) {
                for (int i = 0; i < 0x40; i++) {
                    quantY[i] = (short) (in[quantPos + i] & 0xFF);
                    quantC[i] = (short) (in[quantPos + i + 0x40] & 0xFF);
                }
            }
            int bitsOffset = quantPos + 0x80;
            bounds(in, bitsOffset, dcBits + acBits, "JBP bit streams");
            bitsDc = new BitStream(in, bitsOffset, dcBits);
            bitsAc = new BitStream(in, bitsOffset + dcBits, acBits);
            decode();
            return out;
        }

        /**
         * The reference reads a coefficient's sign on 32-bit unsigned values,
         * where a DC bit count of 0 (a legal "unchanged" code) makes the shift
         * count wrap to 31 and the correction term vanish. Java masks shift
         * counts the same way, so only the comparison has to be unsigned.
         */
        private static int signedCoeff(int v, int bitCount) {
            if (Integer.compareUnsigned(v, 1 << (bitCount - 1)) < 0) v -= (1 << bitCount) - 1;
            return v;
        }

        void decode() throws IOException {
            int total = blocksX * blocksY;
            short[] dc = new short[total * 6];
            int prev = 0;
            for (int i = 0; i < dc.length; i++) {
                int n = treeDc.read(bitsDc);
                prev += signedCoeff(bitsDc.getBits(n), n);
                dc[i] = (short) prev;
            }

            short[][] block = new short[6][64];
            for (int y = 0; y < blocksY; y++) {
                int dst1 = y * stride * 16;
                int dst2 = dst1 + stride * 9;
                for (int x = 0; x < blocksX; x++) {
                    for (int j = 0; j < 6; j++) java.util.Arrays.fill(block[j], (short) 0);
                    int base = (y * blocksX + x) * 6;
                    for (int n = 0; n < 6; n++) {
                        block[n][0] = dc[base + n];
                        for (int i = 0; i < 63; ) {
                            int bitCount = treeAc.read(bitsAc);
                            if (15 == bitCount) break;
                            if (0 == bitCount) {
                                int node = 0;
                                while (0 != bitsAc.getBits(1)) node++;
                                if (node >= treeAc.base.length) throw new IOException("Bad JBP AC run");
                                i += treeAc.base[node] & 0xFF;
                            } else {
                                int v = signedCoeff(bitsAc.getBits(bitCount), bitCount);
                                block[n][ZIGZAG[i] & 0xFF] = (short) v;
                                i++;
                            }
                        }
                    }
                    idct(block[0], quantY);
                    idct(block[1], quantY);
                    idct(block[2], quantY);
                    idct(block[3], quantY);
                    idct(block[4], quantC);
                    idct(block[5], quantC);

                    yccToBgr(dst1,              dst1 + stride,      block[0], block[4], block[5], 0);
                    yccToBgr(dst1 + 32,         dst1 + stride + 32, block[1], block[4], block[5], 4);
                    yccToBgr(dst2 - stride,     dst2,               block[2], block[4], block[5], 32);
                    yccToBgr(dst2 - stride + 32, dst2 + 32,         block[3], block[4], block[5], 36);

                    dst1 += 64;
                    dst2 += 64;
                }
            }
        }

        static void idct(short[] t, short[] q) {
            for (int p = 0; p < 8; p++) {
                if (t[p + 0x08] == 0 && t[p + 0x10] == 0 && t[p + 0x18] == 0 && t[p + 0x20] == 0
                        && t[p + 0x28] == 0 && t[p + 0x30] == 0 && t[p + 0x38] == 0) {
                    short v = (short) (t[p] * q[p]);
                    t[p] = t[p + 0x08] = t[p + 0x10] = t[p + 0x18] = v;
                    t[p + 0x20] = t[p + 0x28] = t[p + 0x30] = t[p + 0x38] = v;
                } else {
                    int c = q[p + 0x10] * t[p + 0x10];
                    int d = q[p + 0x30] * t[p + 0x30];
                    int x = ((c + d) * 35467) >> 16;
                    c = ((c * 50159) >> 16) + x;
                    d = ((d * -121094) >> 16) + x;
                    int a = t[p] * q[p];
                    int b = t[p + 0x20] * q[p + 0x20];
                    int w = a + b + c;
                    x = a + b - c;
                    int yy = a - b + d;
                    int z = a - b - d;

                    c = t[p + 0x38] * q[p + 0x38];
                    d = t[p + 0x28] * q[p + 0x28];
                    a = t[p + 0x18] * q[p + 0x18];
                    b = t[p + 0x08] * q[p + 0x08];
                    int n = ((a + b + c + d) * 77062) >> 16;

                    int u = n + ((c * 19571) >> 16) + (((c + a) * -128553) >> 16) + (((c + b) * -58980) >> 16);
                    int v = n + ((d * 134553) >> 16) + (((d + b) * -25570) >> 16) + (((d + a) * -167963) >> 16);
                    int s = n + ((b * 98390) >> 16) + (((d + b) * -25570) >> 16) + (((c + b) * -58980) >> 16);
                    int r = n + ((a * 201373) >> 16) + (((c + a) * -128553) >> 16) + (((d + a) * -167963) >> 16);

                    t[p]        = (short) (w + s);
                    t[p + 0x38] = (short) (w - s);
                    t[p + 0x08] = (short) (yy + r);
                    t[p + 0x30] = (short) (yy - r);
                    t[p + 0x10] = (short) (z + v);
                    t[p + 0x28] = (short) (z - v);
                    t[p + 0x18] = (short) (x + u);
                    t[p + 0x20] = (short) (x - u);
                }
            }
            for (int p = 0; p < 64; p += 8) {
                int a = t[p];
                int c = t[p + 2];
                int b = t[p + 4];
                int d = t[p + 6];
                int x = ((c + d) * 35467) >> 16;
                c = ((c * 50159) >> 16) + x;
                d = ((d * -121094) >> 16) + x;
                int w = a + b + c;
                x = a + b - c;
                int yy = a - b + d;
                int z = a - b - d;

                d = t[p + 5];
                b = t[p + 1];
                c = t[p + 7];
                a = t[p + 3];
                int n = ((a + b + c + d) * 77062) >> 16;

                int s = n + ((a * 201373) >> 16) + (((a + c) * -128553) >> 16) + (((a + d) * -167963) >> 16);
                int u2 = n + ((b * 98390) >> 16) + (((b + c) * -58980) >> 16) + (((b + d) * -25570) >> 16);
                int u = n + ((c * 19571) >> 16) + (((b + c) * -58980) >> 16) + (((a + c) * -128553) >> 16);
                int v = n + ((d * 134553) >> 16) + (((b + d) * -25570) >> 16) + (((a + d) * -167963) >> 16);

                t[p]     = (short) ((w + u2) >> 3);
                t[p + 7] = (short) ((w - u2) >> 3);
                t[p + 1] = (short) ((yy + s) >> 3);
                t[p + 6] = (short) ((yy - s) >> 3);
                t[p + 2] = (short) ((z + v) >> 3);
                t[p + 5] = (short) ((z - v) >> 3);
                t[p + 3] = (short) ((x + u) >> 3);
                t[p + 4] = (short) ((x - u) >> 3);
            }
        }

        static byte clamp(int c) {
            if (c < 0x100) return 0;
            if (c >= 0x200) return (byte) 0xFF;
            return (byte) (c - 0x100);
        }

        void yccToBgr(int dc, int ac, short[] dy, short[] dcb, short[] dcr, int cbcr) {
            int ySrc = 0;
            for (int j = 0; j < 4; j++) {
                for (int i = 0; i < 4; i++) {
                    int cb = dcb[cbcr];
                    int cr = dcr[cbcr];
                    int r = (cr * 0x166F0) >> 16;
                    int g = ((cb * 0x5810) >> 16) + ((cr * 0xB6C0) >> 16);
                    int b = (cb * 0x1C590) >> 16;
                    int c0 = dy[ySrc] + 0x180;
                    int c1 = dy[ySrc + 1] + 0x180;
                    int c8 = dy[ySrc + 8] + 0x180;
                    int c9 = dy[ySrc + 9] + 0x180;
                    out[dc]              = clamp(c0 + b);
                    out[ac + 1 - stride] = clamp(c0 - g);
                    out[ac + 2 - stride] = clamp(c0 + r);
                    out[ac + 4 - stride] = clamp(c1 + b);
                    out[ac + 5 - stride] = clamp(c1 - g);
                    out[ac + 6 - stride] = clamp(c1 + r);
                    out[ac]              = clamp(c8 + b);
                    out[ac + 1]          = clamp(c8 - g);
                    out[ac + 2]          = clamp(c8 + r);
                    out[ac + 4]          = clamp(c9 + b);
                    out[ac + 5]          = clamp(c9 - g);
                    out[ac + 6]          = clamp(c9 + r);
                    ySrc += 2;
                    dc += 8;
                    ac += 8;
                    cbcr++;
                }
                dc += stride * 2 - 32;
                ac += stride * 2 - 32;
                ySrc += 8;
                cbcr += 4;
            }
        }

        /** Frequency-ordered tree over 16 symbols; leaves are the bit counts. */
        static final class Huffman {
            final byte[] base;
            final int[] nodes = new int[0x400];
            final int root;
            final int leafCount;

            Huffman(byte[] base, int[] freq) {
                this.base = base;
                this.leafCount = base.length;
                int depth = base.length;
                for (;;) {
                    int l = -1;
                    int min = MAX_FREQ - 1;
                    for (int i = 0; i < depth; i++) {
                        if (freq[i] < min) { min = freq[i]; l = i; }
                    }
                    int r = -1;
                    min = MAX_FREQ - 1;
                    for (int i = 0; i < depth; i++) {
                        if (i != l && freq[i] < min) { min = freq[i]; r = i; }
                    }
                    if (l < 0 || r < 0) break;
                    nodes[depth] = l;
                    nodes[depth + 0x200] = r;
                    freq[depth++] = freq[l] + freq[r];
                    freq[l] = MAX_FREQ;
                    freq[r] = MAX_FREQ;
                }
                this.root = depth - 1;
            }

            int read(BitStream bits) throws IOException {
                int v = root;
                while (v >= leafCount) v = nodes[v + (bits.getBits(1) << 9)];
                return v;
            }
        }

        /** MSB-first over bit-reversed bytes, which is how the encoder wrote them. */
        static final class BitStream {
            final byte[] in;
            int pos;
            final int end;
            int bits;
            int cached;

            BitStream(byte[] in, int offset, int length) {
                this.in = in;
                this.pos = offset;
                this.end = offset + length;
            }

            int getBits(int count) throws IOException {
                while (cached < count) {
                    if (pos >= end) throw new IOException("JBP bit stream ended early");
                    bits = (bits << 8) | (REVERSE[in[pos++] & 0xFF] & 0xFF);
                    cached += 8;
                }
                cached -= count;
                return (bits >> cached) & ((1 << count) - 1);
            }
        }
    }
}
