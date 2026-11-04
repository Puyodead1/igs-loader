"""Extract the IGS ROFS v2 ("igs_rofs") root filesystems of E2100 kernels.

A Python port of IgsRofsExtractor (romtool/IgsRofsExtractor, C#): same
profiles, same tree reconstruction.  Format facts are in
docs/IGS_ROFS_REVERSE_ENGINEERING.md, keys and constants in docs/IGS_KEYS.md.

Every per-kernel profile shares the AES-128 key transform, the record stream
and the per-page zlib layout; they differ in constants, field positions,
name encryption and the page-table encoding.  A profile without a key
(Fish-OS) is plaintext: fields read straight from the image.  Unlike the C# tool this one
keeps the file modes and does not write IGS_ROFS_SPECIAL_NODES.txt (device
nodes and other special records are only counted).

Usage: igs_rofs.py <p2.img> [--list] [--extract <dir>] [--profile <name>]
       (or import and call extract(data, out, profile) on the partition bytes)

AES comes from the 'cryptography' package when it is installed, otherwise
from a small pure-Python decryptor (slower, same result).
"""
import os
import struct
import sys
import zlib

MAGIC = 0xD4AA2684

# name: (key, magic block, magic word, first record, header size, mode offset,
#        name length (block, word, xor), size (...), table (...),
#        encrypted names, encrypted page table, blanked zlib header, page-table word)
PROFILES = {
    # Harpoon Lagoon (Fish4P-OS).  XORs are the init constants - 0x3CE8.
    'fish4p': ('Hunter4PFishV1.0', 0xC8, 1, 0x140, 0x68, 0x64,
               (0x20, 1, 0x3A9A44A4), (0x40, 1, 0x3CED596B), (0x30, 2, 0x9F3EE741),
               False, False, False, 0),
    # Ocean Party (OceanParty-OS).  Names are AES-CBC (zero IV, same key);
    # page-table entries are one encrypted 16-byte block each; each page's
    # zlib header is zeroed.
    'oceanparty': ('PartyOcean6PV1.0', 0x58, 0, 0x150, 0x78, 0x74,
                   (0x50, 3, 0x02C4290D), (0x30, 1, 0x02C529C9), (0x00, 3, 0x03CB8124),
                   True, True, True, 0),
    # Hero of Robots (HeroOfRobots-OS).  Harpoon's layout, other positions.
    'heroofrobots': ('RobotsHeroOfV1.0', 0xF0, 3, 0x140, 0x68, 0x64,
                     (0x40, 2, 0x51223E7A), (0x30, 0, 0xEF250457), (0x00, 2, 0xF487975F),
                     False, False, False, 0),
    # Scratch (Scratch-OS).  Ocean Party's design; magic block unaligned at
    # 0x74, modes at +0x72, page ends are word 1 of each table block.
    'scratch': ('SwinV1.00Scratch', 0x74, 2, 0x150, 0x78, 0x72,
                (0x00, 3, 0x2C858D5A), (0x30, 1, 0x2C858D5A), (0x50, 1, 0xE1ECA651),
                True, True, True, 1),
    # Percussion Master 3 (PM3-OS, E2200).  Ocean Party's design; XORs are
    # the init constants - 0xED2E; page ends are word 1.
    'pmaster3': ('E2200-PMaster-V3', 0xD8, 1, 0x150, 0x78, 0x74,
                 (0x20, 1, 0x498C027F), (0x50, 3, 0xCD83B702), (0x40, 0, 0xEC285CDA),
                 True, True, True, 1),
    # Fish Hunter 4 M400CN (Fish4P-OS build #38).  Harpoon's key and
    # constants, an earlier record layout: header 0x58, mode at +0x54.
    'fish4p_m400': ('Hunter4PFishV1.0', 0xB8, 1, 0x130, 0x58, 0x54,
                    (0x10, 1, 0x3A9A44A4), (0x30, 1, 0x3CED596B), (0x20, 2, 0x9F3EE741),
                    False, False, False, 0),
    # Fish Hunter 4 / 6 (Fish-OS).  No AES: a cramfs-like plaintext stream,
    # 24-byte headers, names padded to 4 (PLAIN below).  XORs are the init
    # constants - 0x2E9B.
    'fish': (None, 0x78, 0, 0xC0, 0x18, 0x10,
             (0x00, 0, 0), (0x0C, 0, 0xBE5BE76E), (0x14, 0, 0xED8D65C8),
             False, False, False, 0),
    # Fish Hunter 6 M607CN (Fish6Pv2-OS).  Ocean Party's design; XORs are
    # the init constants +/- 0xC7; page ends are word 3.
    'fish6pv2': ('Fish6PHunterV2.0', 0xD0, 1, 0x150, 0x78, 0x72,
                 (0x00, 0, 0x683CD4AC), (0x30, 0, 0x67CC9C41), (0x50, 3, 0xC2693974),
                 True, True, True, 3),
}

# plaintext profiles: (name length mask, unit, name alignment); the name
# length is (field & mask) * unit and already a multiple of the alignment
PLAIN = {
    'fish': (0x3F, 4, 4),
}


def effective_key(literal):
    """The driver transforms the key literal before the AES-128 key expansion."""
    w = struct.unpack('>4I', literal.encode('ascii'))
    return struct.pack('>4I', w[0] >> 2, w[1] >> 1, w[2], (w[3] << 1) & 0xFFFFFFFF)


# ---- AES-128 decryption -------------------------------------------------------------------

class _PyAes:
    """Minimal AES-128 ECB decryption (only used without 'cryptography')."""

    def __init__(self, key):
        sbox = [0] * 256
        p = q = 1
        while True:
            p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
            q ^= q << 1
            q ^= q << 2
            q ^= q << 4
            q &= 0xFF
            if q & 0x80:
                q ^= 0x09
            x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
            sbox[p] = (x ^ 0x63) & 0xFF
            if p == 1:
                break
        sbox[0] = 0x63
        self.inv = [0] * 256
        for i, v in enumerate(sbox):
            self.inv[v] = i

        def xt(a):
            return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1

        def mul(a, b):
            r = 0
            while b:
                if b & 1:
                    r ^= a
                a = xt(a)
                b >>= 1
            return r
        self.m9 = [mul(a, 9) for a in range(256)]
        self.m11 = [mul(a, 11) for a in range(256)]
        self.m13 = [mul(a, 13) for a in range(256)]
        self.m14 = [mul(a, 14) for a in range(256)]
        rk = list(key)
        rcon = 1
        while len(rk) < 176:
            t = rk[-4:]
            if len(rk) % 16 == 0:
                t = [sbox[t[1]] ^ rcon, sbox[t[2]], sbox[t[3]], sbox[t[0]]]
                rcon = xt(rcon)
            rk += [rk[-16 + i] ^ t[i] for i in range(4)]
        self.rk = [rk[16 * r:16 * r + 16] for r in range(11)]

    def block(self, b):
        s = [b[i] ^ self.rk[10][i] for i in range(16)]
        inv, m9, m11, m13, m14 = self.inv, self.m9, self.m11, self.m13, self.m14
        for r in range(9, -1, -1):
            # inverse shift rows + inverse sub bytes
            s = [inv[s[(i + 4 * ((i % 4) * 3)) % 16]] for i in range(16)]
            s = [s[i] ^ self.rk[r][i] for i in range(16)]
            if r:
                t = []
                for c in range(4):
                    a0, a1, a2, a3 = s[4 * c:4 * c + 4]
                    t += [m14[a0] ^ m11[a1] ^ m13[a2] ^ m9[a3], m9[a0] ^ m14[a1] ^ m11[a2] ^ m13[a3],
                          m13[a0] ^ m9[a1] ^ m14[a2] ^ m11[a3], m11[a0] ^ m13[a1] ^ m9[a2] ^ m14[a3]]
                s = t
        return bytes(s)

    def ecb(self, data):
        return b''.join(self.block(data[i:i + 16]) for i in range(0, len(data), 16))


class Aes:
    def __init__(self, key):
        try:
            from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
            self._ecb = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
            self._py = None
        except ImportError:
            self._py = _PyAes(key)

    def ecb(self, data):
        return self._py.ecb(data) if self._py else self._ecb.update(data)

    def cbc0(self, data):
        """CBC with a zero IV."""
        plain = bytearray(self.ecb(data))
        for i in range(16, len(data)):
            plain[i] ^= data[i - 16]
        return bytes(plain)


# ---- the filesystem -----------------------------------------------------------------------

def _align16(v):
    return (v + 15) & ~15


_INVALID = set('<>:"/\\|?*') | {chr(c) for c in range(32)}


def _sanitize(name):
    return ''.join('_' if c in _INVALID else c for c in name)


class Rofs:
    def __init__(self, data, profile=None):
        names = [profile] if profile else list(PROFILES)
        for n in names:
            if n not in PROFILES:
                raise ValueError('unknown ROFS profile %r (known: %s)' % (n, ', '.join(PROFILES)))
            self._setup(data, n)
            if self.magic == MAGIC:
                self._scan()
                return
        raise ValueError('not an IGS ROFS v2 image for any known profile '
                         '(no key decodes the superblock magic)')

    def _setup(self, data, name):
        (key, mblock, mword, self.first, self.hsize, self.modeoff, self.f_namelen, self.f_size,
         self.f_table, self.enc_names, self.enc_table, self.blanked, self.pt_word) = PROFILES[name]
        self.data, self.profile = data, name
        self.aes = Aes(effective_key(key)) if key else None
        self.plain = PLAIN.get(name)
        self.magic = self._word(mblock, mword)

    def _word(self, off, word):
        if not self.aes:
            return struct.unpack_from('<I', self.data, off + 4 * word)[0]
        return struct.unpack_from('<I', self.aes.ecb(self.data[off:off + 16]), word * 4)[0]

    def _align(self, n):
        return (n + 3) & ~3 if self.plain else _align16(n)

    def _field(self, f, off):
        return self._word(off + f[0], f[1]) ^ f[2]

    def _record(self, off):
        if self.plain:
            namelen = (self._field(self.f_namelen, off) & self.plain[0]) * self.plain[1]
        else:
            namelen = self._field(self.f_namelen, off) & 0xFF
        start = off + self.hsize
        if not 0 < namelen <= 255 or start + self._align(namelen) > len(self.data):
            raise ValueError('bad name length')
        raw = self.data[start:start + self._align(namelen)]
        if self.enc_names:
            raw = self.aes.cbc0(raw)
        name = ''.join(chr(b) if b < 0x80 else '?' for b in raw[:namelen]).rstrip('\0')
        size = self._field(self.f_size, off) & 0x0FFFFFFF
        table = (self._field(self.f_table, off) & 0x03FFFFFF) * 4
        mode, = struct.unpack_from('<H', self.data, off + self.modeoff)
        return dict(off=off, name=name, namelen=namelen, mode=mode, size=size, table=table)

    def _scan(self):
        self.records, off = [], self.first
        while off + self.hsize <= len(self.data):
            try:
                r = self._record(off)
            except (ValueError, struct.error):
                break
            if not r['name'] or any(ord(c) < 32 or 0x7F <= ord(c) <= 0x9F for c in r['name']):
                break
            self.records.append(r)
            off += self.hsize + self._align(r['namelen'])
        self.meta_end = off
        self.by_off = {r['off']: r for r in self.records}

    def content(self, r):
        if r['size'] == 0:
            return b''
        pages = (r['size'] + 0xFFF) >> 12
        if self.enc_table:
            tab = self.aes.ecb(self.data[r['table']:r['table'] + 16 * pages])
            ends = [struct.unpack_from('<I', tab, 16 * i + 4 * self.pt_word)[0] for i in range(pages)]
            start = r['table'] + 16 * pages
        else:
            ends = struct.unpack_from('<%dI' % pages, self.data, r['table'])
            start = r['table'] + 4 * pages
        out = []
        for p, end in enumerate(ends):
            if end <= start or end - start > 0x2000 or end > len(self.data):
                raise ValueError('bad compressed extent for %s, page %d: 0x%X-0x%X' % (r['name'], p, start, end))
            comp = self.data[start:end]
            if self.blanked:
                comp = b'\x78\x9c' + comp[2:]
            want = min(0x1000, r['size'] - p * 0x1000)
            page = zlib.decompressobj().decompress(comp, want)
            if len(page) < want:
                raise ValueError('short zlib block for %s, page %d' % (r['name'], p))
            out.append(page[:want])
            start = end
        return b''.join(out)

    def walk(self):
        """Yield (relative path, record) for the whole tree, parents first."""
        dirs = [r for r in self.records if r['mode'] & 0xF000 == 0x4000 and r['size']]
        root_end = min(r['table'] for r in dirs)
        used = set()

        def rng(path, start, length):
            if start < self.first or start + length > self.meta_end or start & 3:
                raise ValueError('bad directory range 0x%X+0x%X' % (start, length))
            off = start
            while off < start + length:
                r = self.by_off.get(off)
                if r is None:
                    raise ValueError('no metadata record at 0x%X' % off)
                if off in used:
                    raise ValueError('directory graph reuses record 0x%X (%s)' % (off, r['name']))
                used.add(off)
                p = path + '/' + _sanitize(r['name']) if path else _sanitize(r['name'])
                yield p, r
                if r['mode'] & 0xF000 == 0x4000 and r['size']:
                    yield from rng(p, r['table'], r['size'])
                off += self.hsize + self._align(r['namelen'])

        yield from rng('', self.first, root_end - self.first)
        if len(used) != len(self.records):
            raise ValueError('tree reconstruction missed %d metadata records' % (len(self.records) - len(used)))


def extract(data, out, profile=None, quiet=False):
    """Extract the partition bytes 'data' into directory 'out'."""
    fs = Rofs(data, profile)
    os.makedirs(out, exist_ok=True)
    counts = dict(dirs=0, files=0, links=0, other=0)
    for rel, r in fs.walk():
        path = os.path.join(out, rel)
        kind = r['mode'] & 0xF000
        if kind == 0x4000:
            os.makedirs(path, exist_ok=True)
            counts['dirs'] += 1
        elif kind == 0x8000:
            with open(path, 'wb') as f:
                f.write(fs.content(r))
            os.chmod(path, r['mode'] & 0o7777)
            counts['files'] += 1
        elif kind == 0xA000:
            if os.path.lexists(path):
                os.remove(path)
            os.symlink(fs.content(r).decode('latin-1'), path)
            counts['links'] += 1
        else:
            counts['other'] += 1
    if not quiet:
        print('igs_rofs profile %s: %d dirs, %d files, %d symlinks, %d special nodes skipped'
              % (fs.profile, counts['dirs'], counts['files'], counts['links'], counts['other']))
    return fs.profile


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        sys.exit('usage: igs_rofs.py <p2.img> [--list] [--extract <dir>] [--profile <%s>]' % '|'.join(PROFILES))
    image = argv[0]
    opt = lambda k: argv[argv.index(k) + 1] if k in argv and argv.index(k) + 1 < len(argv) else None
    out, profile = opt('--extract'), opt('--profile')
    with open(image, 'rb') as f:
        data = f.read()
    if out and '--list' not in argv:
        extract(data, out, profile)
        return
    fs = Rofs(data, profile)
    print('Profile: %s; records: %d; metadata ends: 0x%X' % (fs.profile, len(fs.records), fs.meta_end))
    for rel, r in fs.walk():
        print('0x%06X %04X %9d table=0x%08X %s' % (r['off'], r['mode'], r['size'], r['table'], rel))
    if out:
        extract(data, out, profile)


if __name__ == '__main__':
    main(sys.argv[1:])
