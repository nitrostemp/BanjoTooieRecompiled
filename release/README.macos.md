# Banjo-Tooie: Recompiled for macOS

Apple Silicon macOS alpha. The game is not included: you need your own
supported copy of Banjo-Tooie. There is no installer.

This macOS build comes from a community fork. Report macOS-specific problems
to the fork, not the Windows project.

## Start playing

1. Keep `BanjoTooieRecompiled.app` together with these notes, or move the app
   to Applications.
2. The app is signed ad hoc, not with an Apple developer ID, so macOS blocks
   the first launch. Open it once, then choose **Open Anyway** in
   **System Settings > Privacy & Security**. Alternatively, run
   `xattr -dr com.apple.quarantine BanjoTooieRecompiled.app` in Terminal.
3. Choose **Choose ROM...** (or **Enter ROM path manually**) and select your
   own NTSC-U 1.0 big-endian `.z64` Banjo-Tooie ROM. Then choose **Start Game**.
4. Open **Controls** to check bindings. Press Escape during play for settings.

Rendering uses Metal through RT64. See the [release notes](docs/RELEASE_NOTES.md)
for changes and known issues.

Supported ROM SHA-256:

```text
9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583
```

Check it in Terminal with `shasum -a 256 /path/to/rom.z64`.

## Saves

**Start Private Practice...** opens a separate session with one durable
save-state slot that never writes normal progress; see the player guide.

Your profile, including saves, settings and logs, is in
`~/Library/Application Support/BanjoTooieRecompiled`. Back up its `saves`
folder before trying progression tools or a different version. F5 is ordinary
progress saving, not a save state. Turning off cheats stops active effects but
keeps progression unlocks.

## More information

- [Player guide](docs/PLAYER_GUIDE.md): controls, saves and troubleshooting.
  Where it names `%LOCALAPPDATA%`, use the macOS profile folder above.
- [Release notes](docs/RELEASE_NOTES.md): features and known limits.
- [Third-party notices](THIRD_PARTY_NOTICES.md) and `third_party/notices`.
