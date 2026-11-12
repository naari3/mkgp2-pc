"""gdb helpers for looking at the game's HSD (sysdolphin) scene in the
32-bit Linux executable. No function of the game is called (a `call` from
gdb left the process stuck); memory is read and written directly.

  gdb -p <pid> -x port/tools/gdb_hsd.py
  (gdb) hsd-walk 0x80ac04a0        joint tree from the top ancestor of a joint:
                                   address, flags, next, child, render modes
  (gdb) hsd-hide 0x80abf800        blank the display lists of a joint's
                                   polygons (the game rewrites DOBJ_HIDDEN
                                   every frame, so that flag is of no use)
  (gdb) hsd-rmode 0x80abf800 0x04000000
                                   OR bits into the render mode of every
                                   material of a joint (0x04000000 =
                                   RENDER_SHADOW)
  (gdb) hsd-step                   one game frame while paused

Offsets (docs/port/census.md, "Kart shadow absent inside floor grooves"): JObj next 0x08, parent 0x0C,
child 0x10, flags 0x14, dobj 0x18; DObj next 0x04, mobj 0x08, pobj 0x0C;
MObj rendermode 0x04; PObj next 0x04, n_display 0x0E.
"""
import struct
import gdb


def _inf():
    return gdb.selected_inferior()


def u32(a):
    return struct.unpack('<I', bytes(_inf().read_memory(a, 4)))[0]


def w32(a, v):
    _inf().write_memory(a, struct.pack('<I', v & 0xFFFFFFFF))


def dobjs(j):
    if u32(j + 0x14) & 0x4020:      # spline / particle joints carry no DObj
        return
    d = u32(j + 0x18)
    n = 0
    while d and n < 256:
        yield d
        d = u32(d + 4)
        n += 1


def walk(j, depth, top):
    while j:
        flags = u32(j + 0x14)
        modes = []
        for d in dobjs(j):
            m = u32(d + 8)
            modes.append('%08x' % (u32(m + 4) if m else 0))
        print('%s%x f=%08x next=%x child=%x | %s' % (
            '  ' * depth, j, flags, u32(j + 8), u32(j + 0x10), ' '.join(modes)))
        c = u32(j + 0x10)
        if c and not (flags & 0x1000):
            walk(c, depth + 1, False)
        if top:
            break
        j = u32(j + 8)


class Walk(gdb.Command):
    def __init__(self):
        super().__init__('hsd-walk', gdb.COMMAND_DATA)

    def invoke(self, arg, tty):
        j = int(gdb.parse_and_eval(arg))
        while u32(j + 0xC):
            j = u32(j + 0xC)
        walk(j, 0, True)


class Hide(gdb.Command):
    def __init__(self):
        super().__init__('hsd-hide', gdb.COMMAND_DATA)

    def invoke(self, arg, tty):
        j = int(gdb.parse_and_eval(arg))
        for d in dobjs(j):
            p = u32(d + 0xC)
            while p:
                _inf().write_memory(p + 0xE, b'\0\0')
                p = u32(p + 4)


class RMode(gdb.Command):
    def __init__(self):
        super().__init__('hsd-rmode', gdb.COMMAND_DATA)

    def invoke(self, arg, tty):
        a = gdb.string_to_argv(arg)
        j = int(gdb.parse_and_eval(a[0]))
        bits = int(gdb.parse_and_eval(a[1]))
        for d in dobjs(j):
            m = u32(d + 8)
            if m:
                w32(m + 4, u32(m + 4) | bits)


class Step(gdb.Command):
    def __init__(self):
        super().__init__('hsd-step', gdb.COMMAND_RUNNING)

    def invoke(self, arg, tty):
        gdb.execute('set var s_paused=0')
        gdb.execute('tbreak VIWaitForRetrace')
        gdb.execute('continue')
        gdb.execute('set var s_paused=1')


Walk()
Hide()
RMode()
Step()
