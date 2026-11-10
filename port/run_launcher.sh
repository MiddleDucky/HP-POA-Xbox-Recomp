#!/usr/bin/env bash
# The launcher (hppoa_launcher.exe) under Wine: 4:3 or 16:9 and the graphics settings, then the game with the
# environment run_win.sh sets. Both exes and hppoa.toml sit in run/ beside game/.
# The diagnostics window (hppoa_diag.exe) starts beside it: reports with a note (Ctrl+Shift+R in the game too),
# crash reports, trace switches. HPPOA_DIAG=0 leaves it out.
cd "$(dirname "$0")/run" || exit 1
export WINEDEBUG=${WINEDEBUG:--all}
# the GPU driver's compiled pipelines, kept per game: Wine's vkd3d has no pipeline library, so Mesa's disk cache is
# what carries them between runs, and its shared 1 GB default evicted them for other games
export MESA_SHADER_CACHE_DIR=${MESA_SHADER_CACHE_DIR:-$PWD/shadercache/mesa}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-4G}
if [ "${HPPOA_DIAG:-1}" != 0 ] && [ -f hppoa_diag.exe ] && ! pgrep -x hppoa_diag.exe >/dev/null; then
    wine ./hppoa_diag.exe >/dev/null 2>&1 &
fi
exec wine ./hppoa_launcher.exe "$@"
