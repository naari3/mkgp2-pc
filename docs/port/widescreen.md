# 16:9 display

## State (2026-11-12): implemented as an option, checked on Linux only

`MKGP2PC_ASPECT=16:9` (any `W:H` wider than 4:3, up to 4:1; `16:10`, `21:9`)
turns the wide picture on. Unset or `4:3` is the picture as before, and the
default. The settings program has the item (表示 page).

- Window: height = 480 x `MKGP2PC_SCALE`, width = height x W / H rounded
  down and then up to an even number. `MKGP2PC_SCALE` takes fractions now:
  1 = 854x480, 1.5 = 1280x720, 2.25 = 1920x1080, 3 = 2560x1440.
- The 3D scene fills the whole width at the same vertical field of view
  (more is visible to the left and right). Culling follows.
- The 2D layer (HUD, menus, text) keeps its shape and its 4:3 layout in the
  middle of the screen and is clipped to that 4:3 area.
- Except while it moves across the left or right edge (2026-11-12, after
  the user's report that UI leaving towards the screen edge vanished in the
  middle of the screen): a 2D primitive that reaches beyond the edge of the
  4:3 area is moved outwards by (share of its width beyond the edge) x
  (width of a side area), so it is where the game put it while inside and
  just off the real screen when the game has it just off the 4:3 one. One
  that is as wide as the 4:3 picture is stretched instead, wherever it is.
  A primitive that has rested in the same place for more than 6 frames is
  clipped as before: layout pieces that rest across the edge (the left end
  of the title bars) would otherwise come apart.
- Whole-screen 2D (fades, the saved frame of transitions, full-width
  backgrounds and bars) is stretched over the full width. Tiled backgrounds
  (the star pattern behind the menus) get their edge columns repeated out to
  the sides.
- Scenes whose background is neither (a 3D model, or pieces that do not
  form a grid) show the clear colour, usually black, at the sides.

Code: `port/gfx/gx_shim.cpp` ("Wide picture"), `port/gfx/video_aurora.c`
(`wide_frame`, window size), wrapped names in `port/tools/link_census.py`.
No game source is changed and there is no source patch.

How it works, in one paragraph: Aurora's viewport policy "stretch" spreads
the game's 640x480 over the wide frame buffer. Every projection the game
sets while its viewport is the full screen is then narrowed in x by
`k = (width / height) / (4 / 3)` about the centre, which puts the undistorted
4:3 picture in the middle; since nothing clips the 3D scene at the 4:3
edges, it continues into the side areas. The camera is not touched, so
`GXProject` positions (rival face markers) still match the 2D layer. The
game's culling frustum gets its aspect multiplied by `k` (wrapper around
`fn_8013879C`). 2D primitives are held back until complete, classified by
their horizontal extent, and sent with the stretched or the narrowed
projection. `GXGetProjectionv` and `GXGetScissor` answer with the game's own
values.

Checked (2026-11-12, 32-bit Linux, Xvfb with software GL, a recorded play
session replayed in both shapes and compared frame by frame at 24 points:
class, character, cup and course selection, a full Yoshi Park race):
4:3 output unchanged; 16:9 menus centred with the star background filling
the sides; race in 16:9 with the HUD in the middle.

**Not checked: Windows.** The code is the same on both systems, but it has
not been built or run there, and D3D12 has not seen the "stretch" policy
from this port yet.

Known gaps:

- In the race, a black area can show at a side where the course has no
  ground beside the track (seen in Yoshi Park next to a fence, lower right).
  Widening the culling frustum three times more (`MKGP2PC_WIDE_CULL=3`) did
  not bring anything back there, so it is not the frustum test; whether the
  game leaves that ground out by another rule (course section visibility)
  or the course simply ends there is not established.
- Sliding elements made of several pieces (the title bar swinging in) can
  show a piece apart from the rest for a few frames, because each primitive
  is moved by its own share; and a piece that comes to rest across the edge
  goes back to its clipped place when it stops.
- The HUD is not moved to the screen edges (option C below).
- View-blocking item effects and other whole-screen effects have only been
  seen as far as the recorded session shows them.
- Draws that reach the renderer as display lists under an orthographic
  projection (HSD-drawn 2D) are centred and clipped but never stretched.
- `MKGP2PC_RESIZABLE=1`: the factor follows the window's shape every frame,
  so any shape works in principle; resizing itself still crashes on Windows
  (build.md).

`MKGP2PC_TRACE_WIDE=1` prints how each held-back primitive was classified.

The rest of this file is the survey written before the implementation.

---

## Census of screen-space drawing (2026-11-12)

Asked for by the user after the dash effects were found cut at the 4:3
edge: instead of fixing effects one at a time, every way the game draws in
screen space is listed here with the rule that applies to it. The rules
work on what is drawn (its extent on the screen), not on which effect it
is, so an effect that was never seen is still covered if it comes through
one of these paths.

Tool: `MKGP2PC_WIDE_CENSUS=1` prints, for every place that sets a
projection, what was drawn under it (a line when first seen, the table
every 1000 retraces; addresses are offsets from the load address, look
them up with `nm -n`). `MKGP2PC_TRACE_WIDE=1` prints every 2D primitive and
display list with its extent and the retrace.

### Who sets a projection

| Set by | Kind | What is drawn under it | Wide picture | Seen in a run |
|---|---|---|---|---|
| `setupNormalCamera` (HSD camera) | perspective, whole screen | the 3D scene: course, karts, demo scenes | widened, culling follows | yes |
| `fn_801F6790` (`GX_BeginDraw`: the scene's projection put back after 2D) | perspective, whole screen | 3D effects and particles drawn by game code, as primitives and display lists | widened like the scene | yes |
| `fn_8011EC18` (`SpriteHandle.cpp`, sprite and glyph layer) | 2D, whole screen | menus, HUD, text, backgrounds, fades | 2D rules below | yes |
| `fn_801F6588` (`GX_EndDraw`; from `FrameSelection_Methods.c`, `Scene_8014E0C4.cpp`, `fn_801F0CBC.cpp`, `NamCamFlow.cpp`, `Scene_801721B0.cpp`) | 2D, whole screen | the overlay of a race: sprites, and models as display lists (the tunnel and rays of a dash) | 2D rules below, display lists included | yes |
| `ScreenProjection` (`TransitionEffect.cpp`) | 2D, whole screen | scene transitions: the saved frame, the roulette | 2D rules below | yes |
| `DisplayContext_Flush` | 2D, whole screen | boot and service text | centred, clipped (its vertices are not floats, so the rules do not look at it) | boot only |
| `DrawColoredQuad`, `fn_8015376C` and its sibling (`Scene_8014E0C4.cpp`) | 2D, whole screen | coloured quads and texture fade panels | 2D rules below | no |
| `CardCreate_Page.cpp` | 2D, whole screen | card creation pages | 2D rules below | no (no card in the runs) |
| `DbgMenu.cpp` | 2D | debug menu | centred | no |
| `setupOffscreenCamera`, `HSD_ShadowStartRender`, `DrawNamCamImage` | part of the frame buffer | kart shadow image, pictures rendered for textures | not touched | yes |

### 2D rules (whole-screen 2D projection)

In the order they are tried, per primitive (a quad, a triangle, a whole
strip) or per display list:

1. Edge tile of a grid (three or more equal quads in a row, at most a
   quarter of the picture wide): drawn again outwards. Menu star pattern.
2. As wide as the 4:3 picture or wider, wherever it is: stretched over the
   whole width. Fades, saved frames, bands, and the dash model (2.5 units
   wide, it overfills the arcade screen too).
3. Part of a row: consecutive quads of the same height that continue each
   other from the left edge to the right edge. Whole-screen images cut
   into textures are drawn like this (640 = 512 + 128). Stretched together.
   Found in the census: the band of cup and round select, the background of
   the screen before a race.
4. Moving and reaching beyond the left or right edge: moved outwards (see
   above).
5. Everything else: its shape and place in the centred 4:3 picture,
   clipped at its edges.

Rules 1 and 3 need the neighbours, so they act from the second frame a
picture is on the screen.

### What this does not reach

- A 3D effect that the game sizes to the edges of the 4:3 view (something
  fixed in front of the camera) would end at the 4:3 edge. None was
  identified; the dash effect was thought to be one and is not. It cannot
  be told apart from scenery by the wide code.
- 2D that touches one edge only and rests there (title bars, HUD panels):
  rule 5, so it ends at the 4:3 edge. Deliberate: these are layout, and
  stretching a panel that is 26 pixels over the edge distorts it.
- A whole-screen effect made of pieces that are not a row (left and right
  halves at different heights, a frame of four borders) would stay
  centred. None seen.

### Seen and not seen

Seen in replays on Linux (recorded session, Yoshi Park, no card): attract
logo and demo, class, character, cup and round select, the screen before a
race, countdown, race HUD, the dash panel effect.

Not seen, covered only as far as the rules reach: start dash, mushroom
and other item effects on the screen (the ones that block the view), hits
and spins, goal, results and ranking, continue, name entry, card pages,
time attack and versus screens, link screens, test menu, endings.

# Survey (2026-11-11)

Question: what does it take to show the PC port in 16:9 instead of the
arcade's 4:3? This is a reading of the sources (mkgp2-pc `2502a6b`, Aurora
`f77492f`). **Nothing here has been built or run yet**; items marked
"unverified" need a run to confirm.

Factor used below: `k = (16/9) / (4/3) = 4/3`. A 4:3 picture centred in a
16:9 frame takes the middle 75 % of the width; each side strip is 12.5 %.


## 1. What exists today

| Layer | State | Where |
|---|---|---|
| Window | fixed 640x480 at start | `port/gfx/video_aurora.c` (`config.windowWidth/Height`) |
| Aurora frame buffer | follows the window's pixel size; the game's 640x480 viewport and scissor values are "logical" and are scaled to it | `lib/gx/gx.cpp` `map_logical_viewport`, `map_logical_scissor` |
| Aurora aspect policy | `AURORA_VIEWPORT_FIT` (default): the content frame buffer keeps 4:3 and is centred in the window. `AURORA_VIEWPORT_STRETCH`: the frame buffer takes the window's aspect, logical 640x480 covers all of it | `AuroraSetViewportPolicy`, `include/dolphin/gx/GXAurora.h` |
| EFB copies | source rectangles are logical too and are scaled the same way | `lib/dolphin/gx/GXFrameBuffer.cpp` |

So a wide window today gives a 4:3 picture with bars (FIT), and one call
(`AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH)`) gives a stretched,
fat picture. Neither needs game changes. A true 16:9 picture is "STRETCH,
then undo the stretch in the game's projections".

Resizing or maximising the window currently crashes inside the rendering
library (open, other thread), so a first version should start the window at
a 16:9 size (1280x720) rather than rely on resizing.

## 2. Game side: where the picture's shape is decided

### 3D (perspective)

Every perspective projection goes through the HSD camera object
(`src/sysdolphin/baselib/cobj.c`); game code never calls `MTXPerspective`
itself except `DrawNamCamImage.c` (camera photo, drawn to a texture).

- The aspect is a field of the camera (`projection_param.perspective.aspect`),
  loaded from the camera description in each scene's archive
  (`HSD_CObjLoadDesc` -> `HSD_CObjSetPerspective`). Camera animation has
  tracks for eye, interest, roll, fov, near, far, and none for aspect.
- The race re-sets the projection once at scene start with the camera's own
  aspect read back (`Scene3D_GetCameraPos` -> `Scene3D_SetupProjection`, 12
  callers: race, time attack, VS, the challenge modes). Nobody passes a
  literal aspect.
- The value stored in the archives is unverified (expected 4/3; print
  `HSD_CObjGetAspect` once at run time).

### Culling

The game culls with its own view frustum, four side planes built from
`tan(fovy / 2)` and the camera's aspect: `fn_8013879C`
(`CourseObjects_801375D8.cpp`), called from `Scene3D_Init`,
`Scene3D_SetupProjection` and `CObj_UpdatePerspParam`, all with
`HSD_CObjGetAspect(cobj)`. Course objects, karts (`KartDriver_Render`),
effects (`DrawEffect_TickAndCull`) and the occluder tests all use this one
frustum (`SphereInViewFrustum`).

Consequence: if only the projection matrix is widened (what an emulator
widescreen hack does), objects pop in and out inside the side strips. If the
camera's aspect itself is widened, projection and culling follow together.
This is the main reason to change the camera rather than the matrix.

### 3D point to screen position

`CObj_ProjectPoint` (`CObjProject.c`, `GXProject` with the current
projection and viewport) gives 640x480 screen coordinates for a world point.
Three users: the rival face marker (`EnemyFace_exact2.cpp`),
`Scene_801734B0.cpp`, `Scene_80172844.cpp`. When the 3D is wide and the 2D
layer is not, the result has to be converted
(`x' = 320 + (x - 320) * k`) or these markers drift away from their karts
toward the centre.

### 2D (orthographic)

All 2D is drawn in a 640x480 coordinate space through a handful of set-up
functions:

| Set-up | Ortho box | Used for |
|---|---|---|
| `fn_8011EC18` (`SpriteHandle.cpp`) | constants `lbl_806D6DBC..C8` | sprite layer: HUD, menus |
| `DisplayContext_Flush.c` | 0..640 x 0..480 | text |
| `OverlayViewSetup.cpp` | constants | overlays |
| `Scene_8014E0C4.cpp` (3 places) | 0..640 x 0..480, and 0..608 x -23..448 | scene panels |
| `TransitionEffect.cpp` | +-320 x +-240 | screen transitions (uses the saved frame) |
| `CardCreate_Page.cpp`, `DbgMenu.cpp` | +-320 x +-240 | card page, debug menu |
| HSD cameras of ortho/frustum type (`OrthoCam.c`) | from data | 2D-like scenes drawn as models |

`640.0f` / `0x280` appears 184 times in 41 game files: positions are
absolute everywhere, there is no layout layer to re-anchor.

The art is 4:3: menu backgrounds, the title, result screens are 640x480
pictures. They cannot fill 16:9 without stretching or new art.

### Whole-screen copies and effects

- `Frame.c` and `BootDispatcher.cpp` copy the whole 640x480 frame into
  `g_backupBufferTexture`; `TransitionEffect.cpp` draws it back in three
  places (scene transitions, kart roulette overlay).
- HSD shadow: drawn in a corner of the frame buffer and copied out
  (`shadow.c`; already limited to 480x480 by the port, see `gx_shim.cpp`).
  Must be left alone by any widescreen correction.
- `DrawNamCamImage.c`, `CardCreate_Page.cpp`, `tobj_nm.c`: render to
  texture with their own viewports. Also to be left alone.
- Fades, flashes and the item effects that are meant to cover the view are
  2D quads of exactly 640x480. With a centred 2D layer they leave the side
  strips uncovered. How many such effects there are is unverified; they
  have to be found by playing.

## 3. Options

### A. Stretch everything

STRETCH policy and a 16:9 window. No game change. Everything is 33 % too
wide. Useful only as a first check that the policy and a 1280x720 window
work.

### B. Wide 3D, 4:3 2D layer in the centre (recommended first step)

1. Window 1280x720 (or the desktop size), STRETCH policy.
2. Camera aspect times `k` where the camera is loaded (wrap
   `HSD_CObjLoadDesc`, or `HSD_CObjSetPerspective` called from it): the
   picture gains width at the same vertical field of view, and the culling
   frustum follows because it reads the same field.
3. 2D: every orthographic projection gets its x scaled by `1/k` about the
   centre, in the port's `GXSetProjection` wrapper (already wrapped in
   `gx_shim.cpp`). Shapes stay correct; the HUD sits in the middle 75 %.
4. `CObj_ProjectPoint`: convert x as above.
5. Exemptions: draws whose viewport is not the full 640x480 (shadow image,
   camera photo, card page) are not corrected.
6. Whole-screen quads (fades, saved-frame transitions, view-blocking
   effects): widen to the full frame, one by one as they are found.
7. Menus and other scenes without a race camera: keep the whole picture
   4:3 in the centre with black bars (correct both 3D and 2D by `1/k`),
   switched by "is a race scene active". Their backgrounds are 4:3 art.

All of it fits in mkgp2-pc: `port/gfx/gx_shim.cpp` wrappers
(`GFX_WRAPPED` / `GAME_WRAPPED` in `port/tools/link_census.py`) plus at
most a few `port/patches/`. Nothing goes to mkgp2-decomp.

Pitfalls seen in the source:

- `GXGetProjectionv` / `GXGetViewportv` are used to save and restore state
  (`SpriteHandle.cpp`, `DisplayContext_Flush.c`, `fn_801F6790.c`) and by
  `CObj_ProjectPoint`. A wrapper that alters the matrix on the way in must
  hand back the game's own values on the way out, or the correction is
  applied twice.
- Changing the camera's stored aspect is stable against the game's
  read-back-and-set at scene start; multiplying in a getter instead would
  compound.
- The dump tool's item shadow record assumes a 640x480 picture
  (`gx_shim.cpp`).
- The aspect should be taken from the frame buffer's real size
  (`aurora_get_window_size`), so that other shapes (16:10, 21:9) come for
  free once resizing is safe.

Size: small. Steps 1 to 5 are a few dozen lines; 6 and 7 are found and
fixed while playing.

### C. HUD anchored to the screen edges

After B: move the left HUD groups (position, lap, minimap ...) to the left
edge and the right groups to the right edge instead of keeping them in the
centre block. There is no layout layer, so this is per element: either a
rule in the sprite set-up by x range (fragile) or patches at each HUD
group's draw. Cosmetic; worth deciding after seeing B.

### D. Wide menus

Needs new or extended background art, or stretching. Not recommended.

## 4. Recommendation

B, with the race in 16:9 and everything else 4:3 centred, as an option that
can be switched off (environment variable or key; 4:3 stays the default
until it has been looked at on a real screen). Then decide on C.

Open points to settle with a run: the aspect value in the archives; that
STRETCH with a 1280x720 window is stable on Windows; the list of
whole-screen 2D effects in a race.
