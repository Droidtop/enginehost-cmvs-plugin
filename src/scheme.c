#include "scheme.h"

/*
 * The 24-dword taunt string every CPZ5/CPZ6 game keys its index with. It is
 * cp932 text and it sits verbatim in ChronoClock's own cmvs32.exe at 0x146000,
 * which is how it was confirmed rather than copied on faith.
 */
const uint32_t cmvs_common_secret[24] = {
    0xCD90F089u, 0xE982B782u, 0xA282AB88u, 0xCD82718Eu, 0x52838A83u, 0xA882AA82u,
    0x7592648Eu, 0xB582AB82u, 0xE182BF82u, 0xDC82A282u, 0x4281B782u, 0xED82F48Eu,
    0xBF82EA82u, 0xA282E182u, 0xB782DC82u, 0x6081E682u, 0xC6824181u, 0xA482A282u,
    0xE082A982u, 0xF48EA482u, 0xBF82C182u, 0xA282E182u, 0xB582DC82u, 0xF481BD82u,
};

/* Hapymaher carries its own secret rather than the common one. */
static const uint32_t secret_hapymaher[24] = {
    0xCD90F089u, 0xE982B782u, 0xA282AB88u, 0xCD82718Eu, 0x52838A83u, 0xA882AA82u,
    0x7592648Eu, 0xB582AB82u, 0xE182BF82u, 0xDC82A282u, 0x4281B782u, 0x62838183u,
    0xA9824981u, 0xA282ED82u, 0xC682A282u, 0xBE8CA982u, 0xC482C182u, 0x968BE082u,
    0xC482B582u, 0xB082A082u, 0xA282C882u, 0xBE82F182u, 0xE782A982u, 0x49819F82u,
};

/*
 * Every CMVS archive scheme in GARbro's own database (GameData/Formats.dat,
 * "GARbroDB" + zlib + a BinaryFormatter graph; tools/read_garbro_schemes.py
 * reads it). A CPZ archive says nothing about which one opens it, so cpz_open
 * tries each in turn and keeps the one whose index checksum verifies - which
 * is why listing them all is what makes an unseen CMVS game open unmodified,
 * and why nothing here has to guess a title from a folder name.
 *
 * Confirmed on real data: Chrono Clock, whose twelve archives all index and
 * decrypt. The other five are transcribed, not tested - no copy to test with.
 */
const cmvs_scheme cmvs_schemes[] = {
    {   /* CPZ5 */
        "Hapymaher", CMVS_MD5_B, secret_hapymaher,
        0x1A740235u, 0x2547B39Eu, 0x5C29E87Bu, 0xCBu, 9,
        0x2A65CB4Eu, 0x784C5962u, 0x79u,
        {0x00000000u, 0x00112233u, 0x00000000u, 0x34258765u},
    },
    {   /* CPZ5 */
        "Haruiro Ouse", CMVS_MD5_A, cmvs_common_secret,
        0x1A743125u, 0x2547A39Eu, 0x5C29E87Bu, 0xBCu, 9,
        0x2A65CB4Eu, 0x784C5962u, 0x79u,
        {0x00000000u, 0x00112233u, 0x00000000u, 0x34258765u},
    },
    {   /* CPZ5 */
        "Memoria", CMVS_MD5_MEMORIA, cmvs_common_secret,
        0x1A743125u, 0x2547A39Eu, 0x5C29E87Bu, 0xBCu, 9,
        0x2A65CB4Eu, 0x784C5962u, 0x79u,
        {0x00000000u, 0x00112233u, 0x00000000u, 0x34258765u},
    },
    {   /* CPZ5 */
        "Natsu ni Kanaderu Bokura no Uta", CMVS_MD5_NATSU, cmvs_common_secret,
        0x1A743125u, 0x2547A39Eu, 0x5C29E87Bu, 0xBCu, 9,
        0x2A65CB4Eu, 0x784C5962u, 0x79u,
        {0x00000000u, 0x00112233u, 0x00000000u, 0x34258765u},
    },
    {   /* CPZ6 */
        "Chrono Clock", CMVS_MD5_CHRONO, cmvs_common_secret,
        0x1A74F195u, 0x2748C39Eu, 0x5C29E87Bu, 0xAEu, 10,
        0x2A65CB4Fu, 0x784C5062u, 0x7Du,
        {0x00000000u, 0x11003322u, 0x00000000u, 0x34216785u},
    },
    {   /* CPZ7 */
        "Aoi Tori", CMVS_MD5_AOI, cmvs_common_secret,
        0x1A74F195u, 0x2748C39Eu, 0x5C39E87Bu, 0xAEu, 10,
        0x2A65CB4Fu, 0x784C5062u, 0x7Du,
        {0x00000000u, 0x11003322u, 0x00000000u, 0x34216785u},
    },
    /*
     * The constants GARbro falls back on for CPZ5 when no title matches, kept
     * last so an unlisted CPZ5 game is tried rather than refused outright.
     */
    {
        "CPZ5 fallback", CMVS_MD5_MIRAI, cmvs_common_secret,
        0x1A743125u, 0x2547A39Eu, 0x5C29E87Bu, 0xBCu, 9,
        0x2A65CB4Eu, 0x784C5962u, 0x79u,
        {0u, 0x00112233u, 0u, 0x34258765u},
    },
};

const int cmvs_scheme_count = (int) (sizeof cmvs_schemes / sizeof cmvs_schemes[0]);
