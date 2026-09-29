# Banjo-Tooie: Recompiled

Windows x64 Alpha 2 ZIP release. The game is not included: you need your own
supported copy of Banjo-Tooie. There is no installer.

## Start playing

1. Keep this folder intact, including `assets` and `runtime-data`.
2. Install the [Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) if Windows asks for it.
3. Open `TooieRecompiled.exe`, choose **Choose ROM...**, and select your own
   NTSC-U 1.0 big-endian `.z64` Banjo-Tooie ROM. Then choose **Start Game**.
4. Open **Controls** to check bindings. Press Escape during play for settings.

Alpha 2 fixes the reported character, fire and ground-button jitter in gameplay
and ground-button jitter in recorded **Press Start** attract demos. These fixes
were verified in the reported scenes with **Custom / Display / Present Early**.

Supported ROM SHA-256:

```text
9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583
```

## Saves

Your profile, including saves and settings, is in
`%LOCALAPPDATA%\BanjoTooieRecompiled`. Back up its `saves` folder before trying
progression tools or a different version. Existing ordinary saves, settings,
controls and the selected ROM remain there when updating from Alpha 1. A
profile from an earlier test build is copied there automatically on first
start, and the original is left unchanged. F5 is ordinary progress saving, not
a save state. Turning off cheats stops active effects but keeps progression
unlocks.

Private Practice states require the same build and settings. After updating,
start a fresh practice session and create a new checkpoint.

## More information

- [Player guide](docs/PLAYER_GUIDE.md): controls, saves, Private Practice and troubleshooting.
- [Release notes](docs/RELEASE_NOTES.md): features and known limits.
- [Report a problem](https://github.com/some-scurvy-dog/BanjoTooieRecompiled/issues).
  Never attach ROMs, saves or unreviewed logs.
- [License](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md).

This is an independent project, not affiliated with or endorsed by Nintendo,
Rare or the upstream projects it builds on. AI tools generated substantial
portions of its code; see the release notes for details.
