# Experimental native CMVS plugin

This first runtime reads CMVS `PS2A` scripts in place. It validates all lengths,
decrypts and LZ-decompresses PS2 scripts, resolves PS3 dialogue string references,
decodes Shift-JIS, and presents the resulting dialogue with tap-to-advance.

It does not yet execute general CMVS opcodes, choices, graphics, audio, CPZ
archives, or save state. Games currently need extracted `.ps2`/`.ps3` scripts.
Capability `ps2-ps3-dialogue` is intentionally experimental and narrow.

The format work is based on the MIT-licensed
[`xmoezzz/CMVS-Engine`](https://github.com/xmoezzz/CMVS-Engine) PS2 and PS3
text dumpers. The implementation here is new GPL-3.0 bounds-checked Java
code; no proprietary CMVS runtime or game data is included. Android builds run
only in CI.

## Licence

GPL-3.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE). The code here is ours (the CPZ reader is a port of MIT-licensed GARbro code, notice in LICENSE.GARbro); the third-party attributions in NOTICE and THIRD_PARTY.md keep their own notices.
