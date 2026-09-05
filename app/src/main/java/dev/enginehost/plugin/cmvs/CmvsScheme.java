package dev.enginehost.plugin.cmvs;

import java.util.Arrays;
import java.util.List;

/**
 * The per-game constants a CPZ archive is encrypted with. They live in the
 * game's own executable, not in the archive, so an archive cannot say which set
 * it needs: {@link CmvsArchive} tries each known scheme and keeps the one whose
 * index decrypts into a directory table that makes sense.
 */
final class CmvsScheme {
    /**
     * The 24-word obfuscation table. It is a cp932 sentence the developers left
     * in the executable; most CMVS games ship this same one and vary the
     * scalars below instead.
     */
    static final int[] COMMON_SECRET = {
        0xCD90F089, 0xE982B782, 0xA282AB88, 0xCD82718E, 0x52838A83, 0xA882AA82,
        0x7592648E, 0xB582AB82, 0xE182BF82, 0xDC82A282, 0x4281B782, 0xED82F48E,
        0xBF82EA82, 0xA282E182, 0xB782DC82, 0x6081E682, 0xC6824181, 0xA482A282,
        0xE082A982, 0xF48EA482, 0xBF82C182, 0xA282E182, 0xB582DC82, 0xF481BD82,
    };

    final String name;
    final CmvsMd5.Variant md5Variant;
    final int[] secret;
    final int decoderFactor;
    final int entryInitKey;
    final int entrySubKey;
    final byte entryTailKey;
    final int entryKeyPos;
    final int indexSeed;
    final int indexAddend;
    final int indexSubtrahend;
    final int[] dirKeyAddend;

    private CmvsScheme(String name, CmvsMd5.Variant md5Variant, int[] secret, int decoderFactor,
                       int entryInitKey, int entrySubKey, int entryTailKey, int entryKeyPos,
                       int indexSeed, int indexAddend, int indexSubtrahend, int[] dirKeyAddend) {
        this.name = name;
        this.md5Variant = md5Variant;
        this.secret = secret;
        this.decoderFactor = decoderFactor;
        this.entryInitKey = entryInitKey;
        this.entrySubKey = entrySubKey;
        this.entryTailKey = (byte) entryTailKey;
        this.entryKeyPos = entryKeyPos;
        this.indexSeed = indexSeed;
        this.indexAddend = indexAddend;
        this.indexSubtrahend = indexSubtrahend;
        this.dirKeyAddend = dirKeyAddend;
    }

    /**
     * ChronoClock (Purple Software). Confirmed against the game's own archives:
     * ps.cpz, script.cpz and bg.cpz index cleanly and their entries decrypt to
     * PS2A and PB3B data.
     */
    static final CmvsScheme CHRONO_CLOCK = new CmvsScheme(
        "Chrono Clock", CmvsMd5.Variant.CHRONO, COMMON_SECRET,
        0x1A74F195, 0x2748C39E, 0x5C29E87B, 0xAE, 10,
        0x2A65CB4F, 0x784C5062, 0x7D,
        new int[] {0, 0x11003322, 0, 0x34216785});

    /**
     * The constants GARbro falls back on for CPZ5. Kept as a second attempt so
     * an unlisted CPZ5 game is not refused outright.
     */
    static final CmvsScheme CPZ5_FALLBACK = new CmvsScheme(
        "CPZ5 fallback", CmvsMd5.Variant.MIRAI, COMMON_SECRET,
        0x1A743125, 0x2547A39E, 0x5C29E87B, 0xBC, 9,
        0x2A65CB4E, 0x784C5962, 0x79,
        new int[] {0, 0x00112233, 0, 0x34258765});

    static final List<CmvsScheme> KNOWN = Arrays.asList(CHRONO_CLOCK, CPZ5_FALLBACK);
}
