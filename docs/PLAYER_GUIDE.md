# Windows player guide — Alpha 3

This guide covers the Windows launcher and game menus. Extract all of the
package's contents to one folder and keep the files together. Run
`TooieRecompiled.exe`; there is no installer. Minimum Windows and hardware requirements have not been
established. The application runs as a native Windows program; WSL and the
development tools are not player requirements.

## First launch

Install the [Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
if Windows reports that a required runtime component is missing. Keep the
package's DLLs, `assets` and `runtime-data` beside the application as supplied.

On the **Play** page, choose **Choose ROM...** and select your own supported
Banjo-Tooie ROM. The launcher validates it and shows **Selected ROM: NTSC-U 1.0
validated**. Then choose **Start Game**. If the ROM is rejected, check the file
and its hash below. **Enter ROM path manually** opens a path field; choose
**Validate Path** after entering it.

The game is not included. Only the NTSC-U 1.0 big-endian `.z64` image matching
this SHA-256 is supported:

```text
9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583
```

To check a file in PowerShell, replace the example path with the path to your
own ROM:

```powershell
(Get-FileHash -Algorithm SHA256 'C:\path\to\your.z64').Hash.ToLowerInvariant()
```

Other regions, byte orders and modified ROMs are unsupported. A filename does
not identify the ROM's contents. The launcher validates and stores a local copy
in your profile for later launches. Never upload a ROM or extracted game assets.

## Launcher and controls

Use the mouse, keyboard or a controller to navigate the launcher. On a
controller, use the D-pad or left stick to move, A to confirm, and Back to open
or close in-game settings. Under **Controls**, select the device used for play
and binding capture, give it a name, select its controller type, and choose or
create a mapping profile. Keyboard bindings are separate. When rebinding,
release the control that opened the popup, then press the new input after capture
starts.

Default keyboard bindings:

| Game input | Key |
| --- | --- |
| Move | W / A / S / D |
| A / B | J / K |
| Z | Space |
| L / R | Q / E |
| C buttons | Arrow keys |
| Start / original pause menu | Enter |
| Application settings | Escape |

In game, Escape or controller Back opens and closes the settings menu. On its
**Play** page, use **Resume Game**, **Restart Game**, or **Return to Launcher**.
Restart returns to the title sequence. Return shows the ROM launcher. Both ask
you to confirm because unsaved progress will be lost; cancel keeps the game
running. Your saved games, selected ROM, controller profiles and settings stay
in place.

| Shortcut | What it does |
| --- | --- |
| F3 | Cycles Off, FPS, Detailed and Practice diagnostic views. |
| F4 | Adds a marker to the logs; it does not take a screenshot. |
| F5 | Requests an ordinary progress save while the original pause menu is open. |
| F6 (hold) | Fast forward. |
| F7 | Requests the game's skip action in supported intro/title sequences. |

## Graphics and sound

Start with the current recommended pacing setup: **Custom**, **VSync Off**,
**Display** output target and **Present Early**. Choose **Restore Recommended
Settings** on the pacing page to restore these values. For an N64-style
presentation option, choose **Console**, which uses VSync On and the original
output rate. Display and presentation options may behave differently by scene
and hardware; high output rates are not a promise of full high-frame-rate
compatibility.

See the [release notes](RELEASE_NOTES.md) for the latest rendering fixes and
known issues.

The Graphics pages let you choose the display, window mode, output size,
rendering scale, supported anti-aliasing, downsampling and game framing. Under
**Framing**, **HUD Proportions** offers **Original (4:3 elements)** and **Stretch
with screen**; **HUD Counter Placement** offers **Centered** and **Expanded**.
Some changes need a restart; follow the message shown by the menu and use
**Apply Graphics** when available. Higher rendering scales and downsampling
increase GPU load. **General > Camera > Analog Camera** is an optional experimental
control; its inversion options apply to supported analog-camera modes, not all
original C-button controls.

Main volume controls overall sound. The separate music control covers tracks
from the original Jukebox list; other music cues may not follow it.

## Saves and Private Practice

Ordinary play uses the game's normal save slots. F5 requests a normal progress
save while the original pause menu is open. It does not save your exact
position, and it is separate from a Private Practice state.

To practice without changing normal progress, choose **Start Private
Practice...** on the launcher's **Play** page or from **Tools > Practice** in the
game menu. Confirm **Start Private Practice**. This starts a separate session;
normal game saves are not written during it. Unsaved progress in the session is
lost when you leave unless you capture it with the practice state slot.

In **Tools > Practice**, choose **Save State** to capture the current practice
session or **Load State** to restore it. There is one slot; saving replaces its
previous contents and loading discards unsaved practice progress. A state only
works with the same build, ROM, gameplay settings and rendering settings. An
update may make an earlier state unusable. When updating to a new alpha, start a fresh practice session and save a new checkpoint. This feature
is experimental; confirm important results in ordinary gameplay.

Your profile is in `%LOCALAPPDATA%\BanjoTooieRecompiled`. Ordinary saves are
in its `saves` folder; it also holds your settings, controls, the validated ROM
copy and logs. Close the application and back up the whole `saves` folder
before changing builds or using progression tools. Removing the profile is not
a general troubleshooting step.

Earlier test builds used `%LOCALAPPDATA%\TooieRecomp-frontend`. The first time
this version starts, it copies that folder to the new location, checks the copy,
and tells you when it is done. The original folder is left unchanged, so you can
keep it as a backup or delete it yourself later. Ordinary saves, settings and
controls carry over. A practice state saved by an earlier version may not load;
start a new Private Practice session and save a fresh state. If the copy cannot
be completed, for example because another copy of the game is still running or
the disk is full, the game explains what to do and does not start with an empty
profile.

The two folders are never merged. Once this version has created or copied the
new folder, it keeps using it even if the old folder is still there. If the new
folder already holds saves or settings that this version did not put there, for
example because you copied files into it yourself, and the old folder also
exists, the game does not start. Instead it names both folders so you can keep
the one you want and rename or move the other.

To keep the profile beside the application instead, put an empty file named
`portable.txt` in the application's folder. For a separate profile, run the
application with `--profile-dir` and a folder you choose. Only one copy of the
game can use a profile at a time. Using another profile can make existing
progress appear missing without deleting the original saves.

## Cheats and progression tools

**Tools > Enable Cheats** opens optional effects and progression actions.
Temporary effects, such as Super Banjo or infinite ammunition, can be turned
off with the supported controls. Moves, egg types, opened stations and other
progression unlocks change normal game progress; turning off active effects
does not reverse those unlocks. Back up your saves first.

The **Disable File 1 Cheats**, **Disable File 2 Cheats** and **Disable File 3
Cheats** controls clear supported saved cheat effects from the selected file.
They create a backup before changing save data when a change is needed. They do
not remove permanent progress or affect other files. After changing a file,
choose **Start Game** to use it.

Practice offers selected warp-pad and Isle o' Hags silo entrances, and an
experimental transformation reload. These tools cover selected routes and
forms, not every area or combination. A transition or reload can affect your
position and unsaved progress. Checkpoint your practice state before using
them, and verify results in the game.

## Troubleshooting and reporting

| Problem | First check |
| --- | --- |
| Missing DLL message | Keep the package together and install Microsoft's official x64 Visual C++ Redistributable. Do not download individual DLLs from unofficial sites. |
| ROM rejected | Check the NTSC-U 1.0 `.z64` format and SHA-256 above. Renaming does not convert a ROM. |
| Progress seems missing | Check which profile you launched, including `--profile-dir` or `portable.txt`, and whether an older `TooieRecomp-frontend` folder has newer saves. Do not overwrite the old save. |
| Choppy or unusual presentation | Try the recommended pacing settings and record your graphics settings and location if the problem continues. |
| Moves or routes remain after disabling cheats | These are permanent progress unlocks, not active cheat effects. |

Report reproducible problems in the [issue tracker](https://github.com/some-scurvy-dog/BanjoTooieRecompiled/issues).
Include the version or executable SHA-256, Windows version, GPU and driver,
controller, relevant settings, game location, and steps to reproduce. F4 adds a
marker to the logs. Take a separate screenshot if useful.

To identify the executable in PowerShell:

```powershell
Get-FileHash -Algorithm SHA256 .\TooieRecompiled.exe
```

The launcher's About page also shows the version. Review logs before sharing:
they can include usernames and local paths. Never
upload a ROM, saves, generated game files, an entire profile, or unreviewed
logs/captures.
