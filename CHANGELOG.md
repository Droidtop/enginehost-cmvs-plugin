# Changelog

All notable changes to this plugin are documented in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Unstable and testing stay moving pointers to whichever build was last
published to each; every build they ever point at also gets a permanent
release of its own (`<bundle>-build.<run>`), which is never overwritten.

## [Unreleased]

### Added

- A permanent, never-overwritten release for every bundle build published
  to any channel, so a version that was once installable stays that way in
  the release history even after the next push moves the channel pointers.

## [0.1] - 2026-09-26

This is the plugin's first version-history entry: there was no changelog
before this release, so this section summarizes what already exists.

### Added

- Runs CMVS (PS2/PS3-era) visual novels through a native wrapper around
  the engine, with the engine put in the bundle and the wrapper handing
  Java the engine's own frames.
- Sandbox layer 2 support: isolated launches bridge to the host's file
  broker.
- Signed bundle releases on the unstable and testing channels, verified
  file by file against this repository's pinned key at install time.

### Fixed

- `jni.c`'s `nativeOpen` RegisterNatives entry used the wrong signature
  (it takes 3 arguments, not what was previously registered).
- A stray tag an editor had left in `jni.c` and `CmvsPlugin.java`.
