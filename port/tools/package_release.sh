#!/usr/bin/env bash
# The build players get: configures build-release (HPPOA_RELEASE=ON: F6's trainer and F12's captures off, the
# version in the run log), builds the game and the launcher, and puts in dist/hppoa-<version>/:
#   hppoa_launcher.exe            setup: 4:3 / 16:9, graphics, controls, the game files check; starts the game
#   hppoa.exe                     the game
#   shadercache\base_shaders.bin  shaders compiled ahead (read only; the player's own go in shaders.bin)
#   README.txt                    requirements, installing, controls, the F10 options
#   CHANGES.txt                   what changed in each release
#   licenses\                     the licences of the code built into the executables
# and zips the folder as dist/hppoa_v<version>_windows.zip. Players put the folder's contents beside their game\
# folder (default.xbe and the disc's files); hppoa.toml, saves\ and the rest of shadercache\ are made on first run.
#   VERSION=0.1.0 tools/package_release.sh        (from port/; VERSION defaults to CMakeLists.txt's)
set -euo pipefail
port="$(cd "$(dirname "$0")/.." && pwd)"
cd "$port"
mkdir -p logs
# cross from Linux with the MinGW toolchain file (as build-win-trace), or natively in an MSYS2 UCRT64 shell
if command -v x86_64-w64-mingw32-objdump > /dev/null; then
    cross=(-DCMAKE_TOOLCHAIN_FILE="$port/../cmake/mingw-w64-x86_64.cmake")
    objdump=x86_64-w64-mingw32-objdump
else
    cross=(-G Ninja)
    objdump=objdump
fi
version="${VERSION:-$(sed -n 's/^set(HPPOA_VERSION "\([^"]*\)".*/\1/p' CMakeLists.txt)}"
[ -n "$version" ] || { echo "no version" >&2; exit 1; }

cmake -S . -B build-release "${cross[@]}" -DCMAKE_BUILD_TYPE=Release -DHPPOA_RELEASE=ON \
      -DHPPOA_VERSION="$version" > logs/build-release-config.log
nice -n 10 cmake --build build-release -j"$(nproc)" --target hppoa hppoa_launcher > logs/build-release.log 2>&1 ||
    { tail -30 logs/build-release.log; exit 1; }

dist="$port/dist/hppoa-$version"
# Never delete the folder: players and testers drop game\, settings and saves into it. Only the files below are
# replaced; anything else found there is left alone (the HP:PS port lost a game\ folder to an rm -rf of its dist).
mkdir -p "$dist"
cp -f build-release/hppoa.exe build-release/hppoa_launcher.exe "$dist/"
cp -f README.txt "$dist/"
cp -f CHANGES.txt "$dist/"
# licence texts: the LGPL code from xemu (src/apu, src/nv2a) is linked into hppoa.exe, and the LGPL requires a copy
# of the licence with every binary; the MIT code built in keeps its notices: xboxrecomp, SCDA's renderer, Dear ImGui;
# the launcher title's font, Magic School One (FontMesa), its own terms
mkdir -p "$dist/licenses"
cp -f "$port/../LICENSE" "$dist/licenses/LICENSE.txt"
cp -f "$port/../NOTICE" "$dist/licenses/NOTICE.txt"
cp -f "$port/../LICENSES/LGPL-2.1.txt" "$dist/licenses/LGPL-2.1.txt"
cp -f "$port/src/renderer/LICENSE.SCDA" "$dist/licenses/SCDA-XBERecomp.txt"
cp -f "$port/third_party/imgui/LICENSE.txt" "$dist/licenses/DearImGui.txt"
cp -f "$port/src/launcher/font/MagicSchool-licensing.txt" "$dist/licenses/MagicSchool-font.txt"

# res/base_shaders.bin (not in git) ships as shadercache\base_shaders.bin: a player's first visit to an area then
# needs fewer shaders compiled. Refresh it from a play-through's shadercache\shaders.bin when shader generation
# changes; old entries simply stop matching
if [ -s res/base_shaders.bin ]; then
    mkdir -p "$dist/shadercache"
    cp -f res/base_shaders.bin "$dist/shadercache/base_shaders.bin"
else
    echo "warning: res/base_shaders.bin missing, no base shaders shipped" >&2
fi

# the security bits every player build must carry
for exe in "$dist"/*.exe; do
    bits=$("$objdump" -p "$exe" | grep -A4 DllCharacteristics)
    for want in HIGH_ENTROPY_VA DYNAMIC_BASE NX_COMPAT; do
        grep -q "$want" <<< "$bits" || { echo "$exe: $want missing" >&2; exit 1; }
    done
done

# every DLL either exe imports must come with Windows 10/11 itself (the C/C++ runtimes are linked in statically)
builtin='^(api-ms-win-.*|kernel32|user32|gdi32|shell32|ole32|oleaut32|winmm|dwmapi|dbghelp|d3d11|d3d12|dxgi|d3dcompiler_47|xaudio2_8|xaudio2_9|xinput1_4|mfplat|mfreadwrite|windowscodecs|imm32|comctl32|comdlg32|advapi32|ws2_32|bcrypt|ntdll|uxtheme|shlwapi|setupapi|hid|msvcrt|ucrtbase)\.dll$'
for exe in "$dist"/*.exe; do
    while read -r dll; do
        grep -Eq "$builtin" <<< "$dll" || { echo "$(basename "$exe") needs $dll, which Windows does not ship" >&2; exit 1; }
    done < <("$objdump" -p "$exe" | sed -n 's/^\s*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)
done

ls -la "$dist"

# the zip players download: only what this script put in the folder, at its top. Never the whole folder: a tester's
# game\ files, settings, saves or shaders.bin may be in it, and none of those may be handed out
zip="$port/dist/hppoa_v${version}_windows.zip"
rm -f "$zip"
python3 - "$dist" "$zip" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
ship = ["hppoa_launcher.exe", "hppoa.exe", "README.txt", "CHANGES.txt", "shadercache/base_shaders.bin"]
ship += ["licenses/" + f for f in sorted(os.listdir(os.path.join(src, "licenses")))]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for rel in ship:
        p = os.path.join(src, rel)
        if os.path.isfile(p):
            z.write(p, rel)
    print("\n".join(z.namelist()))
PY
ls -la "$zip"
