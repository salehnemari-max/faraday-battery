# Building Faraday

## Toolchain

| Component | Version used | Source |
|---|---|---|
| Qt (Quick, Controls Basic, Sql, Widgets, LinguistTools) | 6.8.2 `win64_mingw` | `aqtinstall` (free, no Qt account) |
| Compiler | MinGW-w64 GCC 13.1.0 (the Qt-matched build, `tools_mingw1310`) | `aqtinstall` |
| CMake | ≥ 3.21 (4.4.0 used) | winget `Kitware.CMake` |
| Ninja | 1.13 | winget `Ninja-build.Ninja` |
| NSIS | 3.x | winget `NSIS.NSIS` |
| SQLite | bundled inside Qt SQL (`qsqlite.dll`) — nothing extra to install | — |

MSVC works too (the CMakeLists carries `/W4 /WX` for it); MinGW was used for
the shipped build because a matched Qt+compiler pair is fetchable entirely
from free tooling. One-time setup:

```powershell
winget install Kitware.CMake Ninja-build.Ninja NSIS.NSIS Python.Python.3.12
python -m pip install aqtinstall
python -m aqt install-qt  windows desktop 6.8.2 win64_mingw   -O C:\Qt
python -m aqt install-tool windows desktop tools_mingw1310    -O C:\Qt
```

## Build & test

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Test
```

`build.ps1` configures with Ninja + the MinGW toolchain
(`CMAKE_PREFIX_PATH=C:\Qt\6.8.2\mingw_64`), builds, and runs the full ctest
suite (15 binaries covering acquisition, metrics, persistence, alerts,
calibration, exports and the synthetic degradation harness). Warnings are
errors (`-Wall -Wextra -Werror`).

Manual equivalent:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH=C:\Qt\6.8.2\mingw_64 `
      -DCMAKE_CXX_COMPILER=C:\Qt\Tools\mingw1310_64\bin\g++.exe
cmake --build build
ctest --test-dir build --output-on-failure
```

## Release packaging

```powershell
# 1. Deploy the launcher stub (faraday.exe) + the Qt application
#    (faraday-app.exe) + the Qt runtime. --skip-plugin-types drops the
#    network-touching and debug plugins (see AV_HARDENING.md).
mkdir dist\Faraday
copy build\faraday.exe dist\Faraday\        # 63 KB static launcher
copy build\faraday-app.exe dist\Faraday\    # the Qt application
C:\Qt\6.8.2\mingw_64\bin\windeployqt.exe --release --compiler-runtime `
    --no-translations --skip-plugin-types qmltooling,networkinformation,tls,generic `
    --qmldir ui dist\Faraday\faraday-app.exe
Remove-Item dist\Faraday\sqldrivers\qsqlmimer.dll, `
    dist\Faraday\sqldrivers\qsqlodbc.dll, dist\Faraday\sqldrivers\qsqlpsql.dll

# 1b. Generate the launcher's runtime.manifest (every runtime file; the
#     launcher refuses to start the app if any listed file is missing)
#     and the portable README.
$dist = (Resolve-Path dist\Faraday).Path
Get-ChildItem dist\Faraday -Recurse -File |
    Where-Object { $_.Name -notin @('faraday.exe','runtime.manifest','portable.txt','README.txt') } |
    ForEach-Object { $_.FullName.Substring($dist.Length + 1) } | Sort-Object |
    Set-Content dist\Faraday\runtime.manifest -Encoding ASCII

# 2. Installer (per-user, no elevation) — build BEFORE staging the
#    portable marker: the installer payload must stay marker-free.
& 'C:\Program Files (x86)\NSIS\makensis.exe' installer\faraday.nsi
#  -> dist\Faraday-1.0.2-setup-win64.exe

# 3. Portable ZIP — stage a copy WITH the portable.txt marker (which
#    switches data storage to <exeDir>\data; see docs\PORTABLE.md).
New-Item -ItemType Directory -Force $env:TEMP\zipstage | Out-Null
Copy-Item -Recurse dist\Faraday $env:TEMP\zipstage\Faraday
Set-Content "$env:TEMP\zipstage\Faraday\portable.txt" "Faraday portable mode marker"
Compress-Archive "$env:TEMP\zipstage\Faraday" dist\Faraday-1.0.2-portable-win64.zip
```

Release rules (see AV_HARDENING.md): plain Release build, no packer, no
obfuscation; the version-info resource, icon and `asInvoker` manifest are
embedded via `resources/faraday.rc`.

## Translations

`i18n/faraday_en.ts` is the source catalog (≈250 strings); `faraday_ar.ts`
is the complete Arabic translation. Refresh both with:

```powershell
C:\Qt\6.8.2\mingw_64\bin\lupdate.exe src ui -no-obsolete -locations absolute -ts i18n\faraday_en.ts i18n\faraday_ar.ts
```

To add a language, copy the file to `faraday_<lang>.ts`, translate in
Qt Linguist, add it to `qt_add_translations` in CMakeLists.txt and to the
language list in `ui/pages/SettingsPage.qml`; the `.qm` embeds
automatically under `:/i18n/`. The language switch applies live, and a
right-to-left language mirrors the whole layout automatically.

## Project layout

```
src/acquisition  WMI/COM + powercfg readers          tests/  14 QtTest suites
src/core         pure metric formulas + verdicts     ui/     QML (pages, components)
src/data         SQLite + JSON settings              docs/   this documentation
src/app          model, sampler, alerts, tray, ...   installer/ NSIS script
data/schema.sql  reference schema                    resources/ icon, rc, manifest
```
