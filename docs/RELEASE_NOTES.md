# Release notes — Windows alpha 0.1.0-alpha.2

These notes describe the Windows x64 Alpha 2 ZIP release. The exact version is
shown on the launcher's About page, and the package's
`candidate-manifest.json` lists the SHA-256 of every file it contains. The
[Alpha 1 release](https://github.com/some-scurvy-dog/BanjoTooieRecompiled/releases/tag/v0.1.0-alpha.1)
remains available for reference.

Banjo-Tooie Recompiled is a native Windows game made through static
recompilation, with RT64 rendering. You must supply your own supported game
ROM. Neither the ROM nor extracted game assets are included.

## What's new in Alpha 2

Fixed camera-motion jitter affecting characters, fire and ground buttons in
gameplay. Also fixed ground-button jitter in recorded **Press Start** attract
demos. These fixes were verified in the reported scenes with **Custom / Display
/ Present Early**; they are not a claim about every scene or PC.

Alpha 2 keeps Alpha 1's launcher, controls, graphics options, ordinary saves
and Private Practice features. Existing saves, settings, controls and selected
ROM remain in `%LOCALAPPDATA%\BanjoTooieRecompiled`. Back up your `saves`
folder before updating. Private Practice states depend on the exact build and
settings; start a fresh practice session and create a new checkpoint in Alpha 2.

## Requirements and installation

The current release target is Windows x64. Minimum Windows, GPU and driver
requirements have not been established. The application may need the
[Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).

Extract the entire ZIP to a folder, keep its files together and run
`TooieRecompiled.exe`. There is no installer. The
package must contain its companion files; copying only the executable is not
supported. This is a native Windows application. WSL and source-generation
tools are for developers, not part of the player's setup.

Use only the NTSC-U 1.0 big-endian `.z64` ROM with SHA-256:

```text
9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583
```

Other versions, regions, byte orders and modified ROMs are unsupported. Check
the hash in [the player guide](PLAYER_GUIDE.md) if the launcher rejects a file.

## Included features

- A Windows launcher that validates a user-provided ROM.
- Keyboard and controller input, remapping and rumble on supported devices.
- Graphics, framing, audio and pacing controls.
- Ordinary game saves and optional cheat/progression controls.
- A separate Private Practice session with one durable save-state slot.
- Diagnostics and an F4 marker to help report problems.

The recommended pacing setup is Custom, VSync Off, Display output target and
Present Early. Console remains available as an N64-style option with VSync On,
the original output rate and Console presentation. Higher output rates and
some settings are experimental and may vary by scene or hardware.

## Tested behavior and limits

The owner has confirmed the latest visual checks for Expanded HUD counters and
the aiming reticle, Console pacing, and vertical title-logo motion. Earlier
accepted cases include repeated Private Practice saves and loads, selected
world warps, and tested transformations. Those checks cover the tested cases;
they do not establish that every map, route, form, setting or controller works.

This alpha has not been completed from beginning to end and has not been
validated across a broad range of Windows PCs. Minimum requirements and broad
hardware compatibility are unknown. Mac/Apple Silicon and Linux builds are not
ready. Check the player guide before trying travel or transformation tools,
which can affect position and unsaved practice progress.

Known limits:

- Visual artifacts have been reported in some scenes, such as distant geometry
  or sky polygons. Press F4 when one appears and include the time in a report.
- Scene transitions can still pause briefly.
- After travel or progression actions, characters and objects already in the
  area may not update until you leave and re-enter it.
- Draw distance changes how far away characters fade; it does not change
  terrain, fog or the camera's view distance.
- Some boss and progression controls cover selected encounters only; not every
  change can be reversed.

## Saves and cheats

Ordinary play uses normal in-game progress saves. F5 requests an ordinary save
while the original pause menu is open; it is not an exact-position save state.
Private Practice runs separately and does not write normal progress. Its single
state slot requires the same build, ROM, gameplay settings and render settings;
an update may invalidate a saved state. After changing builds, make a fresh
practice checkpoint.

The application stores its profile in `%LOCALAPPDATA%\BanjoTooieRecompiled`. On
first start it copies a profile from the earlier `TooieRecomp-frontend` folder
and leaves the original unchanged; see the player guide for details.

Turning off active cheat effects does not remove permanent unlocks, such as
moves, egg types or opened routes. Back up saves before using progression tools.
See [the player guide](PLAYER_GUIDE.md) for details.

## Report a problem

Use the [issue tracker](https://github.com/some-scurvy-dog/BanjoTooieRecompiled/issues)
and include the version or executable hash, Windows/GPU/driver, controller,
settings, location and steps to reproduce the issue. F4 adds a marker to the
logs; take a separate screenshot if useful. Redact usernames and local paths.
Do not upload ROMs, game assets, saves, generated game data, credentials, an
entire profile or unreviewed logs/captures.

## Credits and disclosure

This project uses [N64Recomp](https://github.com/N64Recomp/N64Recomp),
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime),
[RT64](https://github.com/rt64/rt64), [Dear ImGui](https://github.com/ocornut/imgui)
and other libraries listed in [third-party notices](../THIRD_PARTY_NOTICES.md).
Their contributors retain credit for their work. The project is independent
and has no upstream endorsement.

AI tools generated substantial portions of this project's code and assisted
with engineering and review. The project owner directed development, tested
the game and verified its behavior; generated code was not manually written by
the owner. See the contributor guide in the
[source repository](https://github.com/some-scurvy-dog/BanjoTooieRecompiled).

Project material that its authors can license is under
[GPL-3.0-only](../LICENSE). Dependencies and other material retain their own
terms. This project is unofficial and is not affiliated with Nintendo or Rare.
