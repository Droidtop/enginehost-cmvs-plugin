#include "scheme.h"

const uint32_t cmvs_common_secret[24] = {
    0xCD90F089u, 0xE982B782u, 0xA282AB88u, 0xCD82718Eu, 0x52838A83u, 0xA882AA82u,
    0x7592648Eu, 0xB582AB82u, 0xE182BF82u, 0xDC82A282u, 0x4281B782u, 0xED82F48Eu,
    0xBF82EA82u, 0xA282E182u, 0xB782DC82u, 0x6081E682u, 0xC6824181u, 0xA482A282u,
    0xE082A982u, 0xF48EA482u, 0xBF82C182u, 0xA282E182u, 0xB582DC82u, 0xF481BD82u,
};

const cmvs_scheme cmvs_schemes[] = {
    /*
     * ChronoClock (Purple Software). Confirmed against the game's own archives:
     * every one of its twelve CPZ6 files indexes cleanly and their entries
     * decrypt to PS2A scripts, PB3B images and Ogg audio.
     */
    {
        "Chrono Clock", CMVS_MD5_CHRONO, cmvs_common_secret,
        0x1A74F195u, 0x2748C39Eu, 0x5C29E87Bu, 0xAEu, 10,
        0x2A65CB4Fu, 0x784C5062u, 0x7Du,
        {0u, 0x11003322u, 0u, 0x34216785u},
    },
    /*
     * The constants GARbro falls back on for CPZ5. Kept as a second attempt so
     * an unlisted CPZ5 game is not refused outright.
     */
    {
        "CPZ5 fallback", CMVS_MD5_MIRAI, cmvs_common_secret,
        0x1A743125u, 0x2547A39Eu, 0x5C29E87Bu, 0xBCu, 9,
        0x2A65CB4Eu, 0x784C5962u, 0x79u,
        {0u, 0x00112233u, 0u, 0x34258765u},
    },
};

const int cmvs_scheme_count = (int) (sizeof cmvs_schemes / sizeof cmvs_schemes[0]);
