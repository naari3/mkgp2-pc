# High-resolution 2D textures

An option of the PC port: the game's 2D pictures (text fonts, sprites, HUD,
menu art) are shown from enlarged copies instead of the textures on the
disc. Off by default; the original look stays what the port shows unless
asked. 3D model textures are not touched.

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

1. Make the folder (once, several minutes; about 650 MB):

   ```powershell
   .\port\docker\wsl-hires.ps1 -Iso 'C:\path\Mario Kart Arcade GP 2 (Japan).iso' -Distro Ubuntu-20.04
   ```

   It runs `port/hires/make_hires.py` in the build container (WSL and
   docker, as `wsl-build.ps1`) and writes `build\pc-gfx-win\dist\hires`,
   next to `mkgp2-gfx.exe`. `-Scale 4` makes the sprites four times instead
   of two (for windows larger than 1280x960; four times the disk space and
   time). On Linux call the script directly:
   `python3 port/hires/make_hires.py --iso <image> --out <folder>` (numpy,
   Pillow; `xxhash` makes it faster).

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

### The generator (port/hires/make_hires.py)

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
made as above either way. The plumbing was tried with a stand-in enlarger;
**no neural enlarger has been run through it yet** (it cannot be run on the
cloud machine; it would run on the user's PC, on their decision), so its
result on this game's art is unseen.

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
