#!/usr/bin/env bash
# Disassemble + recompile default.xbe into src/recomp/gen (run from anywhere). Every hand-written override file is
# listed with --exclude-manual so its sub_X keeps the generated body as sub_X_gen.
set -e
cd "$(dirname "$0")/.."
[ -f game/default_analysis.json ] || python3 -m tools.xbe_parser game/default.xbe --json game/default_analysis.json
python3 -m tools.disasm game/default.xbe --text-only --extra-sections D3D,D3DX,XGRPH,DSOUND,XPP \
    --seed-functions port/icall_seeds.json
python3 -m tools.func_id game/default.xbe
python3 -m tools.abi_analysis game/default.xbe
python3 -m tools.recomp game/default.xbe --all --split 1000 --gen-dir port/src/recomp/gen --game-name hppoa \
    --exclude-manual port/src/hle_dsound.c \
    --exclude-manual port/src/hle_input.c \
    --exclude-manual port/src/hle_debug.c \
    --exclude-manual port/src/hle_clock.c \
    --exclude-manual port/src/hle_widescreen.c \
    --exclude-manual port/src/hle_quit.c \
    --exclude-manual port/src/hle_trainer.c \
    --exclude-manual port/src/hle_loader.c \
    --exclude-manual port/src/hle_portals.c \
    --exclude-manual port/src/recomp_extra.c \
    --exclude-manual port/src/renderer/xdk/hle_d3d8.cpp
