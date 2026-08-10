
# libprojectM (bundled)

This directory contains the pre-built binaries (Windows x64 + macOS Universal)
and public C headers of [libprojectM 4](https://github.com/projectM-visualizer/projectm),
used by the `MilkdropModule` in Y2KMeter to render Milkdrop-style audio visualizations.

## Contents

| Path | Origin |
|---|---|
| `bin/projectM-4.dll` | Extracted from [`projectMSDL-2.0.0-win64.zip`](https://github.com/projectM-visualizer/frontend-sdl-cpp/releases/tag/2.0.0-pre1) (upstream projectM master ~= 4.1.x). |
| `bin/projectM-4-playlist.dll` | Same source. Currently unused by Y2KMeter (we walk the preset folder ourselves), kept for future use. |
| `bin/glew32.dll` | Required by `projectM-4.dll` at load time. |
| `bin/macos/libprojectM-4.dylib` | Built from [projectM v4.1.3](https://github.com/projectM-visualizer/projectm/releases/tag/v4.1.3) source as a Universal Binary (x86_64 + arm64). Only depends on system OpenGL / CoreFoundation / libc++. |
| `bin/macos/libprojectM-4.4.dylib` | Symlink / copy of the above (soname alias). |
| `bin/macos/libprojectM-4.4.1.3.dylib` | Full-version copy of the above. |
| `include/projectM-4/*.h` | Copied verbatim from `projectm` master `src/api/include/projectM-4/` at the same revision. |
| `include/projectM-4/projectM_export.h` | Hand-written stub replacing the CMake-generated export macros (see comment in file). |
| `include/projectM-4/version.h` | Hand-written to match `version.h.in`. |

## Linking model

Y2KMeter does **not** link against `projectM-4.lib` / `libprojectM-4.dylib` at
build time. Instead the library is loaded at runtime via platform-specific APIs:

- **Windows**: `LoadLibraryW()` + `GetProcAddress()`
- **macOS**: `dlopen()` + `dlsym()`

See `source/ui/modules/ProjectMApi.{h,cpp}` for details. This keeps the build
toolchain free of any link-time dependency on projectM and allows users to swap
in a newer/self-built library without rebuilding Y2KMeter.

## License

libprojectM is released under **LGPL-2.1-or-later**. See
[projectM/LICENSE.txt](https://github.com/projectM-visualizer/projectm/blob/master/LICENSE.txt).

Because Y2KMeter loads the library dynamically and only through the published C
API, LGPL requirements are satisfied by:

1. Keeping the shared library as a **separate, unmodified file** shipped alongside
   the application. Users can drop in a newer/self-built library (of compatible
   ABI) without rebuilding Y2KMeter.
2. Providing this notice, a link to the upstream sources, and the full license
   text (see [../../README.md](../../README.md) and `LICENSES/`).

## Refreshing the bundled version

### Windows binaries (DLLs)

```powershell
Invoke-WebRequest https://github.com/projectM-visualizer/frontend-sdl-cpp/releases/download/2.0.0-pre1/projectMSDL-2.0.0-win64.zip -OutFile projectMSDL.zip
Expand-Archive projectMSDL.zip -DestinationPath _tmp
Copy-Item _tmp\projectMSDL-2.0.0-win64\projectM-4.dll,_tmp\projectMSDL-2.0.0-win64\projectM-4-playlist.dll,_tmp\projectMSDL-2.0.0-win64\glew32.dll bin/
```

### macOS dylib (Universal Binary x86_64 + arm64)

Build from source (requires CMake + Xcode Command Line Tools):

```bash
# 1. Download projectM v4.1.3 source
curl -L https://github.com/projectM-visualizer/projectm/archive/refs/tags/v4.1.3.tar.gz -o /tmp/projectm-4.1.3.tar.gz
tar xzf /tmp/projectm-4.1.3.tar.gz -C /tmp

# 2. Download the projectm-eval submodule (required by the build)
curl -L https://github.com/projectM-visualizer/projectm-eval/archive/refs/heads/master.tar.gz -o /tmp/projectm-eval.tar.gz
tar xzf /tmp/projectm-eval.tar.gz -C /tmp/projectm-4.1.3/vendor/
mv /tmp/projectm-4.1.3/vendor/projectm-eval-master /tmp/projectm-4.1.3/vendor/projectm-eval

# 3. Configure and build as Universal Binary
cmake -S /tmp/projectm-4.1.3 -B /tmp/projectm-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_INSTALL_PREFIX=/tmp/projectm-install \
  -DENABLE_PLAYLIST=OFF \
  -DENABLE_TESTING=OFF \
  -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build /tmp/projectm-build --parallel 4
cmake --install /tmp/projectm-build --prefix /tmp/projectm-install

# 4. Copy into this repo
mkdir -p bin/macos
cp /tmp/projectm-install/lib/libprojectM-4.dylib     bin/macos/
cp /tmp/projectm-install/lib/libprojectM-4.4.dylib   bin/macos/
cp /tmp/projectm-install/lib/libprojectM-4.4.1.3.dylib bin/macos/
```

### Public headers (both platforms)

```bash
curl -L https://codeload.github.com/projectM-visualizer/projectm/zip/refs/heads/master -o projectm-src.zip
unzip -q projectm-src.zip
cp projectm-master/src/api/include/projectM-4/*.h include/projectM-4/
```
