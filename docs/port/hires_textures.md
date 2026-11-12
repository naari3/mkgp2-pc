# High-resolution 2D textures

An option of the PC port: the game's 2D pictures (text fonts, sprites, HUD,
menu art) are shown from enlarged copies instead of the textures on the
disc. Off by default; the original look stays what the port shows unless
asked. 3D model textures only on request (see below).

State 2026-11-12 (second version, after the user rejected the first: see
the generator section): written and tried on 32-bit Linux only (software GL under
Xvfb, window 1280x960, a recorded play replayed with and without the
option through the menus into a race: the pictures are replaced, positions
and sizes stay; also together with `MKGP2PC_ASPECT=16:9`). The glue compiles
for the Windows target; **the Windows executable has not been linked or run
with it yet.**

One run out of eight with the option on ended in a segmentation fault
during the attract race (between retrace 4500 and 6200; three copies of the
game were running on the machine at the time, no debugger attached). Five
further runs through the same part, four of them in parallel under gdb,
went through. Whether the option had a part in it is not known.

The enlarged textures are derived from the game's data. They are made on the
user's machine from the user's own disc image and must never be committed or
uploaded.

## Using it

1. Make the folder (once; several minutes; 650 to 850 MB). In the settings
   program: 表示 page, button 作成... next to the check box. The window
   that opens offers the method (内蔵フィルタ, or AI with one of two
   Real-ESRGAN models) and the size (2 or 4 times), shows the progress and
   can stop the run. It takes the disc image named on the 基本 page and
   writes the folder `hires` next to the programs. Choosing AI asks once
   before it downloads the enlarger (see "An outside enlarger").

   The window only starts `mkgp2-hires.exe`, which sits next to the game
   and can be used alone:

   ```
   mkgp2-hires.exe --iso <image> --out hires [--scale 4]
   mkgp2-hires.exe --iso <image> --out hires --ai realesrgan-x4plus-anime --work hires-ai --download
   ```

   On Linux build it with
   `g++ -std=c++17 -O2 -pthread -o mkgp2-hires port/hires/mkgp2_hires.cpp`.
   `port/hires/make_hires.py` (numpy, Pillow) is the same work in Python,
   the version the filters were developed in; `port\docker\wsl-hires.ps1`
   runs that one through WSL and docker.

2. Switch it on: the check box on the settings program's 表示 page, or
   `MKGP2PC_HIRES=1`. The game reads the folder `hires` under the folder it
   is started from; `MKGP2PC_HIRES_DIR=<folder>` names another. Without the
   folder the game says so in the log (`hires: no folder ...`) and runs with
   the original textures.

3. `F8` (key action `hires` of `MKGP2PC_KEYS`) switches the textures off and
   on while the game runs, for comparing.

The difference only shows when the window is larger than 640x480
(`MKGP2PC_SCALE` 2 and up): at 640x480 one texel of the original already
covers one pixel.

## How it works

### In the game (port/gfx/gx_shim.cpp)

Aurora has texture replacement built in (`include/aurora/texture.hpp`,
`lib/gfx/texture_replacement.cpp`), with the file naming of Dolphin's custom
textures: `tex1_<width>x<height>_<hash of the texels>[_<hash of the
palette>]_<GX format>.png`, hashes XXH64 of the texture's first level and of
the part of the palette the texture uses. `MKGP2PC_HIRES` calls
`aurora::texture::load_replacement_directory`; the key toggles with
`unregister_replacements`. A replacement may be any size: texture
coordinates are relative, so nothing in the game's drawing changes. Files
are read on worker threads when a texture is first used; until one has
arrived the original is shown.

Mip levels: a file `<name>_mip<N>.png` beside the base picture is level N.
Aurora samples replacements through all their levels even when the game's
texture has none, so a picture four times the original does not shimmer
when drawn at a smaller size. The generator writes levels down to twice the
original size (at twice, bilinear sampling of a 2:1 reduction is already an
exact average).

Because the naming is Dolphin's, a texture pack made for Dolphin from the
same disc should load as well (not tried).

### The generator (port/hires/mkgp2_hires.cpp, port/hires/make_hires.py)

Two programs that do the same. `mkgp2_hires.cpp` is the one handed out
(`mkgp2-hires.exe`, built by `port/hires/build.sh` in the Windows build): one
C++ file plus `stb_image.h` and `stb_image_write.h` (public domain) for PNG
and zlib, no Python, nothing of the game or the port linked in. The user
asked for it on 2026-11-12: the enlarged pictures may not be handed out, so
every player has to be able to make them. `make_hires.py` came first and is
kept for experiments. The description below holds for both.

Checked against each other on the whole disc (Linux, 2026-11-12): the same
10,518 file names; of 400 files compared, all but two agree to within
rounding (the two are one character, ☆ of the 16 pixel font, whose likeness
to the 24 pixel one is exactly on the threshold). The C++ program takes 6
to 7 minutes on 4 cores, as the Python one; its PNG files are larger (850 MB
against 630 MB).

`mkgp2-hires.exe` prints lines for the settings program: `stage <name>`,
`progress <0-100>`, `note <text>`, `error <text>`, `done <folder>`. It
writes into `<folder>.making` and renames at the end, so a run that is
stopped leaves the old folder untouched. It is a 32-bit program like the
game: at most 6 million source texels are in work at a time (about 100
bytes of memory each), and it is linked large-address-aware.


Reads the disc image directly: the file table, every `*.tpl` (plain, or 8
bytes followed by a zlib stream), and three tables of `main.dol` (the sprite
resource table and its path table, and the character table `lbl_804E5590`:
Shift-JIS code and the resource of the 16, 24 and 64 pixel glyph).

**Sprites, HUD, menus** (about 2,400 textures, 230 million texels; formats
C8, IA4, CMPR, RGB5A3, RGBA8, RGB565, IA8):

1. Texels that are fully transparent and black get the colour of their
   neighbourhood, so that the enlargement does not draw a dark rim into
   every outline. (The original shows such rims where the game filters
   across them; this is the one place the result differs on purpose.)
2. Bicubic enlargement at four times, clamped to the smallest and largest
   value of the source's 3x3 neighbourhood (no ringing, no colour that was
   not there).
3. Edge tightening: where that neighbourhood spans an edge (more than 12
   levels, fully from 40) the value is pushed toward the nearer side, twice
   (smoothstep). Once and only on strong edges for CMPR and RGB565
   (rendered pictures, where it would bring the compression blocks out).
4. For `--scale 2` the result is averaged down to twice the size.

This keeps the picture's own shapes: outlines of flat-coloured art become
crisp instead of two blurred screen pixels wide, gradients stay gradients.
It cannot add detail that was never in the 640x480 art (the texture of a
rendered logo, small faces); only a neural enlarger invents that, see below.

**Text fonts** (`tpl2_sysFONT_16_*`, `tpl2_sysFONT_24_*`: one 16x16 or 32x32
IA4 texture per character, intensity = the shape, plus an `_a` texture with
the grown and offset shape that is drawn underneath as the outline; 652 and
654 characters). The characters are the game's own bitmaps enlarged, nothing
is drawn from another typeface:

- Bicubic enlargement four times, then the soft edge is narrowed around the
  half-coverage line (gain 3 for the character, 1.6 for the outline texture,
  which keeps most of its soft fall-off). Two mip levels.
- The outline texture is enlarged from the original outline texture, so its
  width and its offset to the lower right are the original's.
- A character of the 16 pixel font is too small to enlarge well (kanji
  strokes are one texel). Where the 24 pixel font has the same drawing, its
  bitmap is used instead, fitted ink box onto ink box so the character sits
  where and as large as the 16 pixel one: kana and kanji (574 characters;
  test: ink box proportions within 0.88 to 1.14 and overlap of the reduced
  picture with the original at least 0.80). Latin letters and digits are a
  different, narrower design in the 16 pixel font and are enlarged from
  their own bitmap, as are all characters of the 24 pixel font.
- The 64 pixel font and the atlas `gfont.tpl` are treated as sprites.

Two things of the first version (PR 15) that must not come back. It smoothed
the colour levels of the formats (RGB5A3 has 16 or 32 per channel, 8 of
alpha) by up to half a level: that erased art that is one level deep (the
faint stars of the menu background) and, on alpha, lifted transparent texels
near a shape enough to draw a box around every sprite. The code is still
there, off (`LEVEL_SMOOTHING`), colour only. And it kana and kanji from M PLUS Rounded 1c and grew
its own outline. The user rejected it the same day: the shapes and the
outline were not the game's. Do not bring a foreign typeface back.

Measured on the cloud machine (4 cores): whole disc at x2 in 7 minutes,
10,500 files, 630 MB.

### Textures of the 3D models (`--models`, C++ program only)

Off unless asked for (`mkgp2-hires --models`, the check box 3D のテクスチャも
in the settings program's window; the user asked on 2026-11-12 whether the
in-game textures can be done too). They go to `hires/models`; the game
reads the folder with its subfolders, so nothing changes on its side.

The textures are inside the model archives (`*.dat`, 1,918 files; HSD
format: 0x20 byte header, data, a table of the offsets that hold pointers;
most files are 8 bytes plus a zlib stream like the `*.tpl`). A texture is
an image record {pointer to texels, u16 width, u16 height, u32 format, u32
mip flag} that a texture object points at from offset 0x4C; the texture
object's wrap modes are at 0x34 and 0x38. The program looks at every pointer
of the table instead of walking the model tree. Found: 3,641 different
textures, 97 million texels (CMPR 2,937, RGB5A3 441, RGBA8 240, RGB565 50,
IA4 5; no palette formats), 265 with mip levels.

- Repeating and mirrored textures are extended by 4 texels of what the wrap
  mode puts beside them before enlarging, and cut back, so that no seam
  shows where two copies meet.
- Mip levels are written only where the original has them. A first try
  gave every model texture a level of the original size: the start banner
  of Yoshi Park then came out softer than the original, without the level
  it is as sharp as the original.
- Whole disc at x2: 2 minutes on 4 cores, 5,141 files, 580 MB.

Tried on Linux (replay into a race on Yoshi Park, window 1280x960): the game
runs and nothing is misplaced. **With the built-in filter the 3D picture is
practically the same as the original**: most of these textures are CMPR
(block compressed, rendered or painted pictures), where the filter is held
back on purpose, and they are seen through perspective and motion. The
user expected these to go through the neural enlarger (2026-11-12, "AI
拡大してないの？"), see `--ai-models` below. Memory of the 32-bit game
with a whole course's textures doubled has not been measured on Windows.

### An outside enlarger for the sprites

A neural enlarger (Real-ESRGAN and the like) is the only way to more than
crisp edges, at the price of a redrawn look. The generator can hand the
sprites over and take the result back:

```sh
python3 port/hires/make_hires.py --iso <image> --export-sprites work
#   work/rgb/<name>.png    colour, opaque (levels smoothed, transparent texels filled)
#   work/alpha/<name>.png  alpha as a grey picture, only where it varies
# ... enlarge both folders 2, 3 or 4 times into up/rgb and up/alpha, same file names ...
python3 port/hires/make_hires.py --iso <image> --out hires --import-sprites up
```

Textures without a picture in `up` get the built-in filter; the fonts are
made as above either way.

`mkgp2-hires.exe --ai <model> --work <folder> [--download]` does all of it
on Windows, and so does `wsl-hires.ps1 -Ai` with the Python program (the
user asked for this on 2026-11-12): export into `<work>\src`, download of
`realesrgan-ncnn-vulkan` v0.2.5.0 (BSD 3-clause; a Vulkan program, no Python
or CUDA; unpacked into `<work>\tool`, nothing installed; only with
`--download`, which the settings program passes after the player said yes),
both folders enlarged four times on the GPU into `<work>\up` (several GB,
deleted afterwards), import. The enlarger runs in a job object and dies with
`mkgp2-hires.exe`. Models:

- `realesrgan-x4plus-anime` (default; 6 residual blocks, 17 MB): trained on
  illustrations; suits flat-coloured art with outlines, which is most of
  this game's 2D (logos, icons, frames).
- `-Model realesrgan-x4plus` (23 blocks, 64 MB, several times slower):
  general pictures; suits the rendered pictures, tends to invent texture.

The program works in tiles, so video memory use stays at a few GB at most
(`-Tile` sets the size). Both models redraw: small text and faces can change
shape.

Run once on the user's PC through `wsl-hires.ps1 -Ai`, 2026-11-12 (RTX 3060
Ti, default model): 586 s
in all, of which 429 s enlarging (colour 2,694 pictures, alpha 1,286);
`build\hires-ai\up` 2.4 GB; pack 10,518 files, 723 MB. The user's first verdict: quite good, some
odd places (halftone dots in logo backgrounds vanish, soft translucent
edges turn wavy, gradients flatten). The cloud machine may not run
the downloaded program, so there is no picture of it from there.

`--ai-models <model>` (C++ program only) sends the 3D models' textures
through the enlarger too, with a model of their own; the settings program
passes `--ai-models realesrgan-x4plus` when an AI method is chosen and 3D
のテクスチャも is ticked, since these are rendered and painted pictures.
`--ai` and `--ai-models` are independent (one without the other leaves the
other kind to the built-in filter). So that tiling textures still join,
each is handed over with 8 texels of its surroundings on every side, taken
the way the game addresses it (repeat, mirror or clamp), in `models-rgb`
and `models-alpha`, and the margin is cut off again on import. That is
about 1.3 times the texels of the plain textures (97 million before the
margin), so expect the general model to take a multiple of the sprites'
time. With a stand-in enlarger the
imported pictures line up with the built-in ones (Linux, whole disc) and
all four folders pass through the Windows program under Wine (which found
that the enlarger was started with relative paths: fixed).

Run on the user's PC, 2026-11-12 (RTX 3060 Ti, `--ai-models
realesrgan-x4plus`, whole disc): 699 s, of which 480 s colour and 63 s
alpha of the 3,641 model textures; generator peak 2.1 GB, enlarger 1.3 GB;
`models` 5,141 files, 784 MB. In the game (Windows, 16:9, one indoor course
raced): private bytes peak 711 MB. The user: "まあいい感じ", and course
textures could go one step further.

`--scale-courses 4` is that step: the enlarger's output is four times the
original and was being halved; textures used by a course's own archive (name
of two or three capitals and an underscore, `YIA_cake.dat`,
`MR_highway_long_A.dat`; not `AT_`, the attract film) now keep it, unless a
side would pass 2048 texels. Textures the original gives no mip levels get
one here (the picture at twice the size), so the far parts of a course do
not shimmer and nothing is softer than the x2 pack. Yoshi Park's 74 course
textures go from 46 MB to 185 MB of texels. Tried on Linux with a stand-in
enlarger only; not yet with the real one, not yet in the game.

**`mkgp2-hires.exe --ai` itself (the download, the unpacking on Windows,
starting the enlarger) has not been run anywhere yet.** Tried: the archive
reader on Linux against `unzip` (same files), export and import with a
stand-in enlarger on Linux, the rest of the program under Wine (whole disc
with the built-in filter), and the settings program's window under Wine
(start, progress, stop).

## Known limits

- The boot and test-mode text (the console ROM font, for which the port has
  its own stand-in, `port/tools/gen_os_font.py`) is not covered.
- Sprites are cut out of atlases; the filters work on the whole atlas. With
  the built-in filter neighbours are too far apart to bleed; an outside
  enlarger may bleed a little across tightly packed sprites.
- Pictures the game makes itself (camera photo, frame copies) and model
  textures (`*.dat`) are not replaced.
- 2D-looking parts that are drawn as models take their textures from the
  model files and stay as they are.
