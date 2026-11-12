# Anti-aliasing

Two options smooth the stair-step edges of the 3D picture. Both are off by
default, both use what the rendering library (Aurora) already offers, and
no game source is touched. Code: `port/gfx/video_aurora.c` ("Anti-aliasing").

| variable | launcher (表示, アンチエイリアス) | what it does |
|---|---|---|
| `MKGP2PC_MSAA=4` | MSAA 4x | Four samples per pixel along the edges of polygons (`AuroraConfig.msaa`). WebGPU has the counts 1 and 4 only; any other value is taken as 4. |
| `MKGP2PC_SSAA=F` | SSAA 2x (`F` = 2) | The frame buffer is `F` times the window's width and height (1 < F <= 4, fractions allowed; `VISetFrameBufferScale`), and each window pixel is the mean of the frame buffer pixels under it (`aurora_set_resampler(SAMPLER_AREA)`). |

The launcher's fourth choice sets both. `mkgp2-pc.ini` keeps the choice as
`aa` (0 none, 1 MSAA, 2 SSAA, 3 both).

## What each one reaches

- MSAA smooths the outlines of polygons: kart and character silhouettes,
  course geometry against the sky, guard rails. It does nothing inside a
  polygon, so cut-out textures (fences, leaves, tyre spokes drawn by alpha
  test) and shimmering texture detail far away stay as they are. Cost:
  small.
- Supersampling smooths everything, cut-out textures and texture shimmer
  included, and the 2D parts (HUD, menus, text) are drawn at the larger
  size and brought down with the rest. Cost: `F` x `F` times the pixels; at
  1920x1080 with factor 2 the game draws 3840x2160.

## What follows from the choice

- The game's copies of the frame buffer into textures (kart shadow, the
  boot screens' still picture, the camera picture) are taken from the
  resolved picture under MSAA and, as before, scaled to the size the game
  asked for; nothing in the port's handling of them changes.
- With MSAA the library cannot read the frame buffer back, so the picture
  of an F9 dump comes from the window instead (Windows: `shot_win32.c`; on
  Linux there is then no `frame.png`). With supersampling `frame.png` is
  the large frame buffer, not the window's picture.
- The library refuses depth copies from a multisampled frame buffer
  (fatal). None was met in the runs below: the copies seen in the sources
  are colour ones (RGB565 for the still pictures, a one-channel format for
  the shadow); `tobj` and the camera picture pass a format on from their
  data. `GXPeekZ` is a stub in the port.
- The list of prepared pipelines (`initial_pipeline_cache.db`,
  census.md section 36) does not depend on the sample count: the entries
  are compiled for the frame buffer in force at start-up. The compiled
  results kept by Dawn (`dawn_cache.db`) do, so the first start after
  switching MSAA on or off takes as long as a first start.

## State

2026-11-12, 32-bit Linux build under Xvfb (software OpenGL ES, so nothing
is said here about speed): the attract demo paused at the same retrace
(5000, a kart close up; 10500, the demo race) with none, MSAA, SSAA 2 and
both, window 640x480, pictures taken from the X screen. The four pictures
of a retrace show the same frame; edges are smoothed as described above;
HUD text, the card notice, the kart shadow and the dust sprites are in
place in all four. Log lines at start-up:

```
anti-aliasing: MSAA 4x, supersampling 2
anti-aliasing: frame buffer 1280x960 for a window of 640x480 (factor 2)
```

Not yet run on Windows (Direct3D 12), where the cost and the first start
after a change are still to be seen. The launcher's new item was compiled
with MinGW only. Not tried together with the persistent display copy of
the port menu (`MKGP2PC_XFB`, pull request 17, not on main at the time).
