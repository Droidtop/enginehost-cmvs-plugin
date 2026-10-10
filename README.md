# Experimental native CMVS plugin

This first runtime reads CMVS `PS2A` scripts in place. It validates all lengths,
decrypts and LZ-decompresses PS2 scripts, resolves PS3 dialogue string references,
decodes Shift-JIS, and presents the resulting dialogue with tap-to-advance.

It reads scripts out of the game's own `CPZ` archives, which is how a real CMVS
game ships them: `script.cpz` under the folder `cmvs.cfg` names, decrypted with
the per-game scheme and LZSS-expanded. CPZ5 and CPZ6 are handled; CPZ7 is not.

It does not yet execute general CMVS opcodes, choices, graphics, audio, video,
or save state. Capability `ps2-ps3-dialogue` is intentionally experimental and
narrow: it is the smoke test that the archive and script layers are right, not
a way to play a game.

The format work is based on the MIT-licensed
[`xmoezzz/CMVS-Engine`](https://github.com/xmoezzz/CMVS-Engine) PS2 and PS3
text dumpers. The implementation here is new MIT-licensed, bounds-checked Java
code; no proprietary CMVS runtime or game data is included. Android builds run
only in CI.
