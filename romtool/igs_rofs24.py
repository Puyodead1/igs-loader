"""Extract the plaintext IGS ROFS used by E2000 (Linux 2.4.31) root partitions.

The format is cramfs with IGS changes (see wiki Disk Formats,
"E2000 plaintext ROFS"):

* superblock: magic 0xD4AA2681 at +0x4C, root inode at +0xA8;
* 16-byte inodes: version word (XOR checks), namelen:6|offset:26 (both in
  4-byte units), size:24, mode:16|uid:8|gid:8, then the name;
* regular files: a table of ceil(size/4096) block end offsets, then one zlib
  stream per 4 KiB block (cramfs);
* symlinks: same as files, the target is the data.

Usage: igs_rofs24.py <partition.img> <output dir>
       (or import and call extract(image, out, offset, size) on a whole disk)
"""
import os
import struct
import sys
import zlib

MAGIC = 0xD4AA2681
ROOT = 0xA8


def extract(src, out, offset=0, size=None):
    with open(src, 'rb') as f:
        f.seek(offset)
        d = f.read(size) if size else f.read()
    if struct.unpack_from('<I', d, 0x4C)[0] != MAGIC:
        sys.exit('not an E2000 plaintext ROFS (magic at +0x4C)')

    def inode(o):
        ver, a, b, c = struct.unpack_from('<IIII', d, o)
        namelen = (a & 63) * 4
        return dict(off=(a >> 6) * 4, size=b & 0xFFFFFF, mode=c & 0xFFFF,
                    name=d[o + 16:o + 16 + namelen].rstrip(b'\0').decode('latin-1'),
                    next=o + 16 + namelen)

    def data(n):
        nb = (n['size'] + 4095) // 4096
        ptrs = struct.unpack_from('<%dI' % nb, d, n['off'])
        start, parts = n['off'] + 4 * nb, []
        for end in ptrs:
            parts.append(zlib.decompress(d[start:end]) if end > start else b'\0' * 4096)
            start = end
        return b''.join(parts)[:n['size']]

    counts = dict(dirs=0, files=0, links=0, other=0)

    def walk(o, path):
        n = inode(o)
        kind = n['mode'] & 0xF000
        if kind == 0x4000:
            os.makedirs(path, exist_ok=True)
            counts['dirs'] += 1
            p, end = n['off'], n['off'] + n['size']
            while p < end:
                c = inode(p)
                walk(p, os.path.join(path, c['name']))
                p = c['next']
        elif kind == 0x8000:
            with open(path, 'wb') as f:
                f.write(data(n))
            try:
                os.chmod(path, n['mode'] & 0o777)
            except OSError:
                pass
            counts['files'] += 1
        elif kind == 0xA000:
            target = data(n).decode('latin-1')
            if os.path.lexists(path):
                os.remove(path)
            try:
                os.symlink(target, path)
            except OSError:
                open(path + '.symlink', 'w').write(target)
            counts['links'] += 1
        else:
            counts['other'] += 1   # device nodes, fifos: nothing to extract

    walk(ROOT, out)
    print('%(dirs)d dirs, %(files)d files, %(links)d symlinks, %(other)d other' % counts)


if __name__ == '__main__':
    extract(sys.argv[1], sys.argv[2])
