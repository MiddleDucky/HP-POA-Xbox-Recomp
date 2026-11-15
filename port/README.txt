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
       README.txt
       CHANGES.txt
       licenses\
       bin\hppoa.exe
       bin\shadercache\base_shaders.bin
2. Copy the game files from your disc, or the .xiso / .iso file, into a
   folder named "game" beside hppoa_launcher.exe. A disc image
   is unpacked into the game folder the first time you start the launcher;
   the image is left as it is and can be deleted afterwards. Both the North
   American (NTSC-U) and the European (PAL) disc play; with the PAL disc the
   launcher's Language setting picks English, French, German, Spanish or
   Italian.
   The launcher checks every game file against the disc's once (a few
   seconds, longer on a hard drive) and names any that are damaged or
   missing; "Verify game files" on its page checks them again.

The port creates these itself on the first run:
    saves\              your saved games (beside the launcher)
    bin\hppoa.toml      your settings (the launcher and PC Settings change
                        them)
    bin\shadercache\    shaders built on your PC (the first visit to an
                        area prepares them; later visits load at once)
    bin\logs\           the game's log

All the program is in bin\ but the launcher; game\ and saves\ stay beside
the launcher.

Updating to a later version: unpack the new zip over your folder and keep
your game\, saves\ and bin\. Coming from v0.1.0 (everything beside the
launcher): the launcher moves your hppoa.toml, shadercache\ and the file
check into bin\ the first time it starts, and removes the old hppoa.exe;
your saves\ stay where they are.


PLAYING
-------

Start hppoa_launcher.exe. Its page sets 4:3 or 16:9 and the graphics, sound
and control settings, then Play starts the game. ("Don't show this at the
next startup" goes straight to the game; PC Settings in the game changes
everything the page does, and hppoa.toml has show_launcher to bring the page
back.)

Quitting: "Quit To Desktop" (BACK on a controller, Tab on the keyboard) on
the main menu and the pause menu, or Alt+F4.

PC Settings: press Y on the main menu or the pause menu (see below).

Keys that always work
  F10          the full options panel (everything in PC Settings, plus key
               bindings and button remapping)
  Alt+Enter    switch between windowed and borderless
  Alt+F4       quit the game

Controller
  An Xbox 360 / One / Series controller plays like the original Xbox pad.
  The original pad's White and Black buttons are LB and RB.
  Pressing both sticks in (L3 + R3) opens and closes the options; the
  d-pad, A and B move through them.

Keyboard and mouse (defaults; change them under F10 > Controls)
  Move                  W A S D
  Camera                mouse, or the arrow keys (in the menus the arrow
                        keys move through them)
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
the tutorials and the menus, show your keys instead (PC Settings >
Enhancements > Button prompts: Automatic follows what you played with last).


PC SETTINGS (Y on the main menu or the pause menu)
-------------------------------------------------

Up / down choose a row, left / right or A change it, B goes back a level;
B on the list or Start closes. Changes are saved as you make them.

Display       aspect ratio (4:3 or 16:9), windowed / borderless /
              fullscreen, resolution, vsync, 30 or 60 fps
Graphics      field of view, internal resolution, MSAA, SMAA, anisotropic
              filtering, brightness, contrast, an FPS counter
Post-processing  bloom, sharpening, saturation, vibrance, warmth,
              vignette, film grain
Audio         stereo or 5.1 speakers
Controls      keyboard and mouse, mouse sensitivity, camera keys in menus,
              the controller's deadzones and sensitivity, camera
              inversion, vibration
Accessibility colour-blind correction filters
Enhancements  changes to the game, each with its own note in the panel:
              - Button prompts (controller / keyboard / automatic)
              - Show each tutorial once
              - Leave out the "time to turn in" and "we need to go" lines
                (off by default)
              - Responsive movement: quicker turning, the first step at
                once and full speed through turns (off: as the console;
                carrying always as the console)
              - Smooth stretched textures (skies, distant landscapes)
              - Cutscenes without black bars (16:9; off by default)
              - Hide the saving / loading compass
              - Skip the EA and Warner Bros. logos (off by default)

F10 opens the same options as a panel, plus the key bindings and button
remapping.

In 16:9 the game draws the whole wider picture; the HUD moves to the
screen's edges, and the picture shows more at the sides with the same
height as 4:3. Cutscenes keep their letterbox bars unless PC Settings >
Enhancements > Cutscenes without black bars is on; then the cutscene
picture fills the screen.


IF SOMETHING GOES WRONG
-----------------------

The launcher keeps the game's log in bin\logs\hppoa_run.log
(a crash writes its report there too). Please include it with a report.


LEGAL
-----

This port is a fan project, not affiliated with or endorsed by Warner Bros.,
Electronic Arts or Microsoft. It contains no game data; you need your own
copy of the game. The licences of the code built into the executables are
in licenses\. The port is built on the xboxrecomp toolkit,
https://github.com/sp00nznet/xboxrecomp, whose LGPL parts come from xemu.
