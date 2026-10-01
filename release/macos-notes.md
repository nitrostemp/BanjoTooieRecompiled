Banjo-Tooie: Recompiled for Apple Silicon Macs, built from the Windows Alpha 3
source with Metal rendering through RT64. It is a community macOS build of
[Banjo-Tooie: Recompiled](https://github.com/some-scurvy-dog/BanjoTooieRecompiled).

**The game is not included.** You need your own NTSC-U 1.0 big-endian `.z64`
ROM (SHA-256 `9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583`).

## Install

1. Download `BanjoTooieRecompiled-…-macOS-arm64.zip` and unzip it.
2. Open `BanjoTooieRecompiled.app`. It is not signed with an Apple developer ID,
   so the first time macOS blocks it: choose **Open Anyway** in
   **System Settings > Privacy & Security**, or run
   `xattr -dr com.apple.quarantine BanjoTooieRecompiled.app`.
3. Choose **Choose ROM...**, select your ROM, then **Start Game**.

Requires an Apple Silicon Mac running macOS 15 or later. Saves, settings and
logs are kept in `~/Library/Application Support/BanjoTooieRecompiled`.

## Status

The launcher, menus, gameplay, audio, saving and quitting have been checked on
an M3 Pro with macOS 27. A full playthrough on macOS has not been done yet.
Private Practice is included as in the Windows release; its save states have
not been exercised on macOS yet. Intel Macs are not supported. See the
included `README.md` and `docs/RELEASE_NOTES.md` for the Alpha 3 changes and
known limits.
