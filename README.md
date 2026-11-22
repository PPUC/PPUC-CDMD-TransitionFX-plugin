# TransitionFX

A plugin for the PPUC Colorizing DMD editor (the Serum editor). It builds transitions for you on
new frames, or as a scene, from a frame of your project. It needs the editor's plugin API version
10 (PPUC Colorizing DMD 3.99.40 or later).

![The transitions, halfway through: white is the picture, black is what the transition has taken away](docs/transitions.png)

| Transition | Options | What it does |
| --- | --- | --- |
| **Alternating Slide** | Horizontal, Vertical | Horizontal: even rows (0, 2, 4) slide left and odd rows (1, 3, 5) slide right. Vertical: even columns (0, 2, 4) slide up and odd columns (1, 3, 5) slide down. |
| **Blinds** | Horizontal, Vertical | Bars at regular intervals grow wider. |
| **Checkerboard** | 2 x 2, 4 x 4, 8 x 8, 16 x 16 | The picture is cut into squares of the size you choose. They disappear one by one: first the squares of one color of a checkerboard, sweeping from the top left corner, then the others. |
| **Dissolve** | Orderly, Random, WPC | Pixels disappear in a regular pattern, or at random. WPC is the dot pattern many Bally/Williams games use: one eighth of the pixels at a time, in a fixed pattern (always 7 frames: the number of frames is fixed, by design). |
| **Dust** | Right, Left, Up, Down (the wind) | The picture crumbles to dust and the wind blows it away. A wave of wind starts on the side it comes from; the lit pixels fly off, faster and faster, drifting and wobbling, and some are lost on the way. One last speck stays in the last frame. |
| **Fade** | | The picture gets darker (to black) or brighter (to white) with every frame. |
| **Melt** | Smooth, Dripping | The picture melts down off the screen: every column slides down at its own speed. Smooth: the speeds follow a wave (the same as Wave, Vertical). Dripping: the speeds look random, in streaks, like paint dripping. |
| **Morph** | Random, Sweep Right / Left / Down / Up | The lit pixels of the first frame travel across the screen and become the lit pixels of the second frame; everything else is black. Needs `Another frame`. Random: the pixels set off at random moments. Sweep: in a wave from the side you choose. |
| **Pixelate** | | The picture turns into bigger and bigger blocks (each shows the pixel at its middle), up to blocks as big as the frame is tall. The blocks grow slowly at first and faster later. |
| **Slide** | Up, Down, Left, Right | The whole picture slides off the screen. |
| **Split** | Horizontal, Vertical | Cut in the middle; the halves slide apart. Horizontal: cut across, the halves go up and down. Vertical: cut down, the halves go left and right. |
| **Wave** | Horizontal, Vertical | The picture turns into a wavy curtain that is pulled out of the frame. Horizontal: the rows slide sideways, in one wave over the height. Vertical: the columns slide down, in four waves over the width. The crests go twice as fast as the troughs. |
| **Wipe** | Up, Down, Left, Right, Centre-Out, Edge-In, Spiral | A bar wipes over the picture in the direction you choose (Left: it comes in from the right edge and moves left). Centre-Out: a shape grows from the centre. Edge-In: the picture closes in from the outside edges. Spiral: a spiral winds in from the top left corner. |
| **Zoom** | In, Out | In: the picture grows toward you, a smaller and smaller part of the middle fills the frame (down to an eighth of the width). Out: the picture shrinks toward the middle (down to a sixteenth of the width). |

Every transition goes **to black, to white or to another frame**: choose `Transition to` in the window. The last
frame of a transition is never completely black (or white): a dissolve or a wipe ends with a few
pixels of the picture left, the sliding ones with a thin strip of the picture, a fade with a color one
step from black (white).

## How to use it

1. Open your project and select the frame you want to start the transition from.
2. Choose `Plugins -> TransitionFX...`. One window opens:
   - **Transition:** the transition. The next field changes with it.
   - **Option:** the transition's direction, dissolve type ... Its name and its list depend on the
     transition. The window remembers the option of each transition.
   - **Transition to:** black, white, or **another frame** (see "Going to another frame" below).
   - **Number of frames:** the list holds only the numbers that work for that transition on your
     frame (see below), so there is nothing to mistype. A number you chose stays chosen while
     the transition can make that many frames.
   - **Place the new frames:** immediately after the starting frame, at the end of the project, or
     as a scene that the starting frame starts (see "As a scene" below).
   - **Preview:** four frames of the transition, drawn from your frame as `Apply` will
     make them. Dynamic zones are drawn in the colors of their dynamic sets, so a dynamic frame
     shows its colors here (the new frames keep their zones as they are).
3. Click `Apply`. `Cancel` changes nothing.

The transition, its option, what it goes to (black or white) and the placement are radio buttons,
so one click chooses.

The window does not block the editor. While it is open you can click another frame in the frame
list: the window then starts from that frame, and the preview shows it. The window opens on the
choices you applied last.

### Why the number of frames is limited

Every frame of a transition has to differ from the others. A slide moves whole pixels, so a
frame 32 pixels tall can only be in so many places; blinds can only grow as wide as the gap
between them. Each transition therefore has a most for the size of your frame, and the list in
the window stops there. On a 128 x 32 frame, for example: Dissolve 255 (WPC always 7), Split Horizontal 15. Dust and Morph find their most by trying: the frames must all differ.
Larger frames (192 x 64, 256 x 64) allow more. A fade is limited by how many brightness levels
the picture has.

### What Apply makes

The plugin adds that many new frames. Each new frame is a copy of
the frame you started from, one step further along the transition than the one before. The new
frames are selected afterwards, so you can use them right away. Placing them after the
starting frame moves them as one block, so sections and the order of the project stay right. It is all one step of the
editor's history: **one `Undo` removes all the new frames.** The frame you started from is never
changed.

The new frames have **no comparison mask** (the mask of the frame you started from is not
copied; its shape mode is). They keep the frame's display time. To change it, use the editor's
`Duration` field in the Frame section: the plugin leaves the new frames selected, and `Duration`
applies to all selected frames. (The editor's plugin API cannot set a frame's duration.)

### As a scene

`As a scene that the starting frame starts` is offered in projects of 128 x 32 frames (libserum
plays scenes in no other). The plugin makes the frames as above, makes a scene of them, lets the
starting frame start it (its Scene field), removes the plain frames again and puts the scene's
frames immediately after the starting frame. The window then also asks for the scene's **frame duration** in milliseconds;
it starts as the duration of your frame. The scene's other settings are the editor's defaults
(played once, keeps its last frame, no flags); change them in the editor's Scenes section. If the
starting frame started another scene before, it starts the new one now, and the plugin tells you.
A frame of a scene has no dynamic zones, so the scene's frames have none.

The field **Also include in the scene** adds the **starting frame** (your frame, unchanged) as the scene's
first frame, and, for a transition to another frame, the **ending frame** (the other frame, as it
is) as its last. They are there so you can give those frames their own timing in the editor's
Scenes section. **They do not count towards the number of frames**: with 10 frames and both
added the scene holds 12 frames, 10 of them the transition's steps.

All of it, frames and scene, is one step of the editor's history.

## Installing it

1. Get the plugin folder `TransitionFX` (see "Getting the plugin" below). It contains
   `plugin.cfg` and the plugin libraries: `plugin-transitionfx64.dll` (Windows x64),
   `plugin-transitionfx.dll` (Windows x86), `plugin-transitionfx.so` (Linux x64 and aarch64) and
   `plugin-transitionfx.dylib` (macOS arm64). The download for your platform holds the library
   that fits your computer.
2. In the editor, open `Settings -> Plugins` and click `Open Folder`. Put the whole
   `TransitionFX` folder into that folder (on Windows usually
   `%APPDATA%\ppuc.org\PPUC Colorizing DMD\plugins`), so that `plugin.cfg` is at
   `...\plugins\TransitionFX\plugin.cfg`.
   If you used this plugin under its earlier name, **delete the old `TransitionBuilder` folder**
   from the plugin folder first; the two would both appear in the `Plugins` menu.
3. Restart the editor, open `Settings -> Plugins`, and check `TransitionFX`.
4. A `Plugins` menu appears with the entry `TransitionFX...`.

If the plugin does not load, `Settings -> Plugins` names the reason, and `Help -> Plugin Log`
has more.

On macOS, a library downloaded from the internet can be blocked until you allow it. If the editor
says it cannot load the plugin, run `xattr -dr com.apple.quarantine <the TransitionFX folder>` in
Terminal and restart the editor.

## Getting the plugin

GitHub builds it for you, for Windows (x64 and x86), Linux (x64 and aarch64) and macOS (arm64):

1. Open the repository's `Actions` tab. The `Build plugin` run starts by itself after a push (or
   choose it and click `Run workflow`).
2. When the run is green, open it and download the one for your platform under `Artifacts`, such as `TransitionFX-windows-x64_v1.02`
   (the version is in the name). Unpack it: it holds the `TransitionFX` folder from the steps above.

Every platform build also runs the tests. They come after the upload, so a failing test never
hides the plugin, but a red job means something is wrong on that platform; the failing line says
what. The Linux aarch64 build runs on GitHub's free ARM runner, which only public repositories
get.

## Going to another frame

Choose `Another frame` in `Transition to`, then select two frames in the frame list: click one,
then Ctrl+click (Cmd+click on macOS) the other. **The frame you clicked last is the start**, the
other selected frame the end; the note at the top of the window says "From frame X to frame Y",
and the window follows when you select other frames. The preview shows your two frames.

- Slide, Wipe, Split, Blinds, Dissolve, Alternating Slide, Checkerboard, Wave and Zoom Out take
  pixels of the start frame away, and the other frame shows underneath, where they were. By
  default the other frame stays still.
- **Second frame:** Slide, Alternating Slide, Wave and Melt also have a `Second frame` choice.
  `Stays in place, revealed` is what is described above. `Moves in with the effect` makes the other
  frame follow the start frame in, in the same pattern: a slide pushes it in right behind the start
  frame, alternating lines bring it in line by line, a wave or a melt lets each row or column of it
  arrive at the speed of the same row or column of the start frame (a melt lets it drip in from the
  top). The choice is only shown for those transitions.
- **Morph** only works going to another frame (without one it offers no frames). Its pixels are
  the ones brighter than the darkest shade of each frame, plus the pixels you colored in where the
  original is black (a colorized pixel that is not black, or in a dynamic zone, counts too); each of them is paired with a pixel of
  the other frame so that they hardly cross, and changes to the new pixel's color halfway.
  Pixels go to pixels of the same color (white to white, and the white multiplies if the other picture
  has more of it); colors only one frame has are paired by place. Pixels that are the same in both
  frames (same shade, same color) do not take part; a background of several colors (a dithered sky) is
  recognised by being the same in both frames, and a place without a travelling pixel shows real background: its own pixel,
  or the other frame's pixel there if that is background (the sky behind the cow is still there before the cow
  arrives), or else the nearest background pixel of its row. Never black, never one flat color. frames (same shade, same color) do not take part: they stay, and the travelling pixels pass in front. The
  background (the color most pixels of the colorized frame have, or the darkest shade) stays where it is and is not
  taken along: only the picture moves, and the background of the other frame takes over at the halfway point. Dust does not need another frame: to black
  or white it leaves that color behind, to another frame it uncovers it.
- Pixelate and Zoom In keep every pixel, so nothing is taken away: they go through both frames.
  The start frame turns into blocks (or zooms in) and then the other frame comes out of the blocks
  (or zooms back out to full size). Their most frames is twice that of a transition to black.
- Fade cross-fades: every pixel blends from the colors of the start frame to those of the other
  frame. The most frames is how far the two frames' colors are apart. The original shades and the
  dynamic zones change over from the start frame's to the other frame's at the halfway point.
- The new frames lie between the two; the end frame itself is not copied.
- **Dynamic zones:** where both frames use the same dynamic set for different colors, the
  other frame's zones of that set are moved to a dynamic set nobody uses, with that set's colors, so
  the first frame's set gives way to the other frame's. The plugin tells you which sets it moved. If no
  set is free, those zones are left out of the new frames, and it says so. In a cross-fade a pixel
  changes from its zone in the start frame to its zone in the other frame at the halfway point,
  because a pixel can be in only one zone.
- **Color rotations** are removed from the new frames, and the new frames keep the start frame's
  background and settings. The two frames must be the same size (and both have, or both lack, an HD
  picture).

## What it does not do (yet)

- It works on the original frame and the one colorized picture; with an HD plane the plugin
  moves it in 2x2 blocks, as the editor's HD plane works.
- **Backgrounds:** where a frame shows its background, those pixels stay as they are, because the
  plugin API cannot change a frame's background mask. The plugin tells you when the frame has one.
- Pixels a transition has taken away are black (or white) in the frame's original shades and in the pictures, and they are taken
  out of the dynamic zones. In moving transitions the dynamic zones move with the picture.
- **Fade:** the colors of the picture and the frame's dynamic colors fade. The original shades
  stay as they are (a dynamic zone colors a pixel by its shade, so fading them would step through the
  zone's colors as well as fading those); the new frames of a fade therefore have the same original
  frame, which is fine for frames you put in a scene. Color rotations of the new frames are removed
  (they would keep turning the colors at full brightness).

## Adding another transition

Everything about a transition is in `src/Transitions.h`. A transition is a class (an `Effect`)
that answers, for a pixel (x, y) of the new frame at step N of M: which pixel of the starting
frame does it show, or has it been taken away (black or white)? It also says its most frames (`maxSteps`). Add one entry to
`kTransitions` (with its options) and it appears in the window. Nothing else changes: the same
answers are used for the original shades, the pictures, the dynamic zones and the preview
(`src/Preview.h`).

## Files

- `plugin.cfg`: what the editor reads to find the plugin.
- `src/TransitionFX.cpp`: the plugin (menu entry, the window, creating the frames and the
  scene).
- `src/Preview.h`: drawing the steps of a transition, for the new frames and the preview.
- `src/Transitions.h`: the transitions.
- `include/`: the editor's plugin API (`ColorizingDMDPlugin.h`, version 10, from
  PPUC-Colorizing-DMD) and the VPX plugin SDK header (`plugins/MsgPlugin.h`).
- `tests/`: tests that run the plugin against a fake editor.

## License

GPL-3.0-or-later, like the editor and the headers in `include/`.

## Some Sample Transitions

Alternating Vertical

<img width="640" height="160" alt="Alternating Vertical" src="https://github.com/user-attachments/assets/32a995df-28ce-43e9-b159-f15fd101a3e9" />

Blinds Vertical

<img width="640" height="160" alt="Blinds Vertical" src="https://github.com/user-attachments/assets/68321ea2-b4f9-40b2-806e-21fcf7516a78" />

Dissolve Random

<img width="640" height="160" alt="Dissolve Random" src="https://github.com/user-attachments/assets/de1702bd-4480-46a6-a8f1-e742f7af4556" />

Fade to Black

<img width="640" height="160" alt="Fade to Black" src="https://github.com/user-attachments/assets/d2c3a01f-6750-4d15-b913-a0fae9bedea1" />

Slide Right

<img width="640" height="160" alt="Slide Right" src="https://github.com/user-attachments/assets/93945a2b-2381-43cc-9f6f-dac6f384c864" />

Split Vertical

<img width="640" height="160" alt="Split Vertical" src="https://github.com/user-attachments/assets/4b05fa97-1550-47e6-95ad-cbb18aa8aed4" />

Wipe Centre-Out

<img width="640" height="160" alt="Wipe Centre-Out" src="https://github.com/user-attachments/assets/517f777c-b3b3-47ae-938a-00be3ded432a" />

Wipe Spiral

<img width="640" height="160" alt="Wipe Spiral" src="https://github.com/user-attachments/assets/66c894c3-2344-43f0-83c3-ad7f8366417b" />

## Languages

The window follows the editor's language (editor API version 11, `GetLanguage`): English, German
and French. The words follow the editor's own translations. An unknown language, or an editor
before version 11, gives English. The texts are in `src/Translations.h`, English first; a text
without a translation is shown in English, and a test checks that every transition's texts
have both.

## Version

The window's title shows the version (`TransitionFX v1.02`), and the files the build makes carry it (`TransitionFX-windows-x64_v1.02`). It is `kVersion` in `src/Version.h`;
raise it by 0.01 with every update merged into the main branch, and keep `version` in `plugin.cfg`
the same.
