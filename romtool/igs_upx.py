"""Unpack a UPX-packed Linux i386 ELF (We Dance Online 2's `wedo`).

The executable is stock UPX (l_info version 13, format 12 = linux/elf386):
after the stub's two program headers come l_info, p_info (original file
size) and the compressed blocks, each a b_info {sz_unc, sz_cpr, method,
filter id, filter cto, 0} and its data; sz_unc 0 ends them.  The first
block is the original ELF header and program headers; then come each
PT_LOAD's file bytes (the first one's after the headers), then the gaps
between and after them in file order.  Only NRV2B/NRV2D/NRV2E (le32)
without filters is handled, which is all `wedo` uses.
wiki Disk Formats (E2000).

Usage: igs_upx.py <packed> <out>
"""
import struct
import sys


class _Bits:
    def __init__(self, src):
        self.src, self.ilen, self.bb, self.bc = src, 0, 0, 0

    def bit(self):
        if not self.bc:
            self.bb = struct.unpack_from('<I', self.src, self.ilen)[0]
            self.ilen += 4
            self.bc = 32
        self.bc -= 1
        return (self.bb >> self.bc) & 1

    def byte(self):
        b = self.src[self.ilen]
        self.ilen += 1
        return b


def _nrv(src, size, method):
    """NRV2B (2), NRV2D (5) or NRV2E (8), le32 bit buffer (UCL n2?_d.c)."""
    s, dst, last = _Bits(src), bytearray(), 1
    while True:
        while s.bit():
            dst.append(s.byte())
        m_off = 1
        if method == 2:
            while True:
                m_off = m_off * 2 + s.bit()
                if s.bit():
                    break
        else:
            while True:
                m_off = m_off * 2 + s.bit()
                if s.bit():
                    break
                m_off = (m_off - 1) * 2 + s.bit()
        if method == 2:
            if m_off == 2:
                m_off = last
            else:
                m_off = (m_off - 3) * 256 + s.byte()
                if m_off == 0xffffffff:
                    break
                last = m_off = m_off + 1
            m_len = s.bit() * 2 + s.bit()
            if not m_len:
                m_len = 1
                while True:
                    m_len = m_len * 2 + s.bit()
                    if s.bit():
                        break
                m_len += 2
            m_len += m_off > 0xd00
            m_len += 1
        else:
            if m_off == 2:
                m_off, m_len = last, s.bit()
            else:
                m_off = (m_off - 3) * 256 + s.byte()
                if m_off == 0xffffffff:
                    break
                m_len = (m_off ^ 0xffffffff) & 1
                m_off >>= 1
                last = m_off = m_off + 1
            if method == 5:
                m_len = m_len * 2 + s.bit()
                if not m_len:
                    m_len = 1
                    while True:
                        m_len = m_len * 2 + s.bit()
                        if s.bit():
                            break
                    m_len += 2
                m_len += m_off > 0x500
                m_len += 1
            else:
                if m_len:
                    m_len = 1 + s.bit()
                elif s.bit():
                    m_len = 3 + s.bit()
                else:
                    m_len = 1
                    while True:
                        m_len = m_len * 2 + s.bit()
                        if s.bit():
                            break
                    m_len += 3
                m_len += m_off > 0x500
                m_len += 1
        p = len(dst) - m_off
        if p < 0:
            raise ValueError('UPX: match before the start of the block')
        for k in range(m_len):
            dst.append(dst[p + k])
    if len(dst) != size:
        raise ValueError('UPX: block unpacked to %d bytes, expected %d' % (len(dst), size))
    return bytes(dst)


def unpack(data):
    i = data.find(b'UPX!', 0, 0x400) - 4
    if i < 0:
        raise ValueError('not UPX-packed')
    version, fmt = data[i + 10], data[i + 11]
    if fmt != 12:
        raise ValueError('UPX format %d, only linux/elf386 (12) is handled' % fmt)
    filesize = struct.unpack_from('<I', data, i + 16)[0]
    p, blocks = i + 24, []
    while True:
        u, c, method, ftid = struct.unpack_from('<IIBB', data, p)
        if not u:
            break
        if ftid:
            raise ValueError('UPX filter 0x%x not handled' % ftid)
        blk = data[p + 12:p + 12 + c]
        blocks.append(blk if c == u else _nrv(blk, u, method))
        p += 12 + c
    # the extents in the order UPX stored them: the headers, each PT_LOAD's
    # file bytes (the first after the headers), then the gaps in file order
    hdr = blocks[0]
    phoff, phentsize, phnum = struct.unpack_from('<I', hdr, 28)[0], *struct.unpack_from('<HH', hdr, 42)
    extents = [(0, len(hdr))]
    for k in range(phnum):
        typ, off, _, _, filesz = struct.unpack_from('<IIIII', hdr, phoff + k * phentsize)
        if typ == 1 and filesz:
            if off < len(hdr):
                filesz, off = filesz - (len(hdr) - off), len(hdr)
            extents.append((off, filesz))
    pos = 0
    for off, n in sorted(extents):
        if off > pos:
            extents.append((pos, off - pos))
        pos = max(pos, off + n)
    if pos < filesize:
        extents.append((pos, filesize - pos))
    out = bytearray(filesize)
    blocks = iter(blocks)
    for off, n in extents:
        got = b''
        while len(got) < n:
            got += next(blocks)
        if len(got) != n:
            raise ValueError('UPX: block sizes do not follow the extents')
        out[off:off + n] = got
    return bytes(out)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        out = unpack(f.read())
    with open(sys.argv[2], 'wb') as f:
        f.write(out)
    print('%s: %d bytes' % (sys.argv[2], len(out)))
