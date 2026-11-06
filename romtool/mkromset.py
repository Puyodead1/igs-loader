#!/usr/bin/env python3
"""Build a loader rom set from an original IGS disk image (CHD or raw .img).

    mkromset.py <id> <image.chd|image.img> [-o <dir>] [--format 7z|zip|dir]
                [--mx <0-9>] [--no-verify]

<id> is a software name from hash/<machine>.xml (mkromset.py --list).  The
set is the cabinet's file tree as the running game sees it: the root fs at
/, the data partitions at their mount points, the executable where the
cabinet has it at run time.  The entry's "build" feature says how, as
';'-separated steps that mirror the cabinet's boot:

    root p2               p2 is the root fs (E2100 igs_rofs with the entry's
                          rofs_profile; E2000 plaintext ROFS)
    decrypt SRC DST SIZE  FPGA-cipher decrypt SRC to DST with exe_mask (or
                          asset_mask when the entry has none), keep SIZE
                          bytes (Scratch's /bin/unrar step)
    copy SRC DST          copy a file inside the tree
    assemble DST          E2000: rebuild the split executable at DST
                          (geca_sector, head_sector, pieces, trim_kb, trim_step,
                          head_part, geca_part, geca_file, piece_file, piece_mode,
                          cut_kb, cut_step)
    mount pN DIR          partition N replaces DIR's contents: p3 SquashFS
                          (E2100, unsquashfs) or IGS_SHFS (E2000); ext2/3
                          (debugfs; lost+found is left out), e.g. E2000 p4 or
                          every E1000 partition (p5.. are logical partitions)
    unupx SRC DST         unpack the UPX-packed executable SRC to DST
                          (igs_upx.py; We Dance's wedo), SRC kept as dumped
    loop SRC DIR          the SquashFS image file SRC replaces DIR's contents,
                          as a loop mount; the image is left out of the set.
                          A '*' in SRC matches files, a '*' in DIR is the
                          match's name without its extension (E2200 ROMS/*.rom)
    poweron SRC DST       E2200 launcher decrypt: skip SRC's 4-byte key-id
                          header, FPGA-cipher decrypt with exe_mask, keep the
                          size from the 12-byte trailer; SRC kept as dumped

A dump without its partition table names the layout in the entry's
`partitions` feature (`start+count` sectors for p1, p2...) and the disk
bytes it lacks at the start in `dump_skip` (Fish Hunter 4 M101CN).

Absolute symlinks are made relative to the set root, so the tree works
wherever it is extracted.  Then the files the entry lists in <dataarea
name="files"> are checked, and the tree is packed: 7z (default; 7z a -mx9,
symlinks stored as links), zip (deflate, symlinks as Info-ZIP links) or a
plain folder.

Inputs: a .img is checked against the entry's disk SHA-1 (--no-verify skips
the hash, it reads the whole image); a .chd's header data SHA-1 is checked,
then chdman extracthd writes a temporary .img next to the output.  chdman:
$CHDMAN, then chdman= in igs-loader.ini ($IGS_INI names another file), then
PATH.

Output: -o, else the first rompath entry of igs-loader.ini, else
<loader>/roms.  Temporary files live in <output>/.<id>.stage and go away
as soon as they are packed.

Runs in WSL/Linux (needs 7z, unsquashfs, debugfs; python3-cryptography makes
the E2100 root fs faster).

This is the reference implementation.  igs-mkromset (mkromset/, C, Linux and
Windows, no external tools) builds the same sets and is the one to use.
"""
import argparse
import hashlib
import os
import shutil
import stat
import struct
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import softlist            # noqa: E402
import igs_rofs            # noqa: E402
import igs_rofs24          # noqa: E402
import igs_shfs            # noqa: E402
import igs_fpga_decrypt    # noqa: E402
import igs_e2000_extract   # noqa: E402
import igs_upx             # noqa: E402

LOADER = os.path.dirname(HERE)


def log(*a):
    print('[mkromset]', *a, flush=True)


def host_path(p):
    """A path from igs-loader.ini (T:\\x or /mnt/t/x) in this host's form."""
    p = p.strip()
    if os.name != 'nt' and len(p) > 2 and p[1] == ':' and p[0].isalpha():
        return '/mnt/%s%s' % (p[0].lower(), p[2:].replace('\\', '/'))
    if os.name == 'nt' and p.startswith('/mnt/') and len(p) > 6 and p[6] in '/':
        return '%s:%s' % (p[5].upper(), p[6:].replace('/', '\\'))
    return p


def ini_path():
    """igs-loader.ini, or $IGS_INI (relative: to the loader)."""
    p = os.environ.get('IGS_INI')
    return os.path.join(LOADER, host_path(p)) if p else os.path.join(LOADER, 'igs-loader.ini')


def ini_value(key):
    """A global key=value setting from igs-loader.ini (before the first [section]), or None."""
    ini = ini_path()
    if os.path.exists(ini):
        for line in open(ini, encoding='utf-8', errors='replace'):
            line = line.strip()
            if line.startswith('['):
                break
            if line.startswith(('#', ';')):
                continue
            k, sep, v = line.partition('=')
            if sep and k.strip().lower() == key:
                return v.strip()
    return None


def default_output():
    for p in (ini_value('rompath') or '').split(';'):
        if p.strip():
            return host_path(p)
    return os.path.join(LOADER, 'roms')


def sha1_file(path, bufsize=1 << 22):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        while True:
            b = f.read(bufsize)
            if not b:
                return h.hexdigest()
            h.update(b)


def partitions(image):
    """{n: (start, count)} in sectors: p1..p4, then the logical p5.. (E1000)."""
    with open(image, 'rb') as f:
        mbr = f.read(512)
        if mbr[510:512] != b'\x55\xaa':
            sys.exit('%s: no MBR partition table' % image)
        parts = {i + 1: struct.unpack_from('<II', mbr, 446 + 16 * i + 8) for i in range(4)}
        for i in range(4):
            if mbr[446 + 16 * i + 4] not in (0x05, 0x0f, 0x85):
                continue
            # extended: a chain of EBRs, each a logical partition and a link to the next
            ext, ebr, n = parts[i + 1][0], parts[i + 1][0], 5
            while n < 64:
                f.seek(ebr * 512)
                b = f.read(512)
                if b[510:512] != b'\x55\xaa':
                    break
                start, count = struct.unpack_from('<II', b, 446 + 8)
                if count:
                    parts[n] = (ebr + start, count)
                    n += 1
                link, = struct.unpack_from('<I', b, 446 + 16 + 8)
                if not link:
                    break
                ebr = ext + link
    return parts


# ---- inputs -------------------------------------------------------------------------------

def find_chdman():
    ini = ini_value('chdman')
    for c in (os.environ.get('CHDMAN'), ini and host_path(ini), shutil.which('chdman'),
              shutil.which('chdman.exe')):
        if c and os.path.exists(c):
            return c
    sys.exit('chdman not found: set CHDMAN, chdman= in igs-loader.ini, or put it on PATH')


def chd_data_sha1(path):
    with open(path, 'rb') as f:
        h = f.read(124)
    if h[:8] != b'MComprHD':
        sys.exit('%s: not a CHD' % path)
    version, = struct.unpack_from('>I', h, 12)
    # the raw data SHA-1: v5 at 64, v4 at 88; v3 has only the data SHA-1, at 80
    return {5: h[64:84], 4: h[88:108], 3: h[80:100]}.get(version, b'').hex() or None


def chd_to_img(chd, img):
    chdman = find_chdman()
    args = [chdman, 'extracthd', '-f', '-i', chd, '-o', img]
    if chdman.lower().endswith('.exe') and os.name != 'nt':
        # a Windows chdman from WSL: give it Windows paths
        args = [chdman, 'extracthd', '-f', '-i', wslpath(chd), '-o', wslpath(img)]
    log('chdman extracthd ->', img)
    subprocess.run(args, check=True)


def wslpath(p):
    return subprocess.run(['wslpath', '-w', os.path.abspath(p)], check=True, capture_output=True,
                          text=True).stdout.strip()


# ---- build steps --------------------------------------------------------------------------

class Builder:
    def __init__(self, sw, image, stage):
        self.sw, self.image, self.stage = sw, image, stage
        # a dump without its partition table: the entry gives the layout
        # ("start+count" sectors for p1, p2...) and the bytes the dump lacks
        self.skip = int(sw.feature('dump_skip', '0'), 0)
        if sw.feature('partitions'):
            self.parts = {i + 1: tuple(int(x, 0) for x in p.split('+'))
                          for i, p in enumerate(sw.feature('partitions').split())}
        else:
            self.parts = partitions(image)

    def path(self, cab):
        return os.path.join(self.stage, cab.lstrip('/'))

    def part(self, name):
        if len(name) != 2 or name[0] != 'p' or not name[1].isdigit() or int(name[1]) not in self.parts:
            sys.exit('bad partition %r in build step' % name)
        start, count = self.parts[int(name[1])]
        if not count:
            sys.exit('partition %s is empty' % name)
        if start * 512 < self.skip:
            sys.exit('partition %s starts in the bytes the dump lacks' % name)
        return start * 512 - self.skip, count * 512

    def run(self, steps):
        for step in [s.split() for s in steps.split(';') if s.strip()]:
            op, args = step[0], step[1:]
            t = time.time()
            getattr(self, 'step_' + op, self.unknown)(op, *args)
            log('%-8s %s  (%.0f s)' % (op, ' '.join(args), time.time() - t))

    def unknown(self, op, *a):
        sys.exit('unknown build step %r' % op)

    def step_root(self, op, p):
        off, size = self.part(p)
        os.makedirs(self.stage, exist_ok=True)
        if self.sw.machine == 'e2000':
            igs_rofs24.extract(self.image, self.stage, off, size)
        else:
            with open(self.image, 'rb') as f:
                f.seek(off)
                data = f.read(size)
            igs_rofs.extract(data, self.stage, self.sw.feature('rofs_profile'))

    def step_decrypt(self, op, src, dst, size):
        mask = self.sw.feature('exe_mask') or self.sw.feature('asset_mask')
        igs_fpga_decrypt.MASK = bytes.fromhex(mask)
        with open(self.path(src), 'rb') as f:
            out = igs_fpga_decrypt.decrypt(f.read())[:int(size)]
        self.write(dst, out)

    def step_copy(self, op, src, dst):
        with open(self.path(src), 'rb') as f:
            self.write(dst, f.read())

    def step_link(self, op, path, target):
        p = self.path(path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if os.path.lexists(p):
            os.remove(p)
        os.symlink(target, p)

    def step_unupx(self, op, src, dst):
        with open(self.path(src), 'rb') as f:
            self.write(dst, igs_upx.unpack(f.read()))

    def step_poweron(self, op, src, dst):
        igs_fpga_decrypt.MASK = bytes.fromhex(self.sw.feature('exe_mask'))
        with open(self.path(src), 'rb') as f:
            out = igs_fpga_decrypt.decrypt(f.read()[4:])
        # trailer: random word, plain size, key id
        _, size, _ = struct.unpack_from('<3I', out, len(out) - 12)
        if size > len(out) - 12 or out[:4] != b'\x7fELF':
            sys.exit('%s: bad decrypt (wrong exe_mask?)' % src)
        self.write(dst, out[:size])

    def step_loop(self, op, src, cab):
        if '*' in os.path.basename(src):
            d = self.path(os.path.dirname(src))
            pat = os.path.basename(src).split('*')
            for n in sorted(os.listdir(d)):
                if n.startswith(pat[0]) and n.endswith(pat[1]) and len(n) >= len(pat[0]) + len(pat[1]):
                    self.step_loop(op, os.path.dirname(src) + '/' + n, cab.replace('*', os.path.splitext(n)[0]))
            return
        img, d = self.path(src), self.path(cab)
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        subprocess.run(['unsquashfs', '-q', '-n', '-d', d, img], check=True, stdout=subprocess.DEVNULL)
        os.remove(img)

    def step_assemble(self, op, dst):
        m = self.sw.manifest()
        parts = [self.parts[i] for i in (1, 2, 3, 4)]
        self.write(dst, igs_e2000_extract.build_executable(self.image, parts, self.stage, m))

    def write(self, cab, data):
        p = self.path(cab)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if os.path.lexists(p):
            os.remove(p)
        with open(p, 'wb') as f:
            f.write(data)
        os.chmod(p, 0o755)

    def step_mount(self, op, p, cab):
        off, size = self.part(p)
        d = self.path(cab)
        # the mount hides whatever the root fs has there
        if os.path.lexists(d) and not os.path.isdir(d):
            os.remove(d)
        if os.path.isdir(d):
            for n in os.listdir(d):
                q = os.path.join(d, n)
                shutil.rmtree(q) if os.path.isdir(q) and not os.path.islink(q) else os.remove(q)
        os.makedirs(d, exist_ok=True)
        with open(self.image, 'rb') as f:
            f.seek(off)
            magic = f.read(4)
            f.seek(off + 1080)
            ext_magic = f.read(2)
        if magic == b'hsqs':
            subprocess.run(['unsquashfs', '-q', '-n', '-f', '-o', str(off), '-d', d, self.image],
                           check=True, stdout=subprocess.DEVNULL)
        elif struct.unpack('<I', magic)[0] == igs_shfs.MAGIC:
            with open(self.image, 'rb') as f:
                f.seek(off)
                sb = igs_shfs.squashfs4_superblock(f.read(0xAC))
                if sb:
                    # SquashFS 4 behind the IGS header: the image with a standard superblock
                    f.seek(off)
                    data = bytearray(f.read(struct.unpack_from('<Q', sb, 40)[0]))
            if sb:
                data[:96] = sb
                img = os.path.join(os.path.dirname(os.path.abspath(self.stage)), '.shfs4.img')
                with open(img, 'wb') as f:
                    f.write(data)
                subprocess.run(['unsquashfs', '-q', '-n', '-f', '-d', d, img], check=True,
                               stdout=subprocess.DEVNULL)
                os.remove(img)
            else:
                igs_shfs.extract(self.image, d, off)
        elif ext_magic == b'\x53\xef':
            spec = '%s?offset=%d' % (self.image, off)
            names = subprocess.run(['debugfs', '-R', 'ls -p /', spec], check=True, capture_output=True,
                                   text=True).stdout
            for line in names.splitlines():
                # /inode/mode/uid/gid/name/size/
                f = line.strip().strip('/').split('/')
                if len(f) < 5 or f[4] in ('.', '..', 'lost+found', ''):
                    continue
                subprocess.run(['debugfs', '-R', 'rdump "/%s" "%s"' % (f[4], d), spec], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            sys.exit('%s: unknown filesystem (magic %s)' % (p, magic.hex()))

    def finish(self):
        """Absolute symlinks -> relative to the set root; drop lost+found."""
        fixed = 0
        for d, dirs, files in os.walk(self.stage):
            if 'lost+found' in dirs:
                shutil.rmtree(os.path.join(d, 'lost+found'), ignore_errors=True)
                dirs.remove('lost+found')
            for n in dirs + files:
                p = os.path.join(d, n)
                if os.path.islink(p):
                    t = os.readlink(p)
                    if t.startswith('/'):
                        rel = os.path.relpath(os.path.join(self.stage, t.lstrip('/')), d)
                        os.remove(p)
                        os.symlink(rel, p)
                        fixed += 1
        log('%d absolute symlinks made relative' % fixed)


def resolve(stage, cab, hops=0):
    """A cabinet path inside the set, following the set's own symlinks."""
    parts, cur = [x for x in cab.split('/') if x], stage
    for i, x in enumerate(parts):
        nxt = os.path.join(cur, x)
        if os.path.islink(nxt) and hops < 40:
            t = os.readlink(nxt)
            base = '/' if t.startswith('/') else '/' + os.path.relpath(cur, stage)
            target = os.path.normpath(os.path.join(base, t)).replace('\\', '/')
            return resolve(stage, target + '/' + '/'.join(parts[i + 1:]), hops + 1)
        cur = nxt
    return cur


def verify(sw, stage):
    ok = True
    data = sw.feature('data', '/')
    binary = sw.feature('binary')
    checks = list(sw.roms)
    if binary:
        checks.append((binary if binary.startswith('/') else data.rstrip('/') + '/' + binary, -1, None))
    for name, size, sha1 in checks:
        p = resolve(stage, name)
        if not os.path.isfile(p):
            log('MISSING %s' % name)
            ok = False
            continue
        got = sha1_file(p)
        n = os.path.getsize(p)
        good = (sha1 is None or sha1.startswith('@') or sha1 == got) and (size < 0 or size == n)
        log('%-6s %s  %d  %s' % ('ok' if good else 'BAD', name, n, got))
        ok &= good
    return ok


# ---- packing ------------------------------------------------------------------------------

def pack_7z(stage, out, mx):
    tmp = out + '.part'
    if os.path.exists(tmp):
        os.remove(tmp)
    subprocess.run(['7z', 'a', '-t7z', '-mx=%d' % mx, '-snl', '-bso0', '-bsp0', tmp, '*'], cwd=stage, check=True)
    os.replace(tmp, out)


def pack_zip(stage, out, mx):
    tmp = out + '.part'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED, allowZip64=True, compresslevel=mx or None) as z:
        for d, dirs, files in os.walk(stage):
            dirs.sort()
            for n in sorted(dirs) + sorted(files):
                p = os.path.join(d, n)
                rel = os.path.relpath(p, stage).replace(os.sep, '/')
                st = os.lstat(p)
                if stat.S_ISLNK(st.st_mode):
                    zi = zipfile.ZipInfo(rel)
                    zi.create_system = 3
                    zi.external_attr = (stat.S_IFLNK | 0o777) << 16
                    z.writestr(zi, os.readlink(p), zipfile.ZIP_STORED)
                elif stat.S_ISDIR(st.st_mode):
                    zi = zipfile.ZipInfo(rel + '/')
                    zi.create_system = 3
                    zi.external_attr = ((stat.S_IFDIR | 0o755) << 16) | 0x10
                    z.writestr(zi, b'')
                elif stat.S_ISREG(st.st_mode):
                    z.write(p, rel)
    os.replace(tmp, out)


def main():
    ap = argparse.ArgumentParser(description='Build an IGS loader rom set from a CHD or raw disk image.')
    ap.add_argument('id', nargs='?')
    ap.add_argument('image', nargs='?')
    ap.add_argument('-o', '--output', help='output directory (default: rompath from igs-loader.ini, or roms/)')
    ap.add_argument('--format', choices=('7z', 'zip', 'dir'), default='7z')
    ap.add_argument('--mx', type=int, default=9, help='compression level (default 9)')
    ap.add_argument('--no-verify', action='store_true', help='skip the image SHA-1 check')
    ap.add_argument('--list', action='store_true', help='list the known ids and their disk images')
    a = ap.parse_args()
    if a.list or not a.id:
        for name, sw in sorted(softlist.load().items()):
            print('%-24s %-6s %-30s %s%s' % (name, sw.machine, sw.description, sw.disk_name,
                                             '  (clone of %s)' % sw.cloneof if sw.cloneof else ''))
        return
    if not a.image:
        ap.error('need an image')
    sw = softlist.get(a.id)
    if not sw.feature('build'):
        sys.exit('%s has no build recipe' % a.id)
    outdir = a.output or default_output()
    os.makedirs(outdir, exist_ok=True)
    stage = os.path.join(outdir, '.%s.stage' % a.id)
    if os.path.exists(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    tmp_img = None
    t0 = time.time()
    try:
        image = a.image
        if image.lower().endswith('.chd'):
            got = chd_data_sha1(image)
            if got and sw.disk_sha1 and got != sw.disk_sha1:
                log('WARNING: %s data SHA-1 %s, the list has %s (%s)' % (image, got, sw.disk_sha1, sw.disk_name))
            tmp_img = os.path.join(outdir, '.%s.img' % a.id)
            chd_to_img(image, tmp_img)
            image = tmp_img
        elif not a.no_verify and sw.disk_sha1:
            log('hashing', image)
            got = sha1_file(image)
            if got != sw.disk_sha1:
                log('WARNING: %s SHA-1 %s, the list has %s (%s)' % (image, got, sw.disk_sha1, sw.disk_name))
            else:
                log('image matches', sw.disk_name)
        b = Builder(sw, image, stage)
        b.run(sw.feature('build'))
        b.finish()
        if tmp_img:
            os.remove(tmp_img)
            tmp_img = None
        if not verify(sw, stage):
            log('WARNING: the set does not match the list')
        if a.format == 'dir':
            out = os.path.join(outdir, a.id)
            if os.path.exists(out):
                shutil.rmtree(out)
            os.replace(stage, out)
            log('set ->', out)
            return
        out = os.path.join(outdir, '%s.%s' % (a.id, a.format))
        log('packing', out)
        (pack_7z if a.format == '7z' else pack_zip)(stage, out, a.mx)
        log('set -> %s, %.1f MB, %.0f s' % (out, os.path.getsize(out) / 1e6, time.time() - t0))
    finally:
        if tmp_img and os.path.exists(tmp_img):
            os.remove(tmp_img)
        if os.path.exists(stage):
            shutil.rmtree(stage)


if __name__ == '__main__':
    main()
