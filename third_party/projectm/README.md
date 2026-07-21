
# libprojectM (bundled)

This directory contains the Windows x64 pre-built binaries and public C headers
of [libprojectM 4](https://github.com/projectM-visualizer/projectm), used by the
`MilkdropModule` in Y2KMeter to render Milkdrop-style audio visualizations.

## Contents

| Path | Origin |
|---|---|
| `bin/projectM-4.dll` | Extracted from [`projectMSDL-2.0.0-win64.zip`](https://github.com/projectM-visualizer/frontend-sdl-cpp/releases/tag/2.0.0-pre1) (upstream projectM master ~= 4.1.x). |
| `bin/projectM-4-playlist.dll` | Same source. Currently unused by Y2KMeter (we walk the preset folder ourselves), kept for future use. |
| `bin/glew32.dll` | Required by `projectM-4.dll` at load time. |
| `include/projectM-4/*.h` | Copied verbatim from `projectm` master `src/api/include/projectM-4/` at the same revision. |
| `include/projectM-4/projectM_export.h` | Hand-written stub replacing the CMake-generated export macros (see comment in file). |
| `include/projectM-4/version.h` | Hand-written to match `version.h.in`. |

## Linking model

Y2KMeter does **not** link against `projectM-4.lib` at build time (no `.lib`
files are shipped in the upstream Windows release). Instead the DLL is loaded at
runtime via `LoadLibraryW()` + `GetProcAddress()`; see
`source/ui/modules/ProjectMApi.{h,cpp}`. This keeps the toolchain fully MSVC-only
and avoids any `.def` / `dumpbin` round-trip.

## License

libprojectM is released under **LGPL-2.1-or-later**. See
[projectM/LICENSE.txt](https://github.com/projectM-visualizer/projectm/blob/master/LICENSE.txt).

Because Y2KMeter loads `projectM-4.dll` dynamically and only through the
published C API, LGPL requirements are satisfied by:

1. Keeping the DLL as a **separate, unmodified shared library** shipped alongside
   the application. Users can drop in a newer/self-built `projectM-4.dll` (of
   compatible ABI) without rebuilding Y2KMeter.
2. Providing this notice, a link to the upstream sources, and the full license
   text (see [../../README.md](../../README.md) and `LICENSES/`).

If you rebuild libprojectM yourself, keep the same DLL name (`projectM-4.dll`)
and API version and drop it into `bin/`.

## Refreshing the bundled version

```powershell
# Windows binaries (DLLs)
Invoke-WebRequest https://github.com/projectM-visualizer/frontend-sdl-cpp/releases/download/2.0.0-pre1/projectMSDL-2.0.0-win64.zip -OutFile projectMSDL.zip
Expand-Archive projectMSDL.zip -DestinationPath _tmp
Copy-Item _tmp\projectMSDL-2.0.0-win64\projectM-4.dll,_tmp\projectMSDL-2.0.0-win64\projectM-4-playlist.dll,_tmp\projectMSDL-2.0.0-win64\glew32.dll bin/

# Public headers
Invoke-WebRequest https://codeload.github.com/projectM-visualizer/projectm/zip/refs/heads/master -OutFile projectm-src.zip
Expand-Archive projectm-src.zip -DestinationPath _src
Copy-Item _src\projectm-master\src\api\include\projectM-4\*.h include\projectM-4\
```
