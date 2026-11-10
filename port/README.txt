HARRY POTTER AND THE PRISONER OF AZKABAN - PC PORT
==================================================

A native Windows port of the original Xbox game, made by static recompilation.
It contains no game data: you need the files from your own copy of the Xbox disc.


MINIMUM SYSTEM REQUIREMENTS
---------------------------

  OS:       Windows 10 or 11, 64-bit (Linux: Proton or Wine with vkd3d)
  CPU:      4 threads (Intel Core 2nd generation / AMD FX or newer)
  Memory:   4 GB RAM
  Graphics: DirectX 12 card (feature level 11_0) with a current driver:
            NVIDIA GeForce GTX 600 series, AMD Radeon HD 7000 series,
            Intel HD Graphics 500 series or newer
  Storage:  the game files + about 100 MB for the port
  Input:    keyboard and mouse, or an Xbox controller (360, One or Series)

These are estimates: the port has been played on a recent high-end PC only.
Windows "N" editions also need Microsoft's Media Feature Pack.


INSTALLING
----------

1. Put the port's files in a folder of your choice:
       hppoa_launcher.exe
       hppoa.exe
       README.txt
       CHANGES.txt
       licenses\
       shadercache\base_shaders.bin
2. Copy the game files from your disc or the .xiso file into a folder named
   "game" in the same folder as the executables, so that game\default.xbe
   exists. The port was made with the North American (NTSC-U) disc.
   The launcher checks every game file against the disc's once (a few
   seconds, longer on a hard drive) and names any that are damaged or
   missing; "Verify game files" on its page checks them again.

The port creates these itself on the first run:
    hppoa.toml     your settings (the launcher and F10 change them)
    saves\         your saved games
    shadercache\   shaders built on your PC (the first visit to an area
                   prepares them; later visits load at once)

Updating to a later version: replace the files above and keep your
hppoa.toml, saves\ and shadercache\.


PLAYING
-------

Start hppoa_launcher.exe. Its page sets 4:3 or 16:9 and the graphics, sound
and control settings, then Play starts the game. ("Don't show this at the
next startup" goes straight to the game; F10 in the game changes everything
the page does, and hppoa.toml has show_launcher to bring the page back.)

Quitting: "Quit To Desktop" (BACK on a controller, Tab on the keyboard) on
the main menu and the pause menu, or Alt+F4.

Keys that always work
  F10          Options (see below)
  Alt+Enter    switch between windowed and borderless
  Alt+F4       quit the game

Controller
  An Xbox 360 / One / Series controller plays like the original Xbox pad.
  The original pad's White and Black buttons are LB and RB.
  Pressing both sticks in (L3 + R3) opens and closes the options; the
  d-pad, A and B move through them.

Keyboard and mouse (defaults; change them under F10 > Controls)
  Move                  W A S D
  Camera                mouse, or the arrow keys
  A                     Space
  B                     E
  X                     left mouse button
  Y                     right mouse button
  Black / White         R / Q
  Left / right trigger  Shift / Ctrl
  D-pad                 1 2 3 4 (up, down, left, right)
  Start / Back          Escape / Tab
  Stick clicks          C (left), middle mouse button (right)

Button prompts: with keyboard and mouse the game's buttons, on the HUD, in
the tutorials and the menus, show your keys instead (F10 > Enhancements >
Button prompts: Automatic follows what you played with last).


OPTIONS (F10)
-------------

Graphics      aspect ratio (4:3 or 16:9), windowed / borderless /
              fullscreen, vsync, 30 or 60 fps, internal resolution, MSAA,
              SMAA, anisotropic filtering, brightness, contrast, an FPS
              counter, and post-processing (bloom, sharpening, saturation,
              vibrance, warmth, vignette, film grain, field of view)
Audio         stereo or 5.1 speakers
Controls      key bindings, mouse sensitivity, the controller's deadzones
              and sensitivity, camera inversion, vibration
Accessibility colour-blind correction filters
Enhancements  changes to the game, each with its own note in the panel:
              - Button prompts (controller / keyboard / automatic)
              - Show each tutorial once
              - Leave out the "time to turn in" and "we need to go" lines
                (off by default)
              - Responsive movement: quicker turning, the first step at
                once and full speed through turns (off: as the console)
              - Smooth stretched textures (skies, distant landscapes)
              - Cutscenes without black bars (16:9; off by default)
              - Hide the saving / loading compass

In 16:9 the game draws the whole wider picture; the HUD moves to the
screen's edges. Cutscenes keep their letterbox bars unless F10 >
Enhancements > Cutscenes without black bars is on.


IF SOMETHING GOES WRONG
-----------------------

The launcher keeps the game's log in hppoa_run.log beside the executables
(a crash writes its report there too). Please include it with a report.


LEGAL
-----

This port is a fan project, not affiliated with or endorsed by Warner Bros.,
Electronic Arts or Microsoft. It contains no game data; you need your own
copy of the game. The licences of the code built into the executables are
in licenses\. The port is built on the xboxrecomp toolkit,
https://github.com/sp00nznet/xboxrecomp, whose LGPL parts come from xemu.
