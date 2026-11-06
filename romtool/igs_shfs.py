"""Extract IGS_SHFS game-data partitions (E2000, Linux 2.4.31 kernels).

IGS_SHFS is SquashFS 2.x with an IGS superblock (see
wiki Disk Formats, "E2000 IGS_SHFS").  Two layouts:

  "Game Image" (Gold Captain, Panda Family, Go Go Ball 2, Speed Rider):
    +0x00 magic 0xD4AA2682 (0xD4AA2683 = other endian, not supported here)
    +0x04 "<International Games System Co., Ltd.> Game Image " text
    +0x44 0x00100000
    +0x48 game type, +0x4C game version  (kernel compares both)
    +0x50 major, +0x54 minor             (obfuscated words, also compared)
  plain (Pirate Revenge, Speed Driver 2): magic, 0x00100000, then
    major = 2 and minor = 1 at +0x08 / +0x0C (no text, no game checks)

From the major word at M (0x50 or 0x08), all packed little endian:
  M+0x08 inodes, M+0x0C bytes_used, M+0x10 uid_start, M+0x14 guid_start,
  M+0x18 inode_table_start, M+0x1C directory_table_start,
  M+0x20 flags, M+0x21 no_uids, M+0x22 no_guids, M+0x23 mkfs_time,
  M+0x27 root_inode (u64), M+0x2F block_size, M+0x33 fragments,
  M+0x37 fragment_table_start

Everything after the superblock (metadata blocks, inodes, directories, data
blocks, fragments) is stock SquashFS 2.x.

Usage: igs_shfs.py <partition.img> <output dir> [--list]
       (or import and call extract(image, out, offset) on a whole disk)
"""
import os
import struct
import sys
import zlib

MAGIC = 0xD4AA2682
META = 8192


def squashfs4_superblock(h):
    """IGS_SHFS on SquashFS 4 (Crazy Shoot): the "Game Image" text, then
    SquashFS 4's fields reordered (0x44 root inode, 0x4C bytes used, four
    obfuscated words, 0x64 inodes ... 0x7C the tables).  The standard 96-byte
    superblock for the same image, or None when h (0xAC bytes) is not one."""
    if len(h) < 0xAC or h[4:12] != b' <Intern':
        return None
    bs, lg = struct.unpack_from('<I', h, 0x6C)[0], struct.unpack_from('<H', h, 0x76)[0]
    if not 12 <= lg <= 20 or bs != 1 << lg:
        return None
    return (b'hsqs' + h[0x64:0x7C] + struct.pack('<HH', 4, 0) + h[0x44:0x54] + h[0x7C:0xAC])


class Shfs:
    def __init__(self, path, base=0):
        self.f, self.base = open(path, 'rb'), base
        h = self.read(0, 0x8B)
        u = lambda o: struct.unpack_from('<I', h, o)[0]
        if u(0) != MAGIC:
            sys.exit('not an IGS_SHFS partition (magic 0x%08X)' % u(0))
        if h[4:12] == b' <Intern':
            m = 0x50
            self.game_type, self.game_version = u(0x48), u(0x4C)
        else:
            m = 0x08
            self.game_type = self.game_version = None
        self.major, self.minor = u(m), u(m + 4)
        self.uid_start, self.guid_start = u(m + 0x10), u(m + 0x14)
        self.inode_start, self.dir_start = u(m + 0x18), u(m + 0x1C)
        self.no_uids, self.no_guids = h[m + 0x21], h[m + 0x22]
        self.root = struct.unpack_from('<Q', h, m + 0x27)[0]
        self.block_size, self.fragments, self.frag_start = u(m + 0x2F), u(m + 0x33), u(m + 0x37)
        self.cache = {}
        self.uids = struct.unpack('<%dI' % self.no_uids, self.read(self.uid_start, 4 * self.no_uids))
        self.guids = struct.unpack('<%dI' % self.no_guids, self.read(self.guid_start, 4 * self.no_guids))
        # fragment table: index of metadata block locations, entries {start u32, size u32}
        nidx = (self.fragments * 8 + META - 1) // META
        idx = struct.unpack('<%dI' % nidx, self.read(self.frag_start, 4 * nidx))
        raw = b''.join(self.meta(b)[0] for b in idx)
        self.frags = [struct.unpack_from('<II', raw, 8 * i) for i in range(self.fragments)]

    def read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def meta(self, off):
        """One metadata block at an absolute offset -> (data, next offset)."""
        if off not in self.cache:
            l = struct.unpack('<H', self.read(off, 2))[0]
            raw = self.read(off + 2, l & 0x7FFF)
            self.cache[off] = (raw if l & 0x8000 else zlib.decompress(raw), off + 2 + (l & 0x7FFF))
        return self.cache[off]

    def stream(self, table, block, offset, n):
        """n bytes of a metadata table starting at (block, offset)."""
        out, off = b'', table + block
        while len(out) < n + offset:
            d, off = self.meta(off)
            out += d
        return out[offset:offset + n]

    def inode(self, ref):
        block, offset = ref >> 16, ref & 0xFFFF
        b = self.stream(self.inode_start, block, offset, 64)
        hdr = struct.unpack_from('<I', b)[0]
        t, mode = hdr & 15, (hdr >> 4) & 0xFFF
        uid = self.uids[(hdr >> 16) & 0xFF]
        g = hdr >> 24
        gid = uid if g == 0xFF else self.guids[g]
        n = dict(type=t, mode=mode, uid=uid, gid=gid)
        if t == 1:                                     # dir
            w = struct.unpack_from('<I', b, 4)[0]
            n.update(size=w & 0x7FFFF, offset=w >> 19, mtime=struct.unpack_from('<I', b, 8)[0],
                     start=int.from_bytes(b[12:15], 'little'))
        elif t == 8:                                   # long dir
            w = int.from_bytes(b[4:9], 'little')
            s = int.from_bytes(b[13:18], 'little')
            n.update(type=1, size=w & 0x7FFFFFF, offset=w >> 27, mtime=struct.unpack_from('<I', b, 9)[0],
                     start=s & 0xFFFFFF)
        elif t == 2:                                   # regular file
            mtime, start, frag, foff, size = struct.unpack_from('<IIIII', b, 4)
            nb = (size + self.block_size - 1) // self.block_size if frag == 0xFFFFFFFF else size // self.block_size
            blocks = struct.unpack('<%dI' % nb, self.stream(self.inode_start, block, offset + 24, 4 * nb))
            n.update(mtime=mtime, start=start, frag=frag, foff=foff, size=size, blocks=blocks)
        elif t == 3:                                   # symlink
            l = struct.unpack_from('<H', b, 4)[0]
            n['target'] = self.stream(self.inode_start, block, offset + 6, l).decode('latin-1')
        return n

    def listdir(self, n):
        """Directory entries of a dir inode: [(name, inode ref)]."""
        # i_size counts 3 bytes more than the listing (SquashFS 2.x "." / ".." convention)
        data = self.stream(self.dir_start, n['start'], n['offset'], n['size'])
        out, p = [], 0
        while p < n['size'] - 3:
            count, start = data[p] + 1, int.from_bytes(data[p + 1:p + 4], 'little')
            p += 4
            for _ in range(count):
                w, size = struct.unpack_from('<HB', data, p)
                name = data[p + 3:p + 4 + size].decode('latin-1')
                out.append((name, (start << 16) | (w & 0x1FFF)))
                p += 4 + size
        return out

    def file_data(self, n):
        parts, off = [], n['start']
        for b in n['blocks']:
            l = b & 0xFFFFFF
            raw = self.read(off, l)
            parts.append(raw if b & (1 << 24) else zlib.decompress(raw))
            off += l
        if n['frag'] != 0xFFFFFFFF:
            fstart, fsize = self.frags[n['frag']]
            raw = self.read(fstart, fsize & 0xFFFFFF)
            frag = raw if fsize & (1 << 24) else zlib.decompress(raw)
            parts.append(frag[n['foff']:n['foff'] + n['size'] % self.block_size])
        return b''.join(parts)[:n['size']]


def extract(src, out, offset=0, listing=False):
    fs = Shfs(src, offset)
    if fs.game_type is not None:
        print('game type %08X version %08X' % (fs.game_type, fs.game_version), end=', ')
    print('major %08X minor %08X, block %d, %d fragments' % (fs.major, fs.minor, fs.block_size, fs.fragments))
    counts = dict(dirs=0, files=0, links=0, other=0, bytes=0)

    def walk(ref, path):
        n = fs.inode(ref)
        if n['type'] == 1:
            counts['dirs'] += 1
            if not listing:
                os.makedirs(path, exist_ok=True)
            for name, child in fs.listdir(n):
                walk(child, os.path.join(path, name))
        elif n['type'] == 2:
            counts['files'] += 1
            counts['bytes'] += n['size']
            if listing:
                print('%10d %s' % (n['size'], path))
                return
            with open(path, 'wb') as f:
                f.write(fs.file_data(n))
            try:
                os.chmod(path, n['mode'] & 0o777)
            except OSError:
                pass
        elif n['type'] == 3:
            counts['links'] += 1
            if listing:
                print('%10s %s -> %s' % ('link', path, n['target']))
                return
            if os.path.lexists(path):
                os.remove(path)
            try:
                os.symlink(n['target'], path)
            except OSError:
                open(path + '.symlink', 'w').write(n['target'])
        else:
            counts['other'] += 1

    walk(fs.root, out)
    print('%(dirs)d dirs, %(files)d files (%(bytes)d bytes), %(links)d symlinks, %(other)d other' % counts)


if __name__ == '__main__':
    extract(sys.argv[1], sys.argv[2], listing='--list' in sys.argv)
