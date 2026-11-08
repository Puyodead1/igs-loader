#!/usr/bin/env python3
"""E3000 "Game Image v2" root filesystems that are not igs_rofs streams (wiki Disk Formats).

  speeddriver3  plaintext: the directory stream from 0xC4, 20-byte entry headers
                [check, w1, w2, w3, mode u16, name length u16], names in
                4 * (length + 1) bytes; w1 ^ off_key the offset in words (plus 1
                for a directory; a file's low two bits vary), w3 ^ size_key the size
  speedrider2   keyed inodes: a fixed header then the name padded to 16, header
                words xored with the kernel's constants; the root inode at 0x40;
                SR2 FA's first 0x60 header bytes are AES-ECB

Files are cramfs-style in both: a table of 4 KiB block end offsets, then zlib per
block (a zero-length block is a hole).  The C port is mkromset/fs_gimg.c.

usage: igs_gimg.py <p2.img> <profile> [--extract <dir>]
"""
import os
import struct
import sys
import zlib

import igs_rofs

PROFILES = ('speeddriver3', 'speedrider2')

SD3_OFF_KEY, SD3_SIZE_KEY, MAGIC = 0x016F88CC, 0x0777613F, 0xD4AA2684

# Speed Rider 2's kernel constants (init_igs_rofs_fs)
K_SIZE, K_MAJ, K_MIN, K_ROFS, K_ROOT, K_D = 0x505D3EC3, 0x1F416F72, 0xBAE4CAD7, 0x10B1219F, 0x04E20F34, 0x2A1F
SR2_MAJOR, SR2_MINOR, SR2_ROFS = 0xA5A5A5A5, 0xAA55EB48, 0x14532EAB
SR2_KEY = 'riderspeed2-v1.0'
# name, header size, encrypted bytes, major, name length, offset, size, minor, mode, root list
LAYOUTS = (
    ('Speed Rider 2', 0x58, 0, 0x04, 0x1C, 0x28, 0x30, 0x4C, 0x54, 0x130),
    ('Speed Rider 2 FA', 0x68, 0x60, 0x04, 0x2C, 0x38, 0x40, 0x5C, 0x64, 0x140),
)


def _u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def _sanitize(name):
    return ''.join('_' if c in '<>:"/\\|?*' or ord(c) < 32 else c for c in name)


def _blocks(d, off, size):
    """The content whose block table is at byte offset off, or None when it does not decode."""
    nb = (size + 0xFFF) >> 12
    if off + 4 * nb > len(d):
        return None
    start = off + 4 * nb
    out = []
    for b in range(nb):
        end = _u32(d, off + 4 * b)
        want = min(0x1000, size - b * 0x1000)
        if end < start or end > len(d) or end - start > 0x2000:
            return None
        if end == start:
            out.append(bytes(want))
        else:
            z = zlib.decompressobj()
            try:
                page = z.decompress(d[start:end], want)
            except zlib.error:
                return None
            if len(page) != want:
                return None
            out.append(page)
        start = end
    return b''.join(out)


class _Sink:
    def __init__(self, out):
        self.out = out
        self.counts = dict(dirs=0, files=0, links=0, other=0)

    def node(self, rel, mode, data):
        kind = mode & 0xF000
        if kind == 0x4000:
            self.counts['dirs'] += 1
            if self.out:
                os.makedirs(os.path.join(self.out, rel), exist_ok=True)
        elif kind == 0x8000:
            self.counts['files'] += 1
            if self.out:
                path = os.path.join(self.out, rel)
                with open(path, 'wb') as f:
                    f.write(data)
                os.chmod(path, mode & 0o7777)
        elif kind == 0xA000:
            self.counts['links'] += 1
            if self.out:
                path = os.path.join(self.out, rel)
                if os.path.lexists(path):
                    os.remove(path)
                os.symlink(data.decode('latin-1'), path)
        else:
            self.counts['other'] += 1


# ---- Speed Driver 3: plaintext --------------------------------------------------------------

def _sd3(d, sink):
    if len(d) < 0x100 or _u32(d, 0x7C) != MAGIC:
        raise ValueError('not a plaintext Game Image v2 root (no magic at 0x7C)')
    check = _u32(d, 0xC4)

    def walk(path, o, end, depth):
        if depth > 200:
            raise ValueError('directories nested too deep')
        while o < end:
            if _u32(d, o) != check:
                raise ValueError('no entry at 0x%X' % o)
            w1, w3 = _u32(d, o + 4), _u32(d, o + 12)
            mode, nl = struct.unpack_from('<HH', d, o + 16)
            ln = 4 * (nl + 1)
            name = _sanitize(d[o + 20:o + 20 + ln].split(b'\0')[0].decode('latin-1'))
            rel = path + '/' + name if path else name
            size = w3 ^ SD3_SIZE_KEY
            if mode & 0xF000 == 0x4000:
                sink.node(rel, mode, None)
                at = ((w1 ^ SD3_OFF_KEY) + 1) * 4
                if size:
                    walk(rel, at, at + size, depth + 1)
            elif mode & 0xF000 in (0x8000, 0xA000):
                data = b''
                if size:
                    base = w1 ^ SD3_OFF_KEY
                    data = next((c for c in (_blocks(d, (base ^ x) * 4, size) for x in range(4)) if c is not None), None)
                    if data is None:
                        raise ValueError('no decodable blocks for ' + rel)
                sink.node(rel, mode, data)
            else:
                sink.node(rel, mode, None)
            o += 20 + ln

    walk('', 0xC4, 0xA0 + (_u32(d, 0x50) ^ SD3_SIZE_KEY), 0)
    return 'speeddriver3'


# ---- Speed Rider 2: keyed inodes -------------------------------------------------------------

def _sr2(d, sink):
    aes = igs_rofs.Aes(igs_rofs.effective_key(SR2_KEY))

    def inode(L, o, root):
        _, hdr, enc, maj, nlen, off, size, minor, mode, _ = L
        h = d[o:o + hdr]
        if len(h) != hdr:
            return None
        if enc:
            h = aes.ecb(h[:enc]) + h[enc:]
        if root:
            if _u32(h, maj) ^ K_ROFS != SR2_ROFS:
                return None
            okey = K_ROOT + K_D
        else:
            if _u32(h, maj) ^ K_MAJ != SR2_MAJOR:
                return None
            okey = K_MAJ - K_D
        if _u32(h, minor) ^ K_MIN != SR2_MINOR:
            return None
        e = dict(size=(_u32(h, size) ^ (K_SIZE - K_D)) & 0xFFFFFFF,
                 off=((_u32(h, off) ^ okey) & 0x3FFFFFF) * 4,
                 mode=struct.unpack_from('<H', h, mode)[0], name='', next=o + hdr)
        if not root:
            nl = h[nlen] ^ ((K_MIN - K_D) & 0xFF)
            if not nl:
                return None
            e['name'] = _sanitize(d[o + hdr:o + hdr + nl].split(b'\0')[0].decode('latin-1'))
            e['next'] = o + hdr + ((nl + 15) & ~15)
        return e

    def walk(L, path, o, end, depth):
        if depth > 64:
            raise ValueError('directories nested too deep')
        while o < end:
            e = inode(L, o, False)
            if e is None:
                raise ValueError('no inode at 0x%X' % o)
            rel = path + '/' + e['name'] if path else e['name']
            kind = e['mode'] & 0xF000
            if kind == 0x4000:
                sink.node(rel, e['mode'], None)
                if e['size']:
                    walk(L, rel, e['off'], e['off'] + e['size'], depth + 1)
            elif kind in (0x8000, 0xA000):
                data = _blocks(d, e['off'], e['size']) if e['size'] else b''
                if data is None:
                    raise ValueError(rel + ' does not decode')
                sink.node(rel, e['mode'], data)
            else:
                sink.node(rel, e['mode'], None)
            o = e['next']
        if o != end:
            raise ValueError('a directory\'s entries end at 0x%X, its size says 0x%X' % (o, end))

    for L in LAYOUTS:
        r = inode(L, 0x40, True)
        if r and r['mode'] & 0xF000 == 0x4000 and r['off'] == L[9]:
            walk(L, '', r['off'], r['off'] + r['size'], 0)
            return L[0]
    raise ValueError('not a Speed Rider 2 Game Image v2 root (no root inode at 0x40)')


def extract(data, out, profile, quiet=False):
    """Extract the partition bytes 'data' into directory 'out' (None: only check)."""
    if profile not in PROFILES:
        raise ValueError('unknown Game Image profile %r' % profile)
    sink = _Sink(out)
    if out:
        os.makedirs(out, exist_ok=True)
    layout = _sd3(data, sink) if profile == 'speeddriver3' else _sr2(data, sink)
    if not quiet:
        c = sink.counts
        print('Game Image %s (%s): %d dirs, %d files, %d symlinks, %d special nodes skipped'
              % (profile, layout, c['dirs'], c['files'], c['links'], c['other']))
    return layout


def main(argv):
    if len(argv) < 2 or argv[0] in ('-h', '--help'):
        sys.exit(__doc__)
    out = argv[argv.index('--extract') + 1] if '--extract' in argv else None
    with open(argv[0], 'rb') as f:
        extract(f.read(), out, argv[1])


if __name__ == '__main__':
    main(sys.argv[1:])
