# Third-party notices

Banjo-Tooie: Recompiled uses the projects below. Their copyright and license
terms remain with their authors. The [root GPL-3.0-only license](LICENSE)
applies only to material its copyright holders can license. Linked notices for
packaged components are also included in the Windows package.

## Game material

Banjo-Tooie belongs to its rights holders. The source repository has no game
ROM, extracted assets or generated game code. The Windows executable contains
translated game code and `runtime-data` contains generated metadata; players
supply their own ROM. This project grants no rights to the game and is not
affiliated with or endorsed by Nintendo or Rare.

## Native runtime and rendering

- [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime):
  [GPL-3.0](third_party/notices/N64ModernRuntime-GPL-3.0.txt). Bundled notices:
  [miniz](third_party/notices/miniz.txt), [o1heap](third_party/notices/o1heap-MIT.txt),
  [concurrentqueue](third_party/notices/concurrentqueue-BSD-header-notice.txt),
  [its semaphore](third_party/notices/concurrentqueue-Preshing-zlib-header-notice.txt),
  [nlohmann/json 3.9.1](third_party/notices/nlohmann-json-v3.9.1-MIT.txt).
- [N64Recomp](https://github.com/N64Recomp/N64Recomp):
  [MIT](third_party/notices/N64Recomp-MIT.txt). Related notices:
  [fmt](third_party/notices/fmt-MIT.txt), [rabbitizer](third_party/notices/rabbitizer-MIT.txt),
  [sljit](third_party/notices/sljit-BSD.txt).
- [RT64](https://github.com/rt64/rt64): [MIT](third_party/notices/RT64-MIT.txt).
  Bundled notices: [ddspp](third_party/notices/RT64-ddspp-MIT.txt),
  [hlslpp](third_party/notices/RT64-hlslpp.txt),
  [im3d](third_party/notices/RT64-im3d.txt),
  [Dear ImGui](third_party/notices/RT64-imgui.txt),
  [ImPlot](third_party/notices/RT64-implot.txt),
  [nativefiledialog-extended](third_party/notices/RT64-nfd-zlib.txt),
  [plume](third_party/notices/RT64-plume-MIT.txt),
  [D3D12 Memory Allocator](third_party/notices/RT64-plume-D3D12MA.txt),
  [Vulkan Memory Allocator](third_party/notices/RT64-plume-VMA.txt),
  [volk](third_party/notices/RT64-plume-volk.txt),
  [Vulkan Headers](third_party/notices/RT64-plume-Vulkan-Headers.txt),
  [re-spirv](third_party/notices/RT64-re-spirv-MIT.txt),
  [SPIRV-Cross](third_party/notices/RT64-spirv-cross-Apache-2.0.txt),
  [SPIRV-Headers](third_party/notices/RT64-spirv-headers.txt),
  [stb](third_party/notices/RT64-stb.txt),
  [UnicodeConversions](third_party/notices/RT64-utf8conv-MIT.txt),
  [xxHash](third_party/notices/RT64-xxHash-BSD.txt),
  [zstd](third_party/notices/RT64-zstd-BSD.txt),
  [nlohmann/json 3.12](third_party/notices/nlohmann-json-v3.12.0-MIT.txt).

The launcher and menus use Dear ImGui through RT64. The CIC algorithm in
`src/si_adapter.cpp` retains the
[X-Scale BSD-style notice](third_party/notices/X-Scale-CIC-BSD.txt).

## Windows libraries and packaged assets

- [SDL2](https://github.com/libsdl-org/SDL): [zlib notice](third_party/notices/SDL2-zlib.txt).
- [DirectX Shader Compiler v1.8.2502](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2502):
  [LLVM license](third_party/notices/DXC-v1.8.2502-LICENSE-LLVM.txt) and
  [release notes](third_party/notices/DXC-v1.8.2502-ReleaseNotes.md) from the
  official archive distributed with `dxcompiler.dll`.
- [SDL_GameControllerDB](https://github.com/gabomdq/SDL_GameControllerDB):
  [zlib-style notice](third_party/notices/SDL-GameControllerDB.txt).
  `recompcontrollerdb.txt` is a modified N64/GameCube-oriented subset.
- [Inter](https://github.com/rsms/inter) (`assets/InterVariable.ttf`):
  [SIL Open Font License 1.1](third_party/notices/SIL-OFL-1.1.txt) and
  [font copyrights](third_party/notices/FONT_COPYRIGHTS.txt).

## Additional source assets and credits

`assets/NotoEmoji-Regular.ttf` uses the same [SIL Open Font License
1.1](third_party/notices/SIL-OFL-1.1.txt); its copyrights are in
[FONT_COPYRIGHTS.txt](third_party/notices/FONT_COPYRIGHTS.txt).
[PromptFont](https://shinmera.com/promptfont) retains
`assets/promptfont/LICENSE.txt` and `assets/promptfont/README.md` beside the
font in the source repository. The project-authored `assets/icons/` are under the root
license. `assets/recomp.rcss` is an archival stylesheet based on
[RecompFrontend](https://github.com/N64Recomp/RecompFrontend) styling and retains
`third_party/notices/RmlUi-MIT.txt` in the source repository.
`patches/rt64/` and `patches/runtime/` identify upstream revisions in their
`PROVENANCE.md` files.

Local generation uses the
[Banjo-Tooie decompilation](https://github.com/Mr-Wiseguy/banjo-tooie), IDO 5.3,
and legacy GCC/binutils; `dependencies.lock.json` records downloaded inputs.
Earlier frontend work adapted code from
[BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp) and
[Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp).
`src/register_overlays.cpp` follows BanjoRecomp's overlay registration.
Their authors retain ownership; none of these projects endorse this project.
