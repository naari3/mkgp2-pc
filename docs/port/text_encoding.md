# Japanese text in the game's string literals

Looked at on 2026-11-12. Found while drawing port screens with the game's
fonts (docs/port/game_ui.md): the game's own Japanese strings were UTF-8 in
the PC build.

## What was wrong

The sources are UTF-8. The matching build does not hand them to MWCC as they
are: `sjiswrap` converts each file to Shift-JIS first, so the bytes of a
literal such as `"ルイージ"` in the arcade program are Shift-JIS
(`83 8B 83 43 81 5B 83 57`). The game reads its text that way: `TexMgr`
takes two bytes at a time and looks the glyph up by Shift-JIS code.

The port compiled the same files with clang, which leaves a literal's bytes
as they are in the file: UTF-8, three bytes a character
(`E3 83 AB E3 82 A4 ...`). Read two bytes at a time as Shift-JIS, that is a
row of unrelated kanji; the game's fonts have glyphs for some of those codes
and none for others, so the text came out as a few wrong characters with
gaps.

Most of the game's Japanese text was never affected: the decompilation keeps
it as data with octal escapes (`"\203\211\203\223..."`, `src/game/autodata`
and many units), and those bytes are the same under any compiler. Only
literals written as readable Japanese were wrong.

## The fix

clang has no `-fexec-charset` other than UTF-8 (the option is accepted and
any other value is an error), and the game's units are compiled by clang for
Linux and for Windows alike (`port/tools/link_census.py`), so no compiler
option does this. The build converts the text itself:

- `port/tools/sjis_literals.py` `convert()` walks a source file as C / C++
  text (comments, string literals, character literals) and replaces every
  character outside ASCII inside a string or character literal with the
  octal escapes of its Shift-JIS bytes (Python's `cp932`, the Windows-31J
  table `sjiswrap` uses). Escapes rather than raw bytes, because the second
  byte of a Shift-JIS character may be `0x5C`, a backslash, which is what
  MWCC's `-multibyte` option deals with in the matching build. Octal with
  three digits, because a hexadecimal escape would swallow a following
  `A`..`F` or digit. Comments stay as they are and no line moves.
- `link_census.py` `compile_one()` compiles a unit that has such literals
  from the converted copy (`build/<kind>/<unit>.sjis.cpp`), made after the
  port's source patches are applied. The copy starts with a `#line` so that
  diagnostics and debug information keep naming the file under `src/`.
- Only the units are converted. If a header, an `.inc` file or a file under
  `port/compat` ever gets a literal with Japanese text, `link_census.py`
  stops with a list of the places (`included_literals()`); today there is
  none.
- A character with no Shift-JIS code stops the build with its place.
- Wide literals (`L"..."`) are left alone; the game has none with text
  outside ASCII.
- `MKGP2PC_UTF8_LITERALS=1` in the environment of `link_census.py` builds as
  before, for comparison.

No source file changed, in either repository, and nothing changes for the
matching build.

Code of the port itself is not compiled through `compile_one()` and keeps
UTF-8 literals: `port/platform/ui/gameui.c` takes UTF-8 and converts each
character itself, the launcher is UTF-8 throughout, and
`port/platform/debug/debug.c` already treated everything the game prints as
Shift-JIS when it writes the log. The source patches (`port/patches`) add no
Japanese literals.

## Which texts changed

`python3 port/tools/sjis_literals.py` lists them: 171 literals in three
files.

| File | Literals | What they are |
|---|---|---|
| `src/game/anon_80202BCC_nm1.cpp` | 149 | The message tables: the lines of the card unit's message windows (insert the card, card issued, renewal, saving, "コインを%d枚入れてください。"), the mode descriptions of the first menu (１人プレイ / 対戦プレイ), the waiting lines of the versus entry, the item pack screens, the debug course condition names (昼間, 雨天, ...), cup and course names (マリオステージ, ＤＫジャングル, ...), character names (ルイージ, キノピオ, ドンキーコング, ヨッシー, パックマン, Ms.パックマン, アカベイ) and the full-width Latin names (ＭＡＲＩＯ ...). Eight "English" lines are in the list only for a full-width space |
| `src/game/EName3D.cpp` | 19 | The name shown above each other kart in a race, Japanese and full-width Latin. マリオ, ピーチ, ワリオ, クッパ and four of the Latin names come from data objects and were always right; the other characters' names were wrong |
| `src/dolphin/dvd/dvdFatal.c` | 3 | The SDK's disc error text. Not one of the port's directories |

Checked against the original program:
`python3 port/tools/sjis_literals.py --check-dol <main.dol or the disc image>`
looks for each converted string, with its terminator, in the file. All 168
strings of the two game files are in the disc image byte for byte. The three
of `dvdFatal.c` are pieces of one concatenated literal and are reported as
not found for that reason.

## How it was checked

Not on a screen. The 32-bit Linux build was run with and without the
conversion (`MKGP2PC_UTF8_LITERALS=1`) through the card questions after a
coin, a card purchase and into a race; every text that came up there is a
sprite or one of the escaped data strings and is the same in both builds. No
screen that draws one of the 171 literals was reached by script.

What was checked instead:

- the executables: the one built before holds "ルイージ", "優勝をめざします！"
  and the others as UTF-8, the one built now as Shift-JIS, and no UTF-8 copy
  is left;
- the disc, as above: all 168 strings match the original program;
- the game's own decoder, linked into a small test program with the glyph
  table (`TexMgr_SetTexture.cpp` and `autodata/data_804E549C.c` as built for
  the port; `_` = no glyph):

  | Text | bytes as before (UTF-8) | bytes now (Shift-JIS) |
  |---|---|---|
  | ルイージ | `__う__ず` | `ルイージ` |
  | ゲームデータを | `_____ョ__ち__` | `ゲームデータを` |

Not built or run on Windows. The three changed units pass clang's syntax
check for the Windows target.

## A second fault: char strings given to the card name function

Found the same day from a dump of the Windows build (72a0266f): on the
"インターネットランキング用" screen after a time attack the line
「今回のタイムのパスワードを記録しますか？」 came out as
`タイ__パス_ゼＩ___します_H`. This string is one of the escaped data strings
and its bytes were right. The literals had nothing to do with it.

The game has two functions that set a text: `TexMgr_SetTexture` takes a
Shift-JIS char string, `fn_801F9014` takes a string of 16-bit codes (card
names: `wcscpy`, then the same decoding). On the console a 16-bit code in
memory is the two Shift-JIS bytes in order, so the game hands char strings
to `fn_801F9014` freely:

- `CoinContinue` (anon_8021BC44_nm1.cpp): the password prompt, message 0x75;
- `TextNode` in fn_801BAA88.cpp: messages 0x87 to 0x89;
- `RankingText` in ranking_display.cpp: the course name;
- the ranking rows (anon_801D286C.cpp): one call site gets a character's
  name (char string) or the player's card name (16-bit codes), and the
  title line.

On the PC the 16-bit codes are native words, low byte first, and
`fn_801F9014` reads words (upstream left it that way on purpose, for the
card names). A char string read like that pairs every second byte with the
first byte of the next character. The test program above, with
`fn_801F9014.cpp` as it is upstream, prints exactly the line of the dump.

Because one call site passes both kinds, the callers cannot be sorted out
one by one. `port/patches/src/game/fn_801F9014.cpp.patch` makes the function
decide for each text: it counts the characters the glyph table (654
two-byte, 85 one-byte codes) does not have under each reading and takes the
reading with fewer; with equal counts, a text that has a one-byte character
under the native reading is a char string, since card names have none.
Checked with the rule written out in Python against the game's data: all
1,126 Japanese strings of the sources that the font can draw are read as
char strings, and 200,000 random names of one to six characters from the
table as 16-bit codes. With the patch the test program prints
「今回のタイムのパスワードを記録しますか？」 and "DKカップ" for the char
strings and the card names as before.

A text whose two readings both exist in the font and that has no one-byte
character (a char string made only of characters such as ワ, whose second
byte is itself a first byte) would be taken for a card name. None of the
1,126 strings is one.

Seen on a screen: no. The password prompt needs a time attack record, which
was not played here.

## Not looked at

- Text that reaches the game from outside the program (card names, names
  from other cabinets over the link) is 16-bit codes in memory and was not
  touched by this.
- Character literals with a Japanese character (`'あ'`) would be converted
  to the same escapes, which gives MWCC's two-byte value only by accident of
  clang's multi-character constants; there is none in the game today, the
  sources spell such codes as numbers.
