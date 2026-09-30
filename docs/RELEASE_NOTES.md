# Release notes — Windows alpha 0.1.0-alpha.3

- Fixed camera jitter near walls.
- Fixed low-rate animations for Witchyworld aliens and rats.
- Fixed jitter in Banjo's backpack and its contents.
- Fixed feather jitter, overlapping feather artifacts, and early screen-edge disappearance.
- Fixed glitching mines in the Atlantis submarine minigame.
- Fixed the wall button jitter during the Saucer of Peril cutscene.
- Added separate archives for F4 issue captures so later markers do not overwrite earlier ones.

Known issues: menu overlays can glitch during entry; shadows can lose coverage across angled floor surfaces.

After updating, create a fresh Private Practice checkpoint. Ordinary saves and settings carry over.

See the [player guide](PLAYER_GUIDE.md) for setup, controls and troubleshooting.

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
