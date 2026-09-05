package dev.enginehost.plugin.cmvs;

/**
 * CMVS's mangled MD5. The engine runs one MD5 block over the four header words
 * and then permutes the state differently per game family, so the result is not
 * an MD5 digest and cannot be produced with java.security.MessageDigest.
 */
final class CmvsMd5 {
    /** Which permutation a game family uses; the names are the families, not the games. */
    enum Variant { A, B, CHRONO, MEMORIA, NATSU, AOI, MIRAI }

    private static final int[] SINE = new int[64];
    private static final int[][] SHIFTS = {
        {7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21},
    };

    static {
        for (int i = 0; i < 64; i++) {
            SINE[i] = (int) (long) (Math.abs(Math.sin(i + 1)) * 4294967296.0);
        }
    }

    private CmvsMd5() {
    }

    /** Returns the four transformed words; the input is left alone. */
    static int[] compute(Variant variant, int[] source) {
        int[] state = initialState(variant);
        int[] buffer = new int[16];
        System.arraycopy(source, 0, buffer, 0, 4);
        buffer[4] = 0x80;
        buffer[14] = 0x80;
        transform(state, buffer);
        return permute(variant, state);
    }

    private static int[] initialState(Variant variant) {
        switch (variant) {
            case A:
            case CHRONO: return new int[] {0xC74A2B01, 0xE7C8AB8F, 0xD8BEDC4E, 0x7302A4C5};
            case B: return new int[] {0x53FE9B2C, 0xF2C93EA8, 0xEE81BA59, 0xA2C8973E};
            case MEMORIA: return new int[] {0xA79463F9, 0xB6E755C5, 0xC696AF21, 0x6983E978};
            case NATSU: return new int[] {0x63FE9A7C, 0xC2B93E98, 0xEF91BA5C, 0x72C9A82E};
            case AOI: return new int[] {0xC74A2B02, 0xE7C8AB8F, 0x38BEBC4E, 0x7531A4C3};
            case MIRAI: return new int[] {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};
            default: throw new IllegalArgumentException("Unknown CMVS MD5 variant");
        }
    }

    private static int[] permute(Variant variant, int[] s) {
        switch (variant) {
            case A: return new int[] {s[3], s[1], s[2], s[0]};
            case CHRONO: return new int[] {
                s[2] ^ 0x45A76C2F, s[1] - 0x5BA17FCB, s[0] ^ 0x79ABE8AD, s[3] - 0x1C08561B};
            case B: return new int[] {
                s[1] ^ 0x49875325, s[2] + 0x54F46D7D, s[3] ^ 0xAD7948B7, s[0] + 0x1D0638AD};
            case MEMORIA: return new int[] {s[1], s[2], s[3], s[0]};
            case NATSU: return new int[] {
                s[1] + 0x45876329, s[2] ^ 0x54F36D6C, s[3] + 0x4387A749, s[0] ^ 0xE3F9A742};
            case AOI: return new int[] {
                s[2] ^ 0x53A76D2E, s[1] + 0x5BB17FDA, s[0] + 0x6853E14D, s[3] ^ 0xF5C6A9A3};
            case MIRAI: return new int[] {s[0], s[1], s[2], s[3]};
            default: throw new IllegalArgumentException("Unknown CMVS MD5 variant");
        }
    }

    private static void transform(int[] state, int[] buffer) {
        int a = state[0], b = state[1], c = state[2], d = state[3];
        for (int i = 0; i < 64; i++) {
            int f;
            int g;
            if (i < 16) {
                f = d ^ (b & (c ^ d));
                g = i;
            } else if (i < 32) {
                f = c ^ (d & (b ^ c));
                g = (5 * i + 1) & 0xF;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) & 0xF;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) & 0xF;
            }
            int t = d;
            d = c;
            c = b;
            b += Integer.rotateLeft(a + f + buffer[g] + SINE[i], SHIFTS[i >> 4][i & 3]);
            a = t;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }
}
