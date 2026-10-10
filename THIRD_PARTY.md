# Research attribution

The bounds-checked PS2/PS3 reader was written for enginehost using format
information from the MIT-licensed
[xmoezzz/CMVS-Engine](https://github.com/xmoezzz/CMVS-Engine) text-dumping
tools. No source code from that project or from the proprietary CMVS runtime is
included here.

Games, CMVS, and their assets are not redistributed by this repository.

The CPZ archive reader (`CmvsArchive.java`, `CmvsMd5.java`, `CmvsScheme.java`)
is a Java port of the MIT-licensed
[morkt/GARbro](https://github.com/morkt/GARbro) `ArcFormats/Cmvs` sources and of
the encryption scheme GARbro records for ChronoClock. GARbro's code is not
vendored; the port is new Java, and every length is checked before use.
