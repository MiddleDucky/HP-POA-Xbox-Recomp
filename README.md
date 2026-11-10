# Harry Potter and the Prisoner of Azkaban — Xbox Recomp

A native Windows port of the original Xbox game, made by **static recompilation**: the game's own code, translated
to run directly on a PC, with a new DirectX 12 renderer, sound and input in place of the console's.

> **No game data is included.** You need the files from your own copy of the Xbox disc (North American, NTSC-U).

**[Download the latest release](https://github.com/MiddleDucky/HP-POA-Xbox-Recomp/releases/latest)**

---

## Contents

- [Requirements](#requirements)
- [Installing](#installing)
- [Playing](#playing)
- [What's new compared to the Xbox](#whats-new-compared-to-the-xbox)
- [Fixes and improvements](#fixes-and-improvements)
- [Enhancements](#enhancements)
- [Options (F10)](#options-f10)
- [If something goes wrong](#if-something-goes-wrong)
- [Credits and legal](#credits-and-legal)

---

## Requirements

| | Minimum |
|---|---|
| **OS** | Windows 10 or 11, 64-bit (Linux: Proton or Wine with vkd3d) |
| **CPU** | 4 threads (Intel Core 2nd generation / AMD FX or newer) |
| **Memory** | 4 GB RAM |
| **Graphics** | DirectX 12 card (feature level 11_0) with a current driver: NVIDIA GeForce GTX 600 series, AMD Radeon HD 7000 series, Intel HD Graphics 500 series or newer |
| **Storage** | The game files + about 100 MB for the port |
| **Input** | Keyboard and mouse, or an Xbox controller (360, One or Series) |

These are estimates: the port has so far been played on a recent high-end PC. Windows "N" editions also need
Microsoft's Media Feature Pack.

## Installing

1. Download `hppoa_v0.1.0_windows.zip` from the [releases](https://github.com/MiddleDucky/HP-POA-Xbox-Recomp/releases)
   and unpack it into a folder of your choice.
2. Copy the game files from your disc (or your `.xiso`) into a folder named **`game`** next to the executables, so
   that `game\default.xbe` exists:

   ```
   your folder\
       hppoa_launcher.exe
       hppoa.exe
       README.txt
       CHANGES.txt
       licenses\
       shadercache\base_shaders.bin
       game\
           default.xbe
           ...the rest of the disc
   ```

3. Start **`hppoa_launcher.exe`**. On the first run it checks every game file against the disc's (a few seconds,
   longer on a hard drive) and names any that are damaged or missing.

The port creates these itself: `hppoa.toml` (your settings), `saves\` (your saved games) and more of `shadercache\`
(the first visit to an area prepares its shaders; later visits load at once).

**Updating:** replace the port's files and keep your `hppoa.toml`, `saves\` and `shadercache\`.

## Playing

The launcher sets 4:3 or 16:9 and the graphics, sound and control settings; **Play** starts the game. Everything it
sets can be changed in the game with **F10**.

| Key | |
|---|---|
| **F10** | Options |
| **Alt+Enter** | Windowed / borderless |
| **Alt+F4** | Quit |

**Controller:** an Xbox 360 / One / Series controller plays like the original pad. The original White and Black
buttons are **LB** and **RB**. Pressing **both sticks in (L3 + R3)** opens and closes the options.

**Keyboard and mouse** (defaults; every key can be changed under F10 > Controls):

| Action | Key |
|---|---|
| Move | W A S D |
| Camera | Mouse, or the arrow keys |
| A / B | Space / E |
| X / Y | Left / right mouse button |
| Black / White | R / Q |
| Left / right trigger | Shift / Ctrl |
| D-pad | 1 2 3 4 (up, down, left, right) |
| Start / Back | Escape / Tab |
| Stick clicks | C (left), middle mouse button (right) |

**Quitting:** "Quit To Desktop" on the main menu and the pause menu (Back on a controller, Tab on the keyboard).

## What's new compared to the Xbox

### Picture
- **Any resolution**, windowed, borderless or fullscreen.
- **True 16:9**: the game draws the whole wider picture instead of stretching the 4:3 one. The HUD moves to the
  screen's edges, and the HUD's map and the inventory's map widen with it. 4:3 is still available.
- **60 fps** (or the game's own 30).
- **MSAA, SMAA and anisotropic filtering.**
- **Post-processing**, all neutral by default so the picture starts as the console's: the game's bloom strength,
  sharpening, saturation, vibrance, warmth, vignette, film grain and field of view.
- **Colour-blind correction filters.**

### Sound
- The game's sound on your PC's speakers, **stereo or 5.1 surround**.

### Controls
- **Keyboard and mouse** play as the controller, every key rebindable.
- **Xbox 360 / One / Series controllers** with adjustable deadzones, sensitivity, camera inversion and vibration.
- **Keyboard button prompts**: with keyboard and mouse, the game's buttons show your keys instead, as round
  icons in the button's colour: on the HUD, in the tutorials, the menus and the title screen's "Press [Esc]".
  They switch as soon as you pick up the other device, and follow your bindings.

### Also
- **Quit To Desktop** on the main menu and the pause menu, with a Yes / No box.
- **A launcher** to choose the picture, sound and control settings before playing, and to check the game files
  against the disc's.

## Fixes and improvements

Problems in the game, or ones that 60 fps, 16:9 and a PC brought out, put right.

**Timing and speech**
- **The game runs at the right speed at 60 fps.** The game's clock steps a fixed 1/30 of a second each frame, so at
  60 fps everything, cutscenes included, would play twice as fast while the voices kept their own pace. The clock
  now follows the frame rate: play, cutscenes and voice lines stay in step, and dialogue plays as it should.
- **Lips stop when the line does.** Characters kept mouthing words for a while after their voice line ended (Ron's
  "Are you all right up there?" went on in silence); their faces now go still when the voice stops.
- **No getting stuck on ledges.** At 60 fps Harry could stop part way along a ledge while wall-sneaking in the
  Carpe Retractum challenge, unable to move either way; that stretch now runs at the console's 30 fps.

**Characters and cutscenes**
- **No more orange skin.** In cutscenes the game drew faces, hands and robes a second time over themselves, which
  turned pale skin orange: **Malfoy** most of all, Crabbe and Goyle too. Characters now look in cutscenes as they
  do in play.
- **No blinking faces.** That same second pass came and went from frame to frame, so faces flickered (the train
  cutscene most of all); gone with it.
- **The train's window light falls on the characters**, sweeping over them with the walls, instead of only on the
  walls behind them.

**The world**
- **Rooms beyond doorways always drawn.** The game only drew a room when its doorway was on screen, so rooms
  beside the view popped in as the camera turned.
- **Areas load in time.** A room's data was streamed slowly enough that a doorway into it showed nothing for
  about a second; it now arrives within a frame. Stepping straight back through a doorway no longer leaves the
  next area black.
- **The Glacius lever** no longer turns black before it lights up: its front fades to the lit lever's colour.

**16:9**
- **The HUD's map**: the map and Harry's arrow sit inside their panel.
- **The inventory's map**: wider, with the room names, dots and Harry's mark in their places.
- **Action prompts** (Open, Dodge) stay under the A button, and button icons in a line of text stay in their gap.
- **The menus**: the selection glow stays by its item, a selected item's growing copy grows from the middle of its
  word instead of drifting right, and panels from a side of the picture start at that side of the screen.
- **A cutscene shot reframed** (Night 0's corridor), so a character leaving the room doesn't walk on past where the
  console's frame loses them.
- **The legal screen** at start stays 4:3, as the movies around it.

**Menus**
- **"Quit To Desktop"** no longer stays on screen over the attract movie the main menu plays when left idle.

## Enhancements

Changes to how the game plays, each with its own switch under **F10 > Enhancements**.

| Enhancement | Default | What it does |
|---|---|---|
| **Button prompts** | Automatic | Key icons while you play with keyboard and mouse, Xbox buttons with a controller. Can be fixed to either. |
| **Show each tutorial once** | On | A tutorial popup you have already seen doesn't come back. |
| **Responsive movement** | On | Harry turns to the stick twice as quickly, sets off the moment the stick moves (no shuffle or turn-on-the-spot wait), and keeps his running speed through turns. Off: as on the console. |
| **Smooth stretched textures** | On | Skies and the landscapes behind the levels without blocky pixels. |
| **Hide the saving / loading compass** | On | The turning compass in the top left corner while the game saves or loads isn't drawn. |
| **Leave out "time to turn in" lines** | Off | Harry, Ron and Hermione's "time to turn in" / "we need to go" reminders, and Neville's "can we go back now", left out. |
| **Cutscenes without black bars** | Off | In 16:9, cutscenes fill the screen instead of keeping their letterbox. |

Always on: **L3 + R3** opens the options on a controller, and menus that turn the game world off show black
behind them instead of the game's grey-blue.

## Options (F10)

| Tab | |
|---|---|
| **Graphics** | Aspect ratio, display mode, vsync, 30 / 60 fps, resolution, MSAA, SMAA, anisotropic filtering, brightness, contrast, FPS counter, post-processing, field of view |
| **Audio** | Stereo or 5.1 speakers |
| **Controls** | Key bindings, mouse sensitivity, deadzones and sensitivity, camera inversion, vibration |
| **Accessibility** | Colour-blind correction filters |
| **Enhancements** | See [above](#enhancements) |

"Defaults (this page)" resets the open tab.

## If something goes wrong

The launcher keeps the game's log in **`hppoa_run.log`** beside the executables; a crash writes its report there
too. Please attach it to an [issue](https://github.com/MiddleDucky/HP-POA-Xbox-Recomp/issues), with what you were
doing.

- **The launcher names missing or damaged files:** copy them from the disc again.
- **The first visit to an area stutters:** its shaders are being prepared; it is quick from the second visit on.

## Credits and legal

This port is a fan project, not affiliated with or endorsed by Warner Bros., Electronic Arts or Microsoft.
*Harry Potter* and related names are trademarks of Warner Bros. Entertainment Inc. It contains no game data: you
need your own copy of the game.

Built with:

- **[xboxrecomp](https://github.com/sp00nznet/xboxrecomp)** by sp00nz (MIT): the static recompilation toolkit.
- **xemu** ([xemu-project/xemu](https://github.com/xemu-project/xemu), LGPL-2.1-or-later): the Xbox audio
  processor and GPU register definitions, by espes, Jannik Vogel and Matt Borgerson.
- **SCDA XBERecomp's renderer** by TGP482 and Joshhhuaaa (MIT).
- **[Dear ImGui](https://github.com/ocornut/imgui)** by Omar Cornut (MIT): the options panel.
- **Magic School One** by FontMesa: the launcher's title font.

The licence texts are in the release's `licenses\` folder.
