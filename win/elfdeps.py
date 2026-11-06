#!/usr/bin/env python3
"""
elfdeps.py <id> [--set <dir>] [--all]: the Windows loader's view of a game, offline.

Follows DT_NEEDED from the game binary the way win/elf.c does (the native
shim libraries stop the walk; others are found by DT_SONAME in LIBS, the
game's DATA lib dirs, then the set's /lib, /usr/lib and /usr/X11R6/lib), then prints every
undefined symbol no loaded ELF module defines: the surface the native shims
must provide.  Symbols win/ already provides are left out unless --all
(it greps the N(...) entries of win/*.c; gl* are thunks, the preloaded DLLs'
EXPORTs count too).  Run in WSL; the rom
set comes from build/igs-romset (rompath and cache) unless --set names it.
"""
import os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE_LIBS = {
    "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
    "libSDL-1.2.so.0", "libGL.so.1", "libXxf86vm.so.1", "libpci.so.3",
    "libncurses.so.5", "libX11.so.6", "libXext.so.6", "libXmu.so.6", "libXi.so.6",
    "libtinfo.so.5", "libXt.so.6", "ld-linux.so.2", "libXaw.so.7", "libXpm.so.4",
    "libXfont.so.1", "libSM.so.6", "libICE.so.6", "libXrandr.so.2", "libXcursor.so.1", "libasound.so.2",
}


def preloaded():
    """The preloaded DLLs (igs_soname in their sources): their SONAMEs, and what they export."""
    root = os.path.dirname(HERE)
    names, syms = set(), set()
    for top in ("machines", "games"):
        for d, _, fs in os.walk(os.path.join(root, top)):
            for f in fs:
                if f.endswith(".c"):
                    txt = open(os.path.join(d, f)).read()
                    n = re.findall(r'igs_soname\[\]\s*=\s*"([^"]+)"', txt)
                    if n:
                        names |= set(n)
                        syms |= set(re.findall(r'^EXPORT [^(=]*?\b(\w+)\s*\(', txt, re.M))
    return names, syms


def game(gid, setdir=None):
    """The software-list entry (hash/*.xml) and its rom set directory."""
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), "romtool"))
    import softlist
    sw = softlist.get(gid)
    if not setdir:
        helper = os.path.join(os.path.dirname(HERE), "build", "igs-romset")
        setdir = subprocess.run([helper, "-q", "--path", gid], check=True, capture_output=True,
                                text=True).stdout.strip()
    return sw, setdir


class Elf:
    def __init__(self, path):
        self.path = path
        d = self.d = open(path, "rb").read()
        if d[:4] != b"\x7fELF":
            raise ValueError("not ELF")
        (self.type, _, _, self.entry, phoff, shoff, _, _, phes, phn, shes, shn,
         shstr) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        self.ph = [struct.unpack_from("<8I", d, phoff + i * phes) for i in range(phn)]
        self.dyn = []
        for p in self.ph:
            if p[0] == 2:
                for o in range(p[1], p[1] + p[4], 8):
                    t, v = struct.unpack_from("<iI", d, o)
                    if t == 0:
                        break
                    self.dyn.append((t, v))
        self.dmap = {}
        for t, v in self.dyn:
            self.dmap.setdefault(t, v)
        strtab = self.off(self.dmap[5])
        self.strtab = strtab
        self.needed = [self.s(v) for t, v in self.dyn if t == 1]
        self.soname = self.s(self.dmap[14]) if 14 in self.dmap else None
        # symbol count from DT_HASH (or the section headers)
        if 4 in self.dmap:
            nsym = struct.unpack_from("<II", d, self.off(self.dmap[4]))[1]
        else:
            nsym = None
            for i in range(shn):
                sh = struct.unpack_from("<10I", d, shoff + i * shes)
                if sh[1] == 11:
                    nsym = sh[5] // 16
        symoff = self.off(self.dmap[6])
        self.syms = []
        for i in range(nsym):
            n, v, sz, info, oth, shndx = struct.unpack_from("<IIIBBH", d, symoff + 16 * i)
            self.syms.append((self.s(n), v, shndx, info >> 4, info & 15))
        # versions
        self.ver = {}
        if 0x6ffffffe in self.dmap:          # verneed
            o = self.off(self.dmap[0x6ffffffe])
            while True:
                vn_ver, cnt, file, aux, nxt = struct.unpack_from("<HHIII", d, o)
                a = o + aux
                for _ in range(cnt):
                    h, fl, other, name, anext = struct.unpack_from("<IHHII", d, a)
                    self.ver[other] = self.s(name)
                    a += anext
                if not nxt:
                    break
                o += nxt
        if 0x6ffffffc in self.dmap:          # verdef
            o = self.off(self.dmap[0x6ffffffc])
            while True:
                vd_ver, fl, ndx, cnt, h, aux, nxt = struct.unpack_from("<HHHHIII", d, o)
                name = struct.unpack_from("<I", d, o + aux)[0]
                self.ver[ndx] = self.s(name)
                if not nxt:
                    break
                o += nxt
        self.versym = []
        if 0x6ffffff0 in self.dmap:
            o = self.off(self.dmap[0x6ffffff0])
            self.versym = [struct.unpack_from("<H", d, o + 2 * i)[0] for i in range(nsym)]

    def off(self, vaddr):
        for p in self.ph:
            if p[0] == 1 and p[2] <= vaddr < p[2] + p[4]:
                return vaddr - p[2] + p[1]
        raise ValueError(hex(vaddr))

    def s(self, o):
        st = self.strtab + o
        return self.d[st:self.d.index(b"\0", st)].decode()

    def defined(self):
        return {n for n, v, sh, b, t in self.syms if sh != 0 and n and b in (1, 2)}

    def undefined(self):
        out = []
        for i, (n, v, sh, b, t) in enumerate(self.syms):
            if sh == 0 and n:
                vi = self.versym[i] & 0x7fff if self.versym else 0
                out.append((n, self.ver.get(vi, ""), b == 2))
        return out


def main():
    argv = sys.argv[1:]
    setdir = None
    if "--set" in argv:
        i = argv.index("--set")
        setdir = argv[i + 1]
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith("--")]
    show_all = "--all" in argv
    sw, rootfs = game(args[0], setdir)
    data = rootfs + sw.feature("data", "/")
    lib = lambda l: rootfs + l if l.startswith("/") else data + "/" + l
    dirs = [lib(l) for l in sw.feature("libs", "").split()]
    dirs += [data, data + "/lib", data + "/libs", rootfs + "/lib", rootfs + "/usr/lib", rootfs + "/usr/X11R6/lib"]
    files = []
    for d in dirs:
        if os.path.isfile(d):
            files.append(d)
        elif os.path.isdir(d):
            files += [os.path.join(d, f) for f in sorted(os.listdir(d))]
    cache = {}

    def find(name):
        for f in files:
            if os.path.islink(f) or not os.path.isfile(f):
                continue
            b = os.path.basename(f)
            if b != name and not b.startswith(name):
                if not b.startswith(name.split(".so")[0] + ".so") and not b.startswith(name.split(".so")[0] + "-"):
                    continue
            try:
                e = cache.get(f) or Elf(f)
            except Exception:
                continue
            cache[f] = e
            if e.soname == name or b == name or (e.soname is None and b.startswith(name)):
                return e
        return None

    exe = Elf(lib(sw.feature("binary")))
    mods, queue, seen, natives = [exe], [exe], set(), []
    pre_names, pre_syms = preloaded() if sw.feature("preload", "") else (set(), set())
    native = NATIVE_LIBS | pre_names
    while queue:
        e = queue.pop(0)
        for n in e.needed:
            if n in seen:
                continue
            seen.add(n)
            if n in native:
                natives.append(n)
                continue
            f = find(n)
            if not f:
                print("MISSING LIBRARY", n, "needed by", os.path.basename(e.path))
                continue
            print("%-28s %s" % (n, f.path))
            mods.append(f)
            queue.append(f)
    print("native:", " ".join(natives))
    provided = set()
    if not show_all:
        provided |= pre_syms
        for f in os.listdir(HERE):
            if f.endswith(".c"):
                txt = open(os.path.join(HERE, f), errors="replace").read()
                provided |= set(re.findall(r"\bN(?:[DFAZ]|OK)?\(\s*([A-Za-z_][A-Za-z0-9_]*)", txt))
    defs = set()
    for e in mods:
        defs |= e.defined()
    need = {}
    for e in mods:
        for n, v, weak in e.undefined():
            if n in defs or n in provided or (not show_all and re.match(r"gl[A-Z]", n)):
                continue                     # gl*: thunks to opengl32 (gl.c)
            need.setdefault(n, set()).add("%s%s%s" % (os.path.basename(e.path), "@" + v if v else "", " (weak)" if weak else ""))
    print("native surface: %d symbols" % len(need))
    for n in sorted(need):
        print("  %-36s %s" % (n, ", ".join(sorted(need[n]))))


main()
