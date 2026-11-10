#!/usr/bin/env bash
# Run the Windows build of hppoa under Wine from run/ (the exes, game/ -> ../../game,
# hppoa.toml, shadercache/). Wine hands these variables to the Windows process.
#   RECOMP_TITLE_KEVENTS  events the title builds itself (D3D, worker threads)
#   RECOMP_VBLANK         60 Hz vblank into the title's NV2A ISR
#   RECOMP_AC97_READY     codec ready + emulated APU behind trapped MMIO
#   RECOMP_DSP_ACK        DirectSound's DSP command word; only used with
#                         RECOMP_HLE_DSOUND=0 (XDK DirectSound on the emulated APU)
#   RECOMP_HLE_DSOUND     default on: DirectSound on a host XAudio2 mixer
#   RECOMP_PB_EXEC / RECOMP_FB_WINDOW  execute the pushbuffer, show the framebuffer
#   RECOMP_USB            emulated USB host + Controller S, fed by the PC's pad
#                         through XInput (an Xbox One/360 pad: LB = White, RB = Black)
# Graphics settings (display mode, resolution, internal resolution, MSAA/SMAA,
# filtering, brightness, HDR10 output) live in hppoa.toml beside
# the exe and in the F10 panel in game; Alt+Enter flips windowed/borderless.
# A RECOMP_* variable set here wins over the file for that run, e.g.
#   RECOMP_WINDOW_SIZE=1920x1080 RECOMP_ASPECT=16:9 RECOMP_DISPLAY_MODE=borderless
#   RECOMP_INTERNAL_HEIGHT=1440 RECOMP_MSAA=4 RECOMP_SMAA=1 RECOMP_ANISO=16
#   RECOMP_HLE_D3D=0      old renderer: the emulated NV2A, drawn on the CPU
cd "$(dirname "$0")/run" || exit 1
export RECOMP_TITLE_KEVENTS=1 RECOMP_VBLANK=1 RECOMP_AC97_READY=1
export RECOMP_DSP_ACK=0x834D0810
export RECOMP_PB_EXEC=1 RECOMP_FB_WINDOW=1
# the register mirror thread rests 250 us between passes instead of spinning a core
export RECOMP_ACK_SLEEP_US=${RECOMP_ACK_SLEEP_US:-250}
export RECOMP_USB=1
# NTSC game files: 59.94 Hz vblank; hppoa.toml frame_rate 60 (default) or 30, the game's own (RECOMP_FRAME_RATE wins)
# freed MmAllocateContiguousMemory blocks are reused: the title frees and reloads its textures for every level,
# and the bump allocator ran the 64 MB window out by the first gameplay
export RECOMP_HEAP_RECLAIM=1
export WINEDEBUG=${WINEDEBUG:--all}
# the GPU driver's compiled pipelines, kept per game: Wine's vkd3d has no pipeline library, so Mesa's disk cache is
# what carries them between runs, and its shared 1 GB default evicted them for other games
export MESA_SHADER_CACHE_DIR=${MESA_SHADER_CACHE_DIR:-$PWD/shadercache/mesa}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-4G}
# every run's output also goes to ../logs/last_run.log (the previous one is kept as prev_run.log)
mkdir -p ../logs
[ -f ../logs/last_run.log ] && mv -f ../logs/last_run.log ../logs/prev_run.log
wine ./${HPPOA_EXE:-hppoa.exe} "$@" 2>&1 | tee ../logs/last_run.log
