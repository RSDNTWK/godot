# Third party libraries

Please keep categories (`##` level) listed alphabetically and matching their
respective folder names. Use two empty lines to separate categories for
readability.


## accesskit

- Upstream: https://github.com/AccessKit/accesskit-c
- Version: 0.23.1 (tag 0.23.1, 2026)
- License: MIT

Files extracted from upstream source:

- `accesskit.h`
- `LICENSE-MIT`


## amd-fsr

- Upstream: https://github.com/GPUOpen-Effects/FidelityFX-FSR
- Version: 1.0.2 (a21ffb8f6c13233ba336352bdff293894c706575, 2021)
- License: MIT

Files extracted from upstream source:

- `ffx_a.h` and `ffx_fsr1.h` from `ffx-fsr`
- `license.txt`


## amd-fsr2

- Upstream: https://github.com/GPUOpen-Effects/FidelityFX-FSR2
- Version: 2.2.1 (1680d1edd5c034f88ebbbb793d8b88f8842cf804, 2023)
- License: MIT

Files extracted from upstream source:

- `ffx_*.cpp` and `ffx_*.h` from `src/ffx-fsr2-api`
- `shaders` folder from `src/ffx-fsr2-api` with `ffx_*.hlsl` files excluded
- `LICENSE.txt`

Patches:

- `0001-build-fixes.patch` ([GH-81197](https://github.com/godotengine/godot/pull/81197))
- `0002-godot-fsr2-options.patch` ([GH-81197](https://github.com/godotengine/godot/pull/81197))


## angle

Windows static package: chromium/8118, from the official Godot ANGLE release
(2026-10-23). The generic EGL/GLES header subset below retains Godot upstream's existing
pin. `misc/scripts/install_angle.py` selects the current package.

- Upstream: https://chromium.googlesource.com/angle/angle/
- Version: git (chromium/5907, 430a4f559cbc2bcd5d026e8b36ee46ddd80e9651, 2023)
- License: BSD-3-Clause

Files extracted from upstream source:

- `include/*`
- `LICENSE`


## astcenc

- Upstream: https://github.com/ARM-software/astc-encoder
- Version: 5.7.0 (tag 5.7.0, 2026)
- License: Apache 2.0

Files extracted from upstream source:

- `astcenc_*` and `astcenc.h` files from `Source`
- `LICENSE.txt`

Godot builds these sources through the generated wrappers in `modules/astcenc`.
They isolate private C++ symbols in `godot_astcenc` and prefix the public C API
to avoid collisions with ANGLE's embedded ASTC implementation. Keep the system
header pre-includes in `astcenc_godot.h` aligned with upstream source updates.

## basis_universal

- Upstream: https://github.com/BinomialLLC/basis_universal
- Version: 2.50 (tag v2_50, 2026)
- License: Apache 2.0

Files extracted from upstream source:

- `encoder/` and `transcoder/` folders, with the following files removed from `encoder`:
  `3rdparty/{qoi.h,tinydds.h,tinyexr.cpp,tinyexr.h}`
- `LICENSE`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## brotli

- Upstream: https://github.com/google/brotli
- Version: 1.2.1 (tag v1.2.1, 2026)
- License: MIT

Files extracted from upstream source:

- `common/`, `dec/` and `include/` folders from `c/`,
  minus the `dictionary.bin*` files
- `LICENSE`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## certs

- Upstream: Mozilla, via https://github.com/bagder/ca-bundle
- Version: git (c4d4f0b9ed23c6ef1d19ad2565598cf0c2b24112, 2026)
- License: MPL 2.0

Files extracted from upstream source:

- `ca-bundle.crt`


## clipper2

- Upstream: https://github.com/AngusJohnson/Clipper2
- Version: 2.0.1 (tag Clipper2_2.0.1, 2026)
- License: BSL 1.0

Files extracted from upstream source:

- `CPP/Clipper2Lib/` folder (in root)
- `LICENSE`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## cvtt

- Upstream: https://github.com/elasota/ConvectionKernels
- Version: git (350416daa4e98f1c17ffc273b134d0120a2ef230, 2022)
- License: MIT

Files extracted from upstream source:

- All `.cpp` and `.h` files except the folders `MakeTables` and `etc2packer`
- `LICENSE.txt`

Patches:

- `0001-revert-bc6h-reorg.patch` ([GH-73715](https://github.com/godotengine/godot/pull/73715))


## d3d12ma

- Upstream: https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator
- Version: 3.2.0 (tag v3.2.0, 2026)
- License: MIT

Files extracted from upstream source:

- `src/D3D12MemAlloc.cpp`, `src/D3D12MemAlloc.natvis`
- `include/D3D12MemAlloc.h`
- `LICENSE.txt`, `NOTICES.txt`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## directx_headers

- Upstream: https://github.com/microsoft/DirectX-Headers
- Version: main (25411c74bb9cc7c416b2ff01b3ad8a306811dfdd, 2025)
- License: MIT

Files extracted from upstream source:

- `include/directx/*.h`
- `include/dxguids/*.h`
- `LICENSE`

Patches:

- `0001-win7-8-dynamic-load.patch` ([GH-88496](https://github.com/godotengine/godot/pull/88496))


## doctest

- Upstream: https://github.com/onqtam/doctest
- Version: 2.5.3 (tag v2.5.3, 2026)
- License: MIT

Files extracted from upstream source:

- `doctest/doctest.h` as `doctest.h`
- `LICENSE.txt`


## dr_libs

- Upstream: https://github.com/mackron/dr_libs
- Version: mp3-0.7.3 (5690d4671d7ad07ae6021756d7222eb159745f06, 2026)
- License: Public Domain or Unlicense or MIT-0

Files extracted from upstream source:

- `dr_mp3.h`
- `LICENSE`

`dr_bridge.h` is a Godot file and should be preserved on updates.


## embree

- Upstream: https://github.com/embree/embree
- Version: 4.4.1 (tag v4.4.1, 2026)
- License: Apache 2.0

Files extracted from upstream:

- All `.cpp` files listed in `modules/raycast/godot_update_embree.py`
- All header files in the directories listed in `modules/raycast/godot_update_embree.py`
- All config files listed in `modules/raycast/godot_update_embree.py`
- `LICENSE.txt`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## enet

- Upstream: https://github.com/lsalzman/enet
- Version: 1.3.18 (2662c0de09e36f2a2030ccc2c528a3e4c9e8138a, 2024)
- License: MIT

Files extracted from upstream source:

- All `.c` files in the main directory (except `unix.c` and `win32.c`)
- The `include/enet/` folder as `enet/` (except `unix.h` and `win32.h`)
- `LICENSE` file
- Added 3 files `enet_godot.cpp`, `enet/enet_godot.h`, and `enet/enet_godot_ext.h`,
  providing ENet socket implementation using Godot classes, allowing IPv6 and DTLS.

Patches:

- `0001-godot-socket.patch` ([GH-7985](https://github.com/godotengine/godot/pull/7985))

Important: Building against a system wide ENet is possible, but will limit its
functionality to IPv4 only and no DTLS. We recommend against it.


## etcpak

- Upstream: https://github.com/wolfpld/etcpak
- Version: 2.1 (tag 2.1, 2026)
- License: BSD-3-Clause

Files extracted from upstream source:

- Only the files relevant for compression (i.e. `Process*.cpp` and their deps):
  ```
  Dither.{cpp,hpp} ForceInline.hpp Math.hpp ProcessCommon.hpp ProcessRGB.{cpp,hpp}
  ProcessDxtc.{cpp,hpp} Tables.{cpp,hpp} Vector.hpp
  ```
- The files `DecodeRGB.{cpp.hpp}` are based on the code from the original repository.
- `AUTHORS.txt` and `LICENSE.txt`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## fonts

- `DroidSans*.woff2`:
  * Upstream: https://android.googlesource.com/platform/frameworks/base/+/master/data/fonts/
  * Version: ? (pre-2014 commit when DroidSansJapanese.ttf was obsoleted)
  * License: Apache 2.0
- `Inter*.woff2`:
  * Upstream: https://github.com/rsms/inter
  * Version: v4.1 (e3a3d4c57d5ecc01453a575621882a384c1995a3, 2024)
  * License: OFL-1.1
- `JetBrainsMono_Regular.woff2`:
  * Upstream: https://github.com/JetBrains/JetBrainsMono
  * Version: 2.304 (cd5227bd1f61dff3bbd6c814ceaf7ffd95e947d9, 2023)
  * License: OFL-1.1
- `NotoSansBengali*.woff2`:
  * Upstream: https://github.com/notofonts/bengali
  * Version: 3.011 (85d80394cbbbb798ca0a41c983902e6cf77be3a3, 2026)
  * License: OFL-1.1
- `NotoSansDevanagari*.woff2`:
  * Upstream: https://github.com/notofonts/devanagari
  * Version: 2.006 (bb8d2566a1708ef2dcc6396ee2eb261a18967f76, 2024)
  * License: OFL-1.1
- `NotoSansGeorgian*.woff2`:
  * Upstream: https://github.com/notofonts/georgian
  * Version: 2.005 (c02e5483c2dd63c5cf223845010ebd6e6dc56aec, 2024)
  * License: OFL-1.1
- `NotoSansHebrew*.woff2`:
  * Upstream: https://github.com/notofonts/hebrew
  * Version: 3.001 (caa7ab0614fb5b37cc003d9bf3d7d3e765331110, 2024)
  * License: OFL-1.1
- `NotoSansMalayalam*.woff2`:
  * Upstream: https://github.com/notofonts/malayalam
  * Version: 2.104 (0fd65e553a6af3dc1c09ed39dfe8933e01c17b32, 2023)
  * License: OFL-1.1
- `NotoSansOriya*.woff2`:
  * Upstream: https://github.com/notofonts/oriya
  * Version: 2.006 (97abab82ec512f8a4a98c389352f194a03385ce2, 2024)
  * License: OFL-1.1
- `NotoSansSinhala*.woff2`:
  * Upstream: https://github.com/notofonts/sinhala
  * Version: 3.000 (032355e96de5bac83fd996535af3d13b1fbfeccf, 2025)
  * License: OFL-1.1
- `NotoSansTamil*.woff2`:
  * Upstream: https://github.com/notofonts/tamil
  * Version: 2.004 (f34a08d1ae3fa810581f63410296d971bdcd62dc, 2023)
  * License: OFL-1.1
- `NotoSansTelugu*.woff2`:
  * Upstream: https://github.com/notofonts/telugu
  * Version: 2.005 (e97c3409a8347d68cccd06a82a68b418c315ee0c, 2023)
  * License: OFL-1.1
- `NotoSansThai*.woff2`:
  * Upstream: https://github.com/notofonts/thai
  * Version: 2.002 (f8b482c158650260bba5d5edba9da3e8bb7185b4, 2023)
  * License: OFL-1.1
- `OpenSans_SemiBold.woff2`:
  * Upstream: https://github.com/googlefonts/opensans
  * Version: git (bd7e37632246368c60fdcbd374dbf9bad11969b6, 2023)
  * License: OFL-1.1
- `Vazirmatn*.woff2`:
  * Upstream: https://github.com/rastikerdar/vazirmatn
  * Version: 33.003 (83629f877e8f084cc07b47030b5d3a0ff06c76ec, 2022)
  * License: OFL-1.1

All fonts are converted from the unhinted `.ttf` sources using the
`https://github.com/google/woff2` tool.

Use UI font variant if available, because it has tight vertical metrics and good
for UI.


## freetype

- Upstream: https://gitlab.freedesktop.org/freetype/freetype
- Version: 2.15.0 (tag VER-2-15-0, 2026)
- License: FreeType License (BSD-like)

Files extracted from upstream source:

- `src/` folder, minus the `dlg` and `tools` subfolders
  * These files can be removed: `.dat`, `.diff`, `.mk`, `.rc`, `README*`
  * In `src/gzip/`, keep only `ftgzip.c`
- `include/` folder, minus the `dlg` subfolder
- `LICENSE.TXT` and `docs/FTL.TXT`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.

The optional Apple HVF driver is not registered because this build does not
compile it. FreeType and its text-server consumers explicitly track the
macro-selected FreeType headers in SCons so incremental builds invalidate the
affected objects when those headers change.

## gamepadmotionhelpers

- Upstream: https://github.com/JibbSmart/GamepadMotionHelpers
- Version: git (f1a76007ec058122a4bb6f19c816c6c0c7b7f12a, 2026)
- License: MIT

Files extracted from upstream source:

- `GamepadMotion.hpp`
- `LICENSE.TXT`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## glad

- Upstream: https://github.com/Dav1dde/glad
- Version: 2.0.8 (73db193f853e2ee079bf3ca8a64aa2eaf6459043, 2024)
- License: CC0 1.0 and Apache 2.0

Files extracted from upstream source:
- `LICENSE`

Files generated from [upstream web instance](https://gen.glad.sh/):
- `EGL/eglplatform.h`
- `KHR/khrplatform.h`
- `egl.c`
- `glad/egl.h`
- `gl.c`
- `glad/gl.h`
- `glx.c`
- `glad/glx.h`

See the permalinks in `glad/egl.h`, `glad/gl.h` and `glad/glx.h`
to regenerate the files with a new version of the web instance.

Patches:

- `0001-enable-both-gl-and-gles.patch` ([GH-72831](https://github.com/godotengine/godot/pull/72831))
- `0002-revert-egl_static-removal.patch` ([GH-107312](https://github.com/godotengine/godot/pull/107312))


## glslang

- Upstream: https://github.com/KhronosGroup/glslang
- Version: vulkan-sdk-1.4.363.0 (tag vulkan-sdk-1.4.363.0, 2026)
- License: glslang

Version should be kept in sync with the one of the used Vulkan SDK (see `vulkan`
section).

Files extracted from upstream source:

- `glslang/` folder (except the `glslang/HLSL` and `glslang/ExtensionHeaders`
  subfolders), `SPIRV/` folder
  * Remove C interface code: `CInterface/` folders, files matching `"*_c[_\.]*"`
  * Remove `glslang/stub.cpp`
  * Remove `SPIRV/spirv.hpp11` (should use copy from `thirdparty/spirv-headers`)
- Run `cmake . && make` and copy generated `include/glslang/build_info.h`
  to `glslang/build_info.h`
- `LICENSE.txt`
- Unnecessary files like `CMakeLists.txt` or `updateGrammar` removed

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## graphite

- Upstream: https://github.com/silnrsi/graphite
- Version: 1.3.15 (tag 1.3.15, 2026)
- License: MIT

Files extracted from upstream source:

- The `include` folder
- The `src` folder (minus `CMakeLists.txt` and `files.mk`)
- `COPYING`


## grisu2

Godot's compatibility wrapper retains the previous bounded number formatter and
`grisu2::to_chars` API. Digit generation now uses Dragonbox 1.1.3, for both float
and double. Existing integer, signed-zero and exponent spelling is preserved.

- Upstream formatter origin: https://github.com/simdjson/simdjson
- Version: Godot formatter wrapper (Dragonbox migration, 2026)
- License: MIT / Apache 2.0 (retained formatter); see `dragonbox` for digit generation.

The old Grisu2 algorithm and its obsolete patch were removed.


## harfbuzz

- Upstream: https://github.com/harfbuzz/harfbuzz
- Version: 15.0.0 (tag 15.0.0, 2026)
- License: MIT

Files extracted from upstream source:

- `AUTHORS`, `COPYING`, `THANKS`
- From the `src` folder, recursively:
  - All the `.cc`, `.h`, `.hh` files
  - Except `main.cc`, `harfbuzz*.cc`, `harfrust.cc`, `failing-alloc.c`, `test*.cc`, `hb-gpu*.*`, `hb-wasm*.*`, `hb-harfrust.cc`, `wasm/*`, `ms-use/*`, `rust/*`


## icu4c

- Upstream: https://github.com/unicode-org/icu
- Version: 79.1 (ddc6b66926bfac6036144ca9902992a0d6d50af9, 2026)
- License: Unicode

Files extracted from upstream source:

- The `common` folder
- `scriptset.*`, `ucln_in.*`, `uspoof.cpp` and `uspoof_impl.*` from the `i18n` folder
- `uspoof.h` from the `i18n/unicode` folder
- `LICENSE`

Files generated from upstream source:

- The `icudt_godot.dat` built with the provided `godot_data.json` config file (see
  https://github.com/unicode-org/icu/blob/master/docs/userguide/icu_data/buildtool.md
  for instructions).

1. Download and extract both `icu4c-{version}-src.tgz` and `icu4c-{version}-data.zip`
  (replace `data` subfolder from the main source archive)
2. Build ICU with default options: `./runConfigureICU {PLATFORM} && make`
3. Reconfigure ICU with custom data config:
   `ICU_DATA_FILTER_FILE={GODOT_SOURCE}/thirdparty/icu4c/godot_data.json ./runConfigureICU {PLATFORM} --with-data-packaging=common`
4. Delete `data/out` folder and rebuild data: `cd data && rm -rf ./out && make`
5. Copy `source/data/out/icudt{ICU_VERSION}l.dat` to the `{GODOT_SOURCE}/thirdparty/icu4c/icudt_godot.dat`

Patches:

- `0001-ustr-literals.patch` ([GH-124792](https://github.com/godotengine/godot/pull/124792))
- `0002-tuple-include.patch` ([GH-124792](https://github.com/godotengine/godot/pull/124792))


## jolt_physics

- Upstream: https://github.com/jrouwe/JoltPhysics
- Version: 5.6.0 (e77f175595e64cb44218cc9d9d56fc365ad0e36a, 2026)
- License: MIT

Files extracted from upstream source:

- All files in `Jolt/`, except `Jolt/Jolt.cmake`, any files dependent on `ENABLE_OBJECT_STREAM` as seen in `Jolt/Jolt.cmake` and the `Jolt/Physics/Hair/`, `Jolt/Compute/` and `Jolt/Shaders/` folders.
- `LICENSE`

Patches:

- `0001-backport-upstream-commit-63f2f57.patch` (GH-121974)


## libbacktrace

- Upstream: https://github.com/ianlancetaylor/libbacktrace
- Version: git (0b9b49cf4a2c9229fc052d6716e1528b2f23e91a, 2026)
- License: BSD-3-Clause

Files extracted from upstream source:

- `*.{c,h}` files for Windows platform, i.e. remove the following:
  * `allocfail.c`, `instrumented_alloc.c`, `*test*.{c,h}`
  * `elf.c`, `macho.c`, `mmap.c`, `mmapio.c`, `nounwind.c`, `unknown.c`, `xcoff.c`
- `LICENSE`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## libjpeg-turbo

- Upstream: https://github.com/libjpeg-turbo/libjpeg-turbo
- Version: 3.2.0 (tag 3.2.0, 2026)
- License: BSD-3-Clause and IJG

Files extracted from upstream source:

- `src/*.{c,h}` except for:
  * `cdjpeg.c cjpeg.c djpeg.c example.c jcdiffct.c jclhuff.c jclossls.c jcstest.c jddiffct.c jdlhuff.c jdlossls.c jlossls.h jpegtran.c rdbmp.c rdcolmap.c rdgif.c rdjpgcom.c rdppm.c rdswitch.c rdtarga.c strtest.c tjbench.c tjcomp.c tjdecomp.c tjtran.c tjunittest.c tjutil.c wrbmp.c wrgif.c wrjpgcom.c wrppm.c wrtarga.c`
- `LICENSE.md`
- `README.ijg`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## libktx

- Upstream: https://github.com/KhronosGroup/KTX-Software
- Version: 4.4.2 (4d6fc70eaf62ad0558e63e8d97eb9766118327a6, 2025)
- License: Apache 2.0

Files extracted from upstream source:

- `LICENSE.md`
- `include/` minus `.clang-format`
- `external/dfdutils/LICENSE.adoc` as `LICENSE.dfdutils.adoc` (in root)
- `external/dfdutils/LICENSES/Apache-2.0.txt` as `Apache-2.0.txt` (in root)
- `external/dfdutils/{KHR/,dfd.h,colourspaces.c,createdfd.c,interpretdfd.c,printdfd.c,queries.c,dfd2vk.inl,vk2dfd.*}`
- `lib/{basis_sgd.h,formatsize.h,gl_format.h,ktxint.h,uthash.h,vk_format.h,vkformat_enum.h,checkheader.c,swap.c,hashlist.c,vkformat_check*.c,vkformat_typesize.c,basis_transcode.cpp,miniz_wrapper.cpp,filestream.*,memstream.*,texture*}`
- `other_include/KHR/`
- `utils/unused.h`

Patches:

- `0001-external-basisu.patch` ([GH-76572](https://github.com/godotengine/godot/pull/76572))
- `0002-disable-astc-block-ext.patch` ([GH-76572](https://github.com/godotengine/godot/pull/76572))
- `0003-basisu-1.60.patch` ([GH-103968](https://github.com/godotengine/godot/pull/103968))
- `0004-basis-2.50-compat.patch`: Basis 2.50 texture format enum compatibility.


## libogg

- Upstream: https://www.xiph.org/ogg
- Version: 1.3.6 (be05b13e98b048f0b5a0f5fa8ce514d56db5f822, 2025)
- License: BSD-3-Clause

Files extracted from upstream source:

- `src/*.{c,h}`
- `include/ogg/*.h` in `ogg/` (run `configure` to generate `config_types.h`)
- `COPYING`


## libpng

- Upstream: http://libpng.org/pub/png/libpng.html
- Version: 1.6.59 (tag v1.6.59, 2026)
- License: libpng/zlib

Files extracted from upstream source:

- All `.c` and `.h` files of the main directory, apart from `example.c` and `pngtest.c`
- `arm/`, `intel/`, `loongarch/`, and `powerpc/` folders, except `arm/filter_neon.S` and `.editorconfig` files
- `scripts/pnglibconf.h.prebuilt` as `pnglibconf.h`
- `LICENSE`


## libtheora

- Upstream: https://www.theora.org
- Version: 1.2.0 (8e4808736e9c181b971306cc3f05df9e61354004, 2025)
- License: BSD-3-Clause

Files extracted from upstream source:

- All `.c` and `.h` files in `lib/`, except `arm/` and `c64x/` folders
- All `.h` files in `include/theora/` as `theora/`
- `COPYING` and `LICENSE`


## libvorbis

- Upstream: https://www.xiph.org/vorbis
- Version: 1.3.7 (0657aee69dec8508a0011f47f3b69d7538e9d262, 2020)
- License: BSD-3-Clause

Files extracted from upstream source:

- `lib/*` except from: `lookups.pl`, `Makefile.*`
- `include/vorbis/*.h` as `vorbis/`
- `COPYING`


## libwebp

- Upstream: https://chromium.googlesource.com/webm/libwebp/
- Version: 1.6.0 (4fa21912338357f89e4fd51cf2368325b59e9bd9, 2025)
- License: BSD-3-Clause

Files extracted from upstream source:

- `src/` and `sharpyuv/` except from `.am`, `.rc` and `.in` files
- `AUTHORS`, `COPYING`, `PATENTS`

Patches:

- `0001-msvc-node-debug-rename.patch` ([GH-75769](https://github.com/godotengine/godot/pull/75769))
- `0002-msvc-arm64-fpstrict.patch` ([GH-94655](https://github.com/godotengine/godot/pull/94655))
- `0003-clang-cl-sse2-sse41-avx2.patch` ([GH-92316](https://github.com/godotengine/godot/pull/92316))


## linuxbsd_headers

See `linuxbsd_headers/README.md`.


## lzma2605

- Upstream: https://www.7-zip.org/sdk.html
- Version: 26.05 (2026)
- License: Public Domain

Files extracted from upstream source:

- From the SDK root to `thirdparty/lzma2605/`:
  - Keep only `C/` and `DOC/`
  - Except top-level folders `Asm/`, `CPP/`, `CS/`, `Java/`
- From `C/` to `thirdparty/lzma2605/C/`:
  - `7zAlloc.c`, `7zAlloc.h`, `7zStream.c`, `7zTypes.h`, `7zWindows.h`,
    `Compiler.h`, `CpuArch.c`, `CpuArch.h`, `LzFind.c`, `LzFind.h`,
    `LzFindMt.c`, `LzFindMt.h`, `LzFindOpt.c`, `LzHash.h`,
    `Lzma2Dec.c`, `Lzma2Dec.h`, `Lzma2Enc.c`, `Lzma2Enc.h`,
    `LzmaDec.c`, `LzmaDec.h`, `LzmaEnc.c`, `LzmaEnc.h`, `MtCoder.c`,
    `MtCoder.h`, `MtDec.c`, `MtDec.h`, `Precomp.h`, `Threads.c`,
    `Threads.h`
- From `DOC/` to `thirdparty/lzma2605/DOC/`:
  - `lzma-sdk.txt`


## manifold

- Upstream: https://github.com/elalish/manifold
- Version: 3.5.4 (tag v3.5.4, 2026)
- License: Apache 2.0

Patches:

- `0001-explicit-stream-declarations.patch`: include the standard stream forward declarations explicitly for libc++ builds.

File extracted from upstream source:

- `src/` and `include/`, except from `CMakeLists.txt`, `cross_section.h` and `meshIO.{cpp,h}`
- `AUTHORS`, `LICENSE`


## mbedtls

- Upstream: https://github.com/Mbed-TLS/mbedtls
- Version: 3.6.7 (068ff080b369adfac81509f9b57b2afabaf82dc5, 2026)
- License: Apache 2.0

File extracted from upstream release tarball:

- All `.h` from `include/mbedtls/` to `thirdparty/mbedtls/include/mbedtls/`
  and all `.h` from `include/psa/` to `thirdparty/mbedtls/include/psa/`
- From `library/` to `thirdparty/mbedtls/library/`:
  - All `.c` and `.h` files
  - Except `bignum_mod.c`, `block_cipher.c`, `ecp_curves_new.c`, `lmots.c`,
    `lms.c`
- The `LICENSE` file (edited to keep only the Apache 2.0 variant)
- Added 2 files `godot_core_mbedtls_platform.c` and `godot_core_mbedtls_config.h`
  providing configuration for light bundling with core
- Added 2 files `godot_module_mbedtls_config.h` and `threading_alt.h`
  to customize the build configuration when bundling the full library

Patches:

- `0001-msvc-2019-psa-redeclaration.patch` ([GH-90535](https://github.com/godotengine/godot/pull/90535))


## metal-cpp

- Upstream: https://developer.apple.com/metal/cpp/
- Version: 26.0 (2025)
- License: Apache 2.0

Update instructions:

- Download latest metal-cpp ZIP from https://developer.apple.com/metal/cpp/:
- Run `update-metal-cpp.sh <path to the downloaded zip>` to extract the relevant files and apply patches.


## meshoptimizer

- Upstream: https://github.com/zeux/meshoptimizer
- Version: 1.3 (tag v1.3, 2026)
- License: MIT

Files extracted from upstream repository:

- All files in `src/`
- `LICENSE.md`


## mingw-std-threads

- Upstream: https://github.com/meganz/mingw-std-threads
- Version: git (c931bac289dd431f1dd30fc4a5d1a7be36668073, 2023)
- License: BSD-2-clause

Files extracted from upstream repository:

- `LICENSE`
- `mingw.condition_variable.h`
- `mingw.invoke.h`
- `mingw.mutex.h`
- `mingw.shared_mutex.h`
- `mingw.thread.h`

Patches:

- `0001-disable-exceptions.patch` ([GH-85039](https://github.com/godotengine/godot/pull/85039))
- `0002-clang-std-replacements-leak.patch` ([GH-85208](https://github.com/godotengine/godot/pull/85208))
- `0003-explicit-includes.patch` ([GH-111557](https://github.com/godotengine/godot/pull/111557))


## miniupnpc

- Upstream: https://github.com/miniupnp/miniupnp
- Version: 2.3.3 (bf4215a7574f88aa55859db9db00e3ae58cf42d6, 2025)
- License: BSD-3-Clause

Files extracted from upstream source:

- `miniupnpc/src/` as `src/`
- `miniupnpc/include/` as `include/miniupnpc/`
- Remove the following test or sample files:
  `listdevices.c,minihttptestserver.c,miniupnpcmodule.c,upnpc.c,upnperrors.*,test*`
- `LICENSE`
- `src/miniupnpcstrings.h` was created manually for Godot (it is usually generated
  by CMake). Bump the version number for miniupnpc in that file when upgrading.


## minizip

- Upstream: https://github.com/madler/zlib
- Version: 1.3.2 (da607da739fa6047df13e66a2af6b8bec7c2a498, 2026)
- License: zlib

Files extracted from the upstream source:

- From `contrib/minizip`:
  `{crypt.h,ints.h,ioapi.{c,h},skipset.h,unzip.{c,h},zip.{c,h}}`
  `MiniZip64_info.txt`

Patches:

- `0001-godot-seek.patch` ([GH-10428](https://github.com/godotengine/godot/pull/10428))


## misc

Collection of single-file libraries used in Godot components.

- `bcdec.h`
  * Upstream: https://github.com/iOrange/bcdec
  * Version: snapshot (80859ed3b7afb1c527a2a99d70c61457bea72d0c, reviewed 2026-10-25)
  * License: MIT
- `cubemap_coeffs.h`
  * Upstream: https://research.activision.com/publications/archives/fast-filtering-of-reflection-probes
    File coeffs_const_8.txt (retrieved April 2020)
  * License: MIT
- `fastlz.{c,h}`
  * Upstream: https://github.com/ariya/FastLZ
  * Version: snapshot (b1342dabcf5257ab303743c9332fe75e9147a011, reviewed 2026-10-25)
  * License: MIT
- `FastNoiseLite.h`
  * Upstream: https://github.com/Auburn/FastNoiseLite
  * Version: snapshot (785f37a9ad76e283586a379675085f2063ae03f7, reviewed 2026-10-25)
  * License: MIT
  * Patches:
    - `FastNoiseLite-0001-namespace-warnings.patch` ([GH-88526](https://github.com/godotengine/godot/pull/88526))
- `ifaddrs-android.{cc,h}`
  * Upstream: https://chromium.googlesource.com/external/webrtc/stable/talk/+/master/base/ifaddrs-android.h
  * Version: git (5976650443d68ccfadf1dea24999ee459dd2819d, 2013)
  * License: BSD-3-Clause
  * Patches:
    - `ifaddrs-android-0001-complete-struct.patch` ([GH-34101](https://github.com/godotengine/godot/pull/34101))
- `mikktspace.{c,h}`
  * Upstream: https://archive.blender.org/wiki/index.php/Dev:Shading/Tangent_Space_Normal_Maps/
  * Version: 1.0 (2011)
  * License: zlib
- `nvapi_minimal.h`
  * Upstream: http://download.nvidia.com/XFree86/nvapi-open-source-sdk
  * Version: R525
  * License: MIT
  * Modifications: Created from upstream `nvapi.h` by removing unnecessary code.
- `ok_color.h`
  * Upstream: https://github.com/bottosson/bottosson.github.io/blob/master/misc/ok_color.h
  * Version: snapshot (7561fbab5c8b982020ed212aebb0b8620c44b228, reviewed 2026-10-25)
  * License: MIT
  * Modifications: License included in header.
- `ok_color_shader.h`
  * https://www.shadertoy.com/view/7sK3D1
  * Version: 2021-09-13
  * License: MIT
- `pcg.{cpp,h}`
  * Upstream: http://www.pcg-random.org
  * Version: minimal C implementation, http://www.pcg-random.org/download.html
  * License: Apache 2.0
- `polypartition.{cpp,h}`
  * Upstream: https://github.com/ivanfratric/polypartition (`src/polypartition.{cpp,h}`)
  * Version: snapshot (b000a4a2a72b46e1305fb6e95b080448d7c12049, reviewed 2026-10-25)
  * License: MIT
  * Patches:
    - `polypartition-0001-godot-integration.patch`: Godot types and warning fixes.
- `qoa.{c,h}`
  * Upstream: https://github.com/phoboslab/qoa
  * Version: snapshot (bc0589710ff6aa2f50e08fb4d0a815d567bc0f3e, reviewed 2026-10-25)
  * License: MIT
  * Modifications: Added implementation through `qoa.c`.
- `r128.{c,h}`
  * Upstream: https://github.com/fahickman/r128
  * Version: snapshot (69ba68f1ea666c9136cf56e0105b0cbbe9cd1534, reviewed 2026-10-25)
  * License: Public Domain or Unlicense
- `smaz.{c,h}`
  * Upstream: https://github.com/antirez/smaz
  * Version: snapshot (2f625846a775501fb69456567409a8b12f10ea25, reviewed 2026-10-25)
  * License: BSD-3-Clause
  * Modifications: License included in header.
  * Patches:
    - `smaz-0001-write-string-warning.patch` ([GH-8572](https://github.com/godotengine/godot/pull/8572))
- `smolv.{cpp,h}`
  * Upstream: https://github.com/aras-p/smol-v
  * Version: snapshot (55000efe742f56d8b51223b9ea7775a8f0501881, reviewed 2026-10-25)
  * License: Public Domain or MIT
- `stb_rect_pack.h`
  * Upstream: https://github.com/nothings/stb
  * Version: snapshot (003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5, reviewed 2026-10-25)
  * License: Public Domain or Unlicense or MIT
- `yuv2rgb.h`
  * Upstream: http://wss.co.uk/pinknoise/yuv2rgb/ (to check)
  * Version: ?
  * License: BSD


## msdfgen

- Upstream: https://github.com/Chlumsky/msdfgen
- Version: 1.13 (1874bcf7d9624ccc85b4bc9a85d78116f690f35b, 2025)
- License: MIT

Files extracted from the upstream source:

- `msdfgen.h`
- Files in `core/` folder, minus `export-svg.*` and `save-*.*` files
- `LICENSE.txt`

Patches:

- `0001-remove-unused-save-features.patch` ([GH-113965](https://github.com/godotengine/godot/issues/113965))


## openxr

- Upstream: https://github.com/KhronosGroup/OpenXR-SDK
- Version: 1.1.65 (tag release-1.1.65, 2026)
- License: Apache 2.0

Patches:

- `0001-no-exceptions-destructor-logging.patch`: honor `XRLOADER_DISABLE_EXCEPTION_HANDLING` in the two destructor logging blocks.

Files extracted from upstream source:

- `include/`
- `src/common/`
- `src/loader/`
- `src/*.{c,h}`
- `src/external/jsoncpp/include/`
- `src/external/jsoncpp/src/lib_json/`
- `src/external/jsoncpp/{AUTHORS,LICENSE}`
- `LICENSE` and `COPYING.adoc`

Exclude:

- `src/external/android-jni-wrappers` and `src/external/jnipp` (not used yet)
- Obsolete `src/xr_generated_dispatch_table.{c,h}`
- All CMake stuff: `cmake/`, `CMakeLists.txt` and `*.cmake`
- All Gradle stuff: `*gradle*`, `AndroidManifest.xml`
- All following files (and their `.license` files):
  `*.{def,expsym,in,json,map,pom,rc,txt}`
- All dotfiles

Additional:
- Update `openxrLoaderVersion` in `platform/android/java/app/config.gradle`


## pcre2

- Upstream: https://pcre2project.github.io/pcre2/
- Version: 10.49 (6f9d7c1373262c541324a16a358785b33ef116cf, 2026)
- License: BSD-3-Clause

Files extracted from upstream source:

- Files listed in the file `NON-AUTOTOOLS-BUILD` steps 1-4
- All `.h` files in `src/` apart from `pcre2posix.h`, `pcre2_printint_inc.h`, `pcre2test_inc.h`
- `src/pcre2_compile_cgroup.c`
- `src/pcre2_match_next.c`
- `src/pcre2_{jit_char,jit_match,jit_misc,jit_simd,ucptables}_inc.h`
- `deps/sljit/sljit_src`
- `AUTHORS.md` and `LICENCE.md`


## recastnavigation

- Upstream: https://github.com/recastnavigation/recastnavigation
- Version: 1.6.0 (6dc1667f580357e8a2154c28b7867bea7e8ad3a7, 2023)
- License: zlib

Files extracted from upstream source:

- `Recast/` folder without `CMakeLists.txt`
- `License.txt`


## re-spirv

- Upstream: https://github.com/renderbag/re-spirv
- Version: git (29a77fca357567d00aa37b8ffde19c19cfe477c4, 2026)
- License: MIT

Files extracted from upstream source:

- `re-spirv.cpp`
- `re-spirv.h`
- `LICENSE`


## rvo2

For 2D in `rvo2_2d` folder

- Upstream: https://github.com/snape/RVO2
- Version: git (f7c5380235f6c9ac8d19cbf71fc94e2d4758b0a3, 2021)
- License: Apache 2.0

For 3D in `rvo2_3d` folder

- Upstream: https://github.com/snape/RVO2-3D
- Version: git (bfc048670a4e85066e86a1f923d8ea92e3add3b2, 2021)
- License: Apache 2.0

Files extracted from upstream source:

- All `.cpp` and `.h` files in the `src/` folder except for `Export.h` and `RVO.h`
- `LICENSE`

Important: Nearly all files have Godot-made changes and renames
to make the 2D and 3D rvo libraries compatible with each other
and solve conflicts and also enrich the feature set originally
proposed by these libraries and better integrate them with Godot.


The Godot adaptations are retained after reviewing upstream RVO2
6b4eb00a93a9f3abf723235ff006d6aad3e2f212 and RVO2-3D
f940bfba8bac9532ecae948f11639085e8cbf277 (2026-10-25). The compared library
changes are API/refactoring and equivalent arithmetic; newer example/build
changes do not replace Godot's avoidance layers, priorities and integration.


## smaa

- Upstream: https://github.com/iryoku/smaa
- Version: git (71c806a838bdd7d517df19192a20f0c61b3ca29d, 2013)
- License: MIT

Files extracted from upstream source:

- `LICENSE`
- Textures generated using the Python scripts in the `Scripts` folder


## sdl

- Upstream: https://github.com/libsdl-org/SDL
- Version: 3.4.18 (tag release-3.4.18, 2026)
- License: Zlib
- Vendored: hidapi 0.14.0, license BSD-3-Clause

Files extracted from upstream source:

- See `thirdparty/sdl/update-sdl.sh`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## spirv-cross

- Upstream: https://github.com/KhronosGroup/SPIRV-Cross
- Version: vulkan-sdk-1.4.363.0 (tag vulkan-sdk-1.4.363.0, 2026)
- License: Apache 2.0

Files extracted from upstream source:

- All `.cpp`, `.hpp` and `.h` files, minus `main.cpp`, `spirv.h*`, `spirv_cross_c.*`, `spirv_hlsl.*`, `spirv_cpp.*`
- `include/` folder
- `LICENSE` and `LICENSES/` folder, minus `CC-BY-4.0.txt`

Versions of this SDK do not have to match the `vulkan` section, as this SDK is required
to generate Metal source from Vulkan SPIR-V.


## spirv-headers

- Upstream: https://github.com/KhronosGroup/SPIRV-Headers
- Version: vulkan-sdk-1.4.363.0 (tag vulkan-sdk-1.4.363.0, 2026)
- License: MIT

Files extracted from upstream source:

- `include/spirv/unified1/spirv.{h,hpp,hpp11}` with the same folder structure
- `LICENSE` (edited to keep only relevant license)

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## spirv-reflect

- Upstream: https://github.com/KhronosGroup/SPIRV-Reflect
- Version: vulkan-sdk-1.4.363.0 (tag vulkan-sdk-1.4.363.0, 2026)
- License: Apache 2.0

Version should be kept in sync with the one of the used Vulkan SDK (see `vulkan`
section).

Files extracted from upstream source:

- `spirv_reflect.h`, `spirv_reflect.c`
- `LICENSE`

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## swappy-frame-pacing

- Upstream: https://android.googlesource.com/platform/frameworks/opt/gamesdk/ via https://github.com/godotengine/godot-swappy
- Version: git (1198bb06b041e2df5d42cc5cf18fac81fcefa03f, 2025)
- License: Apache 2.0

Files extracted from upstream source:

- `include/common/`
- `include/swappy/{swappy_common.h,swappyVk.h}`
- `LICENSE`


## thorvg

- Upstream: https://github.com/thorvg/thorvg
- Version: 1.1.3 (589bc4120f2097fb1b135e552c2ef39f6581612b, 2026)
- License: MIT

Files extracted from upstream source:

- See `thorvg/update-thorvg.sh` for extraction instructions.
  Set the version number and run the script.

Patches:

- `0001-let-delete-be-delete.patch` ([GH-116024](https://github.com/godotengine/godot/pull/116024))


## tinyexr

- Upstream: https://github.com/syoyo/tinyexr
- Version: 3.2.0 (6f470c9ab24bf3992bc512ce07e8ecb00d9bf105, 2026)
- License: BSD-3-Clause

Files extracted from upstream source:

- `exr_reader.hh`
- `streamreader.hh`
- `tinyexr.{cc,h}`
- `LICENSE`

Patches:

- `0001-external-zlib.patch` ([GH-55115](https://github.com/godotengine/godot/pull/55115))
- `0002-fix-multipart-header-stale-size.patch` ([GH-124638](https://github.com/godotengine/godot/pull/124638))


## ufbx

- Upstream: https://github.com/ufbx/ufbx
- Version: 0.23.1 (tag v0.23.1, 2026)
- License: MIT

Files extracted from upstream source:

- `ufbx.{c,h}`
- `LICENSE`


## vhacd

- Upstream: https://github.com/kmammou/v-hacd
- Version: 4.1 snapshot (f900e42361491f525262d4825e758845e4969897)
- License: BSD-3-Clause

Files extracted from upstream source:

- `include/VHACD.h` as `public/VHACD.h`
- `LICENSE`
- Godot implementation translation unit `src/VHACD.cpp`.

The module disables internal threading for threadless builds. Existing Godot
settings remain serializable; settings removed in V-HACD 4 have no effect and
are documented in `MeshConvexDecompositionSettings`. Concavity is mapped to
percentage volume error and projected hull vertices to shrink wrapping. The
decomposition algorithm can produce different hulls from V-HACD 2.


## volk

- Upstream: https://github.com/zeux/volk
- Version: 1.4.350 (tag vulkan-sdk-1.4.363.0, 2026)
- License: MIT

Version should be kept in sync with the one of the used Vulkan SDK (see `vulkan`
section).

Files extracted from upstream source:

- `volk.h`, `volk.c`
- `LICENSE.md`


## vulkan

- Upstream: https://github.com/KhronosGroup/Vulkan-Headers
- Version: vulkan-sdk-1.4.363.0 (tag vulkan-sdk-1.4.363.0, 2026)
- License: Apache 2.0

Unless there is a specific reason to package a more recent version, please stick
to tagged SDK releases. All Vulkan libraries and headers should be kept in sync so:

- Update Vulkan SDK components to the matching tag (see "vulkan")
- Update volk (see "volk")
- Update glslang (see "glslang")
- Update spirv-headers (see "spriv-headers")
- Update spirv-cross (see "spirv-cross")
- Update spirv-reflect (see "spirv-reflect")

Files extracted from upstream source:

- `include/`
- `LICENSE.md`

`vk_enum_string_helper.h` is taken from the matching `Vulkan-Utility-Libraries`
SDK release: https://github.com/KhronosGroup/Vulkan-Utility-Libraries/blob/main/include/vulkan/vk_enum_string_helper.h

`vk_mem_alloc.h` is taken from https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
Version: 3.4.0 (tag v3.4.0, 2026)
`vk_mem_alloc.cpp` is a Godot file and should be preserved on updates.

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## wayland

- Upstream: https://gitlab.freedesktop.org/wayland/wayland
- Version: 1.26.0 (tag 1.26.0, 2026)
- License: MIT

Files extracted from upstream source:

- `protocol/wayland.xml`
- `COPYING`


## wayland-protocols

- Upstream: https://gitlab.freedesktop.org/wayland/wayland-protocols
- Version: 1.49 (2026)
- License: MIT

Files extracted from upstream source:

- XML protocols under `stable/`, `staging/` and `unstable/`, plus `COPYING`.
- Existing Godot protocol wrappers, README files and `mesa/wayland-drm.xml` are preserved.


## wslay

- Upstream: https://github.com/tatsuhiro-t/wslay
- Version: 1.1.1+git (0e7d106ff89ad6638090fd811a9b2e4c5dda8d40, 2022)
- License: MIT

File extracted from upstream release tarball:

- Run `cmake .` to generate `config.h` and `wslayver.h`
  Contents might need tweaking for Godot, review diff
- All `.c` and `.h` files from `lib/`
- All `.h` in `lib/includes/wslay/` as `wslay/`
- `COPYING`

Patches:

- `0001-msvc-build-fix.patch` ([GH-30263](https://github.com/godotengine/godot/pull/30263))


## xatlas

- Upstream: https://github.com/jpcy/xatlas
- Version: git (f700c7790aaa030e794b52ba7791a05c085faf0c, 2022)
- License: MIT

Files extracted from upstream source:

- `source/xatlas/xatlas.{cpp,h}`
- `LICENSE`


## zlib

- Upstream: https://github.com/madler/zlib
- Version: 1.3.2 (da607da739fa6047df13e66a2af6b8bec7c2a498, 2026)
- License: zlib

Files extracted from upstream source:

- All `.c` and `.h` files, except `gz*.c` and `infback.c`
- `LICENSE`


## zstd

- Upstream: https://github.com/facebook/zstd
- Version: 1.5.7 (f8745da6ff1ad1e7bab384bd1f9d742439278e99, 2025)
- License: BSD-3-Clause

Files extracted from upstream source:

- `lib/{common/,compress/,decompress/,zstd.h,zstd_errors.h}`
- `LICENSE`

## dragonbox

- Upstream: https://github.com/jk-jeon/dragonbox
- Version: 1.1.3 (tag 1.1.3)
- License: Boost Software License 1.0 (selected); Apache 2.0 with LLVM exceptions also provided upstream.

Files: `include/dragonbox/dragonbox.h` as `dragonbox.h` and both upstream licenses.
The Godot bounded formatting adapter is retained in `grisu2/grisu2.h`.


## libvpx

- Upstream: https://github.com/webmproject/libvpx
- Version: 1.17.0 (tag v1.17.0, 2026)
- License: BSD-3-Clause and patent grant; see `LICENSE` and `PATENTS`.

Legacy WebM decoder sources, used when FFmpeg is disabled. The existing Godot
platform configuration and runtime CPU dispatch integration are retained.

Patches:

- `0001-godot-integration.patch`: local integration changes rebased on the version above.


## libsimplewebm

- Upstream parser: https://github.com/webmproject/libwebm
- Version: libwebm-1.0.0.32, with the existing Godot libsimplewebm adapter.
- License: BSD-3-Clause and patent grant; see `libwebm/LICENSE.TXT` and `PATENTS.TXT`.

The parser release delta was merged using 1.0.0.28 as the closest known lineage;
the original snapshot revision has not been proven. Local parser guards and
the public reader destructor remain. Godot's decoder/demuxer wrapper is retained.


## opus

- Upstream: https://github.com/xiph/opus
- Version: 1.6.1 (retained release source verified 2026-10-25)
- License: BSD-3-Clause

Retained source: `build/media-stable-20261005/opus-1.6.1`. 304 C/H files match;
local quantization/table adjustments and the Android API 21 integration remain.


## ffmpeg-build

- Upstream: https://ffmpeg.org and the component repositories recorded below.
- Version: FFmpeg 9.0.2, dav1d 1.5.4 (six active packages)
- License: See each retained build manifest and FFmpeg configuration.

The six active FFmpeg packages use FFmpeg 9.0.2 and dav1d 1.5.4. Windows also
records AMF 1.5.3. Retained oneVPL 2.17.0 static archives match the active Windows
and Linux dependency archives byte-for-byte. NVIDIA n13.1.15.0 stable headers
match the retained build snapshot's headers; its later package version bump
does not change header code. The correct FFmpeg release source is retained at
`build/media-stable-20261005/ffmpeg-9.0.2`; the separate development checkout
must not be reset or mistaken for the active release source.

Evidence and exact package hashes are recorded in
`HANDOVER_2026-10-25_component_updates.txt` and the linked component-update
checkpoint. Unchanged codec packages were preserved rather than rebuilt.
