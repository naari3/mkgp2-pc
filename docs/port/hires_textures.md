# High-resolution 2D textures

An option of the PC port: the game's 2D pictures (text fonts, sprites, HUD,
menu art) are shown from enlarged copies instead of the textures on the
disc. Off by default; the original look stays what the port shows unless
asked. 3D model textures are not touched.

State 2026-11-12: written and tried on 32-bit Linux only (software GL under
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

1. Make the folder (once, about two minutes; about 700 MB):

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

1. Level smoothing. RGB5A3 has 16 levels per colour where a texel is
   translucent and 32 where opaque, RGB565 32/64, palettes are RGB5A3 or
   RGB565; gradients come out in steps. Each channel is moved toward the
   average of its neighbourhood, never by more than half a level of its
   format: a step that the format forced disappears, an edge that was in
   the art (taller than one level) stays.
2. Texels that are fully transparent and black get the colour of their
   neighbourhood, so that the enlargement does not draw a dark rim into
   every outline. (The original shows such rims where the game filters
   across them; this is the one place the result differs on purpose.)
3. Lanczos enlargement, clamped to the smallest and largest value of the
   source's 3x3 neighbourhood (no ringing).
4. Edge tightening: within a strong edge the value is pushed toward the
   nearer side. Off for CMPR and weak for RGB565 (rendered pictures, where
   it would bring the compression blocks out).

This is a careful classical filter, not a redrawing: outlines and gradients
are cleaner, detail that was never in the 640x480 art does not appear.

**Text fonts** (`tpl2_sysFONT_16_*`, `tpl2_sysFONT_24_*`: one 16x16 or 32x32
IA4 texture per character, intensity = the shape, plus an `_a` texture with
the grown shape that is drawn underneath as the outline; 652 and 654
characters):

- Kana and kanji are drawn anew from an outline font, M PLUS Rounded 1c
  ExtraBold (SIL Open Font License; downloaded from the Google Fonts
  repository on first use, kept in `--cache`; `--font` takes another file,
  `--no-font` skips this). The game's font is a rounded gothic of very
  similar design. The size is fitted per font from the kanji (median ratio
  of the original ink box to the drawing's), each character is centred on
  the ink box of its original bitmap, so text sits where it sat. The outline
  texture is the drawing grown by the reach whose ink matches the original
  outlines.
- Latin letters, digits and signs are of another, heavier design in the
  game (an oblique bold in the 24 pixel font). They keep their own shapes:
  the bitmap is enlarged and its soft edge narrowed.
- Always four times with two mip levels (the textures are tiny).
- The 64 pixel font and the atlas `gfont.tpl` are treated as sprites.

Measured on the cloud machine (4 cores): whole disc at x2 in 90 s, 10,500
files, 660 MB.

### An outside enlarger for the sprites

A neural enlarger (Real-ESRGAN and the like) gives more than the built-in
filter on this kind of art, at the price of a slightly redrawn look. The
generator can hand the sprites over and take the result back:

```sh
python3 port/hires/make_hires.py --iso <image> --export-sprites work
#   work/rgb/<name>.png    colour, opaque (levels smoothed, transparent texels filled)
#   work/alpha/<name>.png  alpha as a grey picture, only where it varies
# ... enlarge both folders 2, 3 or 4 times into up/rgb and up/alpha, same file names ...
python3 port/hires/make_hires.py --iso <image> --out hires --import-sprites up
```

Textures without a picture in `up` get the built-in filter; the fonts are
made as above either way. The plumbing was tried with a stand-in enlarger;
**no neural enlarger has been run through it yet**, so its result on this
game's art is unseen.

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
