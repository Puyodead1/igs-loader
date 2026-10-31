#!/usr/bin/env python3
"""
elfdeps.py <manifest> [--all]: the Windows loader's view of a game, offline.

Follows DT_NEEDED from the game binary the way win/elf.c does (the native
shim libraries stop the walk; others are found by DT_SONAME in LIBS, the
game's DATA lib dirs, then ROOTFS/lib and ROOTFS/usr/lib), then prints every
undefined symbol no loaded ELF module defines: the surface the native shims
must provide.  Symbols win/ already provides are left out unless --all
(it greps the NATIVE(...) / GLFN entries of win/*.c).  Run in WSL.
"""
import os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE_LIBS = {
    "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
    "libSDL-1.2.so.0", "libGL.so.1", "libXxf86vm.so.1", "libpci.so.3",
    "libncurses.so.5", "libX11.so.6", "libXext.so.6", "libXmu.so.6", "libXi.so.6",
    "libtinfo.so.5", "libXt.so.6", "ld-linux.so.2",
}


def manifest(path):
    env = {}
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            v = v[1:-1]
        env[k.strip()] = v
    return env


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
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    m = manifest(args[0])
    data = m["DATA"]
    work = m.get("WORK", os.path.dirname(data))
    rootfs = m.get("ROOTFS", work + "/output")
    dirs = [data + "/" + l for l in m.get("LIBS", "").split()]
    dirs += [data, data + "/lib", data + "/libs", rootfs + "/lib", rootfs + "/usr/lib"]
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

    exe = Elf(data + "/" + m["BINARY"] if os.path.exists(data + "/" + m["BINARY"]) else m["BINARY"])
    mods, queue, seen, natives = [exe], [exe], set(), []
    while queue:
        e = queue.pop(0)
        for n in e.needed:
            if n in seen:
                continue
            seen.add(n)
            if n in NATIVE_LIBS:
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
        for f in os.listdir(HERE):
            if f.endswith(".c"):
                txt = open(os.path.join(HERE, f), errors="replace").read()
                provided |= set(re.findall(r"\bN[DFA]?\(\s*([A-Za-z_][A-Za-z0-9_]*)", txt))
    defs = set()
    for e in mods:
        defs |= e.defined()
    need = {}
    for e in mods:
        for n, v, weak in e.undefined():
            if n in defs or n in provided:
                continue
            need.setdefault(n, set()).add("%s%s%s" % (os.path.basename(e.path), "@" + v if v else "", " (weak)" if weak else ""))
    print("native surface: %d symbols" % len(need))
    for n in sorted(need):
        print("  %-36s %s" % (n, ", ".join(sorted(need[n]))))


main()
