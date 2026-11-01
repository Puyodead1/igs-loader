"""Extract an E2000 game image (Linux 2.4.31 IGS kernels) into a work folder.

Disk layout (all E2000 images seen so far):
  p1 ext2   LILO map + bzImage kernel, and a raw stage-2 loader stub
  p2 ROFS   root filesystem, plaintext cramfs variant  -> <work>/root
  p3 SHFS   game data, SquashFS 2.x variant           -> <work>/game
  p4 ext3   /<Game>/var, writable settings/bookkeeping (not extracted)

The game executable is not stored whole.  At boot the kernel runs a stage-1
loader (Gold Captain /bin/csh, Pirate Revenge /bin/HWtest), which writes
  /mnt/head = 1 KiB from HEAD_PART sector HEAD_SECTOR (the executable's first KiB)
  /mnt/GECA = 1 KiB from GECA_PART sector GECA_SECTOR (or the first KiB of
              the root-fs file GECA_HEAD) + GECA_FILE  (an ELF)
and the kernel then runs /mnt/GECA, which builds the executable as
  head + piece(PIECES[0]) + piece(PIECES[1]) + ...
where piece(d) is PIECE_FILE with %s = d.  PIECE_MODE says how much of each
piece is kept; n starts at TRIM_KB and changes by TRIM_STEP after each piece:
  trim (Gold Captain)     the first size/1K - n KiB; the last piece keeps
                          size - n*1K bytes
  skip (Pirate Revenge)   everything after the first n KiB
  cut (Speed Rider)       skip the first n KiB and drop the last t KiB of the
                          whole KiB (last piece: of the bytes); t starts at
                          CUT_KB, changes by CUT_STEP; empty or missing
                          pieces are passed over and do not step
Details: docs/IGS_ROFS_REVERSE_ENGINEERING.md.

The software list (hash/e2000.xml) supplies the features geca_sector,
head_sector, pieces, trim_kb, trim_step and, where they differ from Gold
Captain's, head_part (default p2), geca_part (p1) or geca_head, geca_file
(/lib/libdb0.a), piece_file (/lib/libdb%s.a), piece_mode (trim), and for
cut cut_kb, cut_step; see
docs/IGS_KEYS.md.

This is the development workspace layout (root/, game/, <BINARY>.bin,
loader/GECA).  Rom sets for the loader are built by mkromset.py, which uses
build_executable() from here.

Usage: igs_e2000_extract.py <id> <image.img> <work dir>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import igs_rofs24     # noqa: E402
import igs_shfs       # noqa: E402
import softlist       # noqa: E402


def partitions(image):
    with open(image, 'rb') as f:
        mbr = f.read(512)
    return [struct.unpack_from('<II', mbr, 446 + 16 * i + 8) for i in range(4)]


def part_index(m, key, default):
    """A partition feature ('p1'..'p4') as an index into partitions()."""
    p = m.get(key, default)
    if len(p) != 2 or p[0] != 'p' or p[1] not in '1234':
        sys.exit('bad %s %r' % (key.lower(), p))
    return int(p[1]) - 1


def root_file(root, cab):
    with open(os.path.join(root, cab.lstrip('/')), 'rb') as f:
        return f.read()


def build_executable(image, parts, root, m):
    with open(image, 'rb') as f:
        f.seek((parts[part_index(m, 'HEAD_PART', 'p2')][0] + int(m['HEAD_SECTOR'])) * 512)
        out = f.read(1024)
    if out[:4] != b'\x7fELF':
        sys.exit('HEAD_SECTOR does not point at an ELF header')
    n, step = int(m['TRIM_KB']), int(m['TRIM_STEP'])
    pieces, mode = m['PIECES'], m.get('PIECE_MODE', 'trim')
    if mode not in ('trim', 'skip', 'cut'):
        sys.exit('bad piece_mode %r' % mode)
    if mode == 'cut':
        t, tstep = int(m['CUT_KB']), int(m['CUT_STEP'])
    for i, d in enumerate(pieces):
        path = os.path.join(root, (m.get('PIECE_FILE', '/lib/libdb%s.a') % d).lstrip('/'))
        if mode == 'cut':
            # missing or empty pieces are passed over without stepping
            piece = open(path, 'rb').read() if os.path.isfile(path) else b''
            if not piece:
                continue
            if i == len(pieces) - 1:
                out += piece[n * 1024:len(piece) - t * 1024]
            else:
                out += piece[n * 1024:(len(piece) // 1024 - t) * 1024]
            n, t = n + step, t + tstep
            continue
        piece = root_file(root, m.get('PIECE_FILE', '/lib/libdb%s.a') % d)
        if mode == 'skip':
            out += piece[n * 1024:]
        elif i == len(pieces) - 1:
            out += piece[:len(piece) - n * 1024]
        else:
            out += piece[:(len(piece) // 1024 - n) * 1024]
        n += step
    # the section header table ends the file: a cheap integrity check
    shoff, = struct.unpack_from('<I', out, 0x20)
    shnum, = struct.unpack_from('<H', out, 0x30)
    if shoff + 40 * shnum != len(out):
        sys.exit('assembled executable is %d bytes, ELF header expects %d' % (len(out), shoff + 40 * shnum))
    return out


def build_geca(image, parts, root, m):
    if 'GECA_HEAD' in m:
        # Panda Family: the stub is the first KiB of a root-fs file
        stub = root_file(root, m['GECA_HEAD'])[:1024]
    else:
        with open(image, 'rb') as f:
            f.seek((parts[part_index(m, 'GECA_PART', 'p1')][0] + int(m['GECA_SECTOR'])) * 512)
            stub = f.read(1024)
    return stub + root_file(root, m.get('GECA_FILE', '/lib/libdb0.a'))


def main():
    if len(sys.argv) != 4:
        sys.exit('usage: igs_e2000_extract.py <id> <image.img> <work dir>')
    m = softlist.get(sys.argv[1]).manifest()
    image, work = sys.argv[2], sys.argv[3]
    parts = partitions(image)
    root, game = os.path.join(work, 'root'), os.path.join(work, 'game')
    print('p2 root ROFS ->', root)
    igs_rofs24.extract(image, root, parts[1][0] * 512, parts[1][1] * 512)
    print('p3 game SHFS ->', game)
    igs_shfs.extract(image, game, parts[2][0] * 512)
    os.makedirs(os.path.join(work, 'loader'), exist_ok=True)
    with open(os.path.join(work, 'loader', 'GECA'), 'wb') as f:
        f.write(build_geca(image, parts, root, m))
    exe = build_executable(image, parts, root, m)
    path = os.path.join(work, m['BINARY'] + '.bin')
    with open(path, 'wb') as f:
        f.write(exe)
    os.chmod(path, 0o755)
    print('executable ->', path, len(exe), 'bytes')
    # the game directory's <BINARY> points at the cabinet's ramfs (/Game/...):
    # point it at the reassembled executable instead
    link = os.path.join(game, m['BINARY'])
    if os.path.islink(link):
        os.remove(link)
        os.symlink(os.path.join('..', m['BINARY'] + '.bin'), link)


if __name__ == '__main__':
    main()
