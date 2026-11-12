# The sign of plain `char`

Looked at on 2026-11-12, after the camera work found a comparison that is
true on the cabinet and never true on the PC
(`end[-2] == -1 && end[-1] == -0x27` in `NamCam_GetImageHandle.c`).

## What was wrong

The port compiled every game file with `-funsigned-char`, on the belief that
plain `char` is unsigned on PowerPC. That is not what the game's compiler
does:

- MWCC's own default is **signed** (`-char signed  # chars are signed;
  default` in the compiler's help). Checked with GC/1.3.2 and GC/2.6:
  without an option `return p[0] == -1;` compiles to `lbz; extsb; ...`,
  the same code as with `-char signed`. With `-char unsigned` the compare
  is on the zero-extended byte and can never be true, as on any conforming
  compiler. There is no MWCC quirk in this.
- The matching build passes `-char unsigned` only for the Triforce library
  (`src/triforce`, 71 objects) and for most of the Dolphin SDK. Everything
  else (`src/game`, `sound`, `ui`, `util`, `resource`, `init`, `sysdolphin`,
  `musyx`, `thp`, `zlib`, 2,470 objects) has signed `char`.

So the camera line was not an odd case of a narrow compare; the whole game
was built with the wrong reading of `char`, and that line was the place
where it showed.

## The fix

`port/tools/compile_census.py` `char_flag(path)` gives each file the sign
the matching build gives it: `-funsigned-char` for `src/triforce` and the
SDK directories built that way, `-fsigned-char` for the rest. All the
tools that compile game code (`link_census.py`, the census scripts) take
their flags from there. No source file changed, in either repository.

`python3 port/tools/char_sign_census.py --check-table <mkgp2-decomp>/build.ninja`
compares that table with the `-char` options of a configured matching
build: 2,694 objects, 0 mismatches.

## What the wrong sign changed

`python3 port/tools/char_sign_census.py` compiles every file twice, with
signed and with unsigned `char`, and lists the functions (and initialised
data) whose optimised code differs. `--lines` lists comparisons that are
constant under the wrong sign, from the compiler's own warnings.

Result over the 2,651 files of the port's directories (5 do not compile on
the host and are not part of the port): 14 functions in 10 files, no data,
nothing in `src/triforce`. The game keeps almost all of its small numbers in
`s8` / `u8` and uses plain `char` for text.

| Function | What depends on the sign | Different on the PC? |
|---|---|---|
| `NamCam_GetImageHandle` | `end[-2] == -1 && end[-1] == -0x27`, the FF D9 end of the camera's JPEG | **Yes.** Never true with unsigned `char`: the picture was never accepted. The only line `--lines` reports. |
| `fn_801F9014`, `TexMgr_SetTexture` (`LookupNarrow`) | a text byte compared with a 16-bit character code | No. Reached only for bytes below 0x80. |
| `fn_80144C1C` (`RankPanel_80144430.cpp`) | a one-byte character widened to 16 bits | No. Same: only bytes below 0x80 take that path. |
| `fn_8015D4F8` (`resultscene.cpp`) | `*text + 0x1d60` for letters | No. Inside `'A'..'Z'` / `'a'..'z'` tests. |
| `Sci2Card_BuildRetryReceiptLine`, `Sci2Card_BuildScoreReceiptLine` | `digit - '0'` as an index | No. Characters out of `sprintf("%3d")`. |
| `CardTaskManager_RenderOverlay`, `CardTaskWelcome_DrawStatus`, `nsWakeup::clSteerChecker::Draw` | a `char` passed to a `%c` format | No. Spinner characters and the three letters of an error code. |
| `ISESlot_SetByte18c` | a `char` argument handed on as `int` | No. Callers pass 1 or 2. |
| `ServiceMenu_p34` constructor (`CardCreate_Page.cpp`) | byte 0x20 of the settings block copied to an `int` | No, unless that setting is 128 or more; test-menu values are small. |
| `view1::fn_8017D660`, `view8::fn_8017F538` (`mapobject/fn_8017CE1C.cpp`) | first letter of a file name compared with `(char)(n + 0x40)` | No. Letters. Debug viewers. |

"No" means the two readings give the same result for every value the code
can meet there, by reading the source; none of it was run with both.

The camera patch (`port/patches/src/game/NamCam_GetImageHandle.c.patch`,
with the camera work) casts the two bytes to `signed char`. With the flag
right it is no longer needed; it does no harm.

## Checked

- Linux, no graphics (`link_census.py ... --link`): builds and links. The
  same 150-second scripted run (coins, steering, buttons) with the old and
  the new flag gives the same log.
- Not built or run on Windows.

## Explicitly unsigned narrow types

`u8`, `u16` and unsigned bit-fields compared with a negative constant, and
`switch` on a narrow value with a case that does not fit, were checked too.
MWCC (GC/1.3.2, GC/2.6) compiles them as the C rules say (zero-extend,
compare as `int`: never true), the same as the host compiler, so they
cannot differ. With the right flags the host compiler reports one such
place in the port's directories, `case 0x8140:` on a `char` in
`TextRenderer_DrawEntry.c` (the debug font; a full-width space that can
never match). Both compilers leave it unreachable.
