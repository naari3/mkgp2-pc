# Dumps: memory, stacks and crash folders

Written 2026-11-12. Code: `port/gfx/diag.c` (stacks, memory, crash
handling), `port/gfx/jpeg_min.c` (the small picture), `port/gfx/video_aurora.c`
(the F9 key and the picture), `port/tools/dump_inspect.py` (reading a dump on
another machine). The keys, the picture and the input recording are described
in [build.md](build.md), "Pause, frame step and dumps".

## What a dump folder holds

`F9` (or the script command `dump`) writes `dumps/<n>_f<frame>/` under the
directory the program was started from; `MKGP2PC_DUMP_DIR` names another
place. When the program dies it writes `dumps/crash_<date>_<time>/` by
itself. Both hold:

| File | What |
| --- | --- |
| `stacks.txt` | every thread: its registers, then the return addresses on its stack, each with the function it lies in |
| `memory.txt` | which address ranges are in the two memory files, which sections are code, which modules were loaded where |
| `mem1.bin.gz` | the console's main memory, 48 MB at `0x80000000`: the game's heap and everything it loaded from the disc |
| `image.bin.gz` | the executable's data sections: the game's global variables (`.mw_data`, `.mw_bss`, `.mw_sdata`, ... are the original's sections), the port's and the libraries' |
| `symbols.txt.gz` | address, kind (`T` code, `D` data) and name of every symbol of the executable that wrote the dump |
| `input.txt`, `state.txt` | as before: the inputs since the start (replayable), the frame number and settings |
| `gx_fifo.bin.gz` | the draw commands of the frame: everything the game's GX calls sent, in order (crash folders: the frame so far, and `gx_fifo_prev.bin.gz`, the frame before) |
| `frame.png`, `frame_small.jpg` | F9 only: the picture, and a copy at most 640 wide (some tens of KB) |
| `crash.dmp` | crash folders on Windows only: a minidump (all threads with their stacks, the module list, the exception) for WinDbg or Visual Studio |

Sizes in a race on Linux: `mem1.bin.gz` 22 MB (the loaded textures and
sounds do not compress), `image.bin.gz` 2.5 MB, `symbols.txt.gz` 0.8 MB,
`stacks.txt` 10 KB. Writing them holds the game for about 0.6 s.
`MKGP2PC_DUMP_MEMORY=0` leaves the three large files out of F9 dumps; crash
folders always have them. `frame.png` is now deflated (it was stored
uncompressed, about 6 MB at 1280x960).

Dumps hold game data and, when a card is in, the card's contents: they are
not committed (`dumps/` is in `.gitignore`).

## When the program dies

Handled, each with the lines `crash: ...` in the log (`gfx.log`) first and
the folder after:

- an exception (Windows: access violation, illegal or privileged
  instruction, stack overflow, integer division by zero, in-page error,
  array bounds, breakpoint; and whatever else nobody handles, a C++
  exception for one) or a signal (Linux: SIGSEGV, SIGBUS, SIGILL, SIGFPE,
  SIGABRT);
- `abort()` called by the program's own code;
- a fatal message of the rendering library (Aurora's log callback is
  called before Aurora aborts): the reason line is the message, for example
  `fatal message of aurora::gx: indexed XF load from unmapped array 21`,
  and the stack shows which function of the game issued the call. Before
  this such an end left no trace in the log.

Not caught: an `abort()` inside a DLL with its own C runtime unless it ends
in a breakpoint or an access violation, a fast-fail (`__fastfail`, heap
corruption detected by Windows), and a kill from outside.

The system's own handling follows as before (Windows' error report, the
core dump on Linux). The handler runs in the dying thread and uses the C
library; if the crash was inside the C library's own locks the folder may
stay incomplete, which is why the log lines come first.

To try it: script commands `crash` (writes to address 8) and `abort`, for
example `MKGP2PC_SCRIPT="600 crash"`.

## Reading `stacks.txt`

```
SIGSEGV in thread 24257, address 00000008

thread 24257: the thread this is about
  at 084402d7 script_extra+0xe7
  eax 00000000 ebx 096b8ff4 ... ebp ffd2c6a8 esp ffd2c6a0
  # +0000c 08081c93 run+0x213
  # +0004c 080817fb mkgp2pc_script_tick+0x15b
  # +0007c 084406e2 VIWaitForRetrace+0x232
  ? +00084 084f0d62 __GXSetDirtyState+0x52
  # +0033c 080a03fe _ZL11FinishFramev+0xde
  # +0037c 0809ff31 BootDispatcher+0xcc1
```

A line is: mark, offset from the stack pointer, the word found there, its
name. `#` lines are return addresses of frames on the frame-pointer chain:
the real call stack wherever the code keeps frame pointers (the game's code
and the port's; `port/gfx` is compiled with `-fno-omit-frame-pointer` for
this). `?` lines are other stack words that point just behind a call
instruction: real callers in code without frame pointers (Dawn, the C
library, drivers), or left-overs of earlier calls. At most 64 KB of each
stack and 96 lines per thread.

Names come from the executable's own symbol table (ELF `.symtab`; on
Windows the COFF symbol table the MinGW link leaves in the file), read from
the executable's file when first needed, so the text is already symbolised
on the machine that wrote it. Addresses in other modules are
`module+0xoffset`.

Other threads. Windows: each is suspended while its registers and stack
are copied; names given with `SetThreadDescription` are shown. Linux: each
is sent a signal (`SIGRTMIN+6`) and copies the same itself; threads that
block signals (Mesa's workers do) are listed as not read.

## Reading the memory on another machine

`port/tools/dump_inspect.py <folder>` needs only the folder (Python 3, no
packages): the symbols travel with the dump, so a dump made by a Windows
build reads the same on Linux without that build's executable.

```
dump_inspect.py <folder>                       regions, files, the frame chain of every thread
dump_inspect.py <folder> stacks --all          stacks.txt with the '?' lines
dump_inspect.py <folder> sym 'Kart.*Speed'     symbols by regular expression
dump_inspect.py <folder> addr 80900000 080a03fe
dump_inspect.py <folder> words lbl_802EAF10 16     32-bit words; values that are addresses are named
dump_inspect.py <folder> read 80c00000 0x100       hex dump
dump_inspect.py <folder> floats <where> 12
dump_inspect.py <folder> str <where>               Shift-JIS string
dump_inspect.py <folder> find --text course        search all regions (or hex bytes)
dump_inspect.py <folder> save mem1 mem1.bin        one region, unpacked
```

`<where>` is a hexadecimal address, a symbol or `symbol+offset`. Values are
little-endian (the port converts what it loads); `--be` reads data that is
still in the disc's byte order. On Windows the game's symbols have their
plain names and the libraries' a leading underscore.

A pointer into `mem1` is a heap object or loaded file; which one is not
recorded (the heap's block headers are in the memory, a walker is not
written).

## The frame's draw commands

Aurora's GX functions do not draw. They write the console's command stream
(BP, CP and XF register loads and vertex data, as the real GX library
does) into a buffer, with commands of Aurora's own where the console
passed an address (vertex arrays, textures, copies), and a worker thread
works through it. `port/gfx/gx_shim.cpp` copies that buffer as it grows, a
frame at a time (about 0.7 MB for a race frame of the attract mode, 5500
draws; 0.35 MB packed). `MKGP2PC_DUMP_GX=0` switches the copying off.

`port/tools/gx_fifo.py <folder>` lists it:

```
gx_fifo.py <folder> --summary          counts, and problems found
gx_fifo.py <folder>                    every command, one line each
gx_fifo.py <folder> --draw 37 --verts  draw 37 with the commands since draw 36 and its vertices
gx_fifo.py <folder> --prev             the frame before (crash folders)
```

```
XF  posMtx0                  -0.000113 0.453948 -0.891028 34.8862 ...
AUR array POS                811e6140 (mem1+0x11e6140) size 1224 big-endian
CP  vcdLo                    00000400
AUR loadTexObj map0 80f3a2c0 (mem1+0xf3a2c0) 64x64 CMPR tlut 0 object 12 version 1
DRAW #2 tristrip fmt0 34 vertices  [POS:index8 TEX0:index8]
-- GXCopyTex dest 0x9cab020 clear 0
```

Addresses are named when the dump's memory files are there, and the array
or texture can then be read with `dump_inspect.py`. The tool reports an
indexed load or draw whose array address is 0, which is what the library's
"indexed XF load from unmapped array" is about.

What the record does not have: state set in earlier frames (the vertex
description and formats at the frame's start are recorded; matrices,
textures and arrays set earlier and not again are not), and the commands a
call writes just before it empties the buffer itself. For the calls known
to do that (`GXDrawDone`, `GXCopyTex`, `GXCopyDisp`) the copy is taken
around them and a note line names the call; if the buffer was emptied
anywhere else the listing says `emptied unseen`. Register values are shown
as numbers, with the register's name; they are not broken down into
fields.

## What was verified where

- Linux (cloud, Xvfb, software GL): F9 dump, `crash` and `abort` in the
  game; folders complete; `dump_inspect.py` on them. `gx_fifo.py` read whole
  frames of the attract mode's race to the last byte (5552 draws). `frame.png` and
  `frame_small.jpg` are not written there (no frame read-back); the JPEG
  writer was checked separately against a decoder.
- Windows code paths, with `diag.c` built alone as a test program
  (`-DMKGP2PC_DIAG_TEST -DMKGP2PC_DIAG_NO_ZLIB`, `diag_test.exe
  dump|crash|abort|fatal`) under Wine 9: COFF symbols, other threads'
  stacks, the vectored handler, `abort()`, `crash.dmp` written by
  `dbghelp.dll`, and `dump_inspect.py` on the result.
- The Windows executable itself (cross-built here) under Wine 9: it does
  not get as far as a picture there (Wine's `msvcp140.dll` lacks a function
  Dawn calls), and that end was caught: log lines with `webgpu_dawn.dll`,
  `aurora_initialize` and `__wrap_main` on the stack, a complete crash
  folder, readable with `dump_inspect.py` (87123 symbols).
- Not verified: on Windows proper (real `dbghelp.dll`, D3D12 threads, an F9
  dump during play, the rendering library's fatal path, the size of
  `crash.dmp`).

A fatal message of the rendering library comes from the thread that works
through the draw commands ("Aurora FIFO processor"), not from the game's
thread: the first thread in `stacks.txt` is then that worker, and the game's
thread is further down (look for `BootDispatcher`).

## Not done

- `gx_fifo.py` names registers but does not decode their fields, and
  cannot draw the frame.
- No heap walker and no typed view of structures in `dump_inspect.py`.
- No picture in a crash folder.
