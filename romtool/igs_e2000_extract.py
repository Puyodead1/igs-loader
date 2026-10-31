"""Extract an E2000 game image (Linux 2.4.31 IGS kernels) into a work folder.

Disk layout (all E2000 images seen so far):
  p1 ext2   LILO map + bzImage kernel, and a raw stage-2 loader stub
  p2 ROFS   root filesystem, plaintext cramfs variant  -> <work>/root
  p3 SHFS   game data, SquashFS 2.x variant           -> <work>/game
  p4 ext3   /<Game>/var, writable settings/bookkeeping (not extracted)

The game executable is not stored whole.  At boot the kernel runs
/bin/csh /lib/libdb0.a __KERNEL__ "-no-print -GC", which writes
  /mnt/head = 1 KiB from p2 sector HEAD_SECTOR (the executable's first KiB)
  /mnt/GECA = 1 KiB from p1 sector GECA_SECTOR + /lib/libdb0.a  (an ELF)
and the kernel then runs /mnt/GECA, which builds /Game/<BINARY> as
  head + libdb<PIECES[0]>.a + libdb<PIECES[1]>.a + ...
taking (size/1K - trim) KiB of each piece, trim starting at TRIM_KB and
changing by TRIM_STEP after each piece; the last piece keeps
size - trim*1K bytes.  Details: docs/IGS_ROFS_REVERSE_ENGINEERING.md.

The software list (hash/e2000.xml) supplies the features geca_sector,
head_sector, pieces, trim_kb, trim_step (read from /bin/csh and GECA of each
game, see docs/IGS_KEYS.md).

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


def build_executable(image, parts, root, m):
    with open(image, 'rb') as f:
        f.seek((parts[1][0] + int(m['HEAD_SECTOR'])) * 512)
        out = f.read(1024)
    if out[:4] != b'\x7fELF':
        sys.exit('HEAD_SECTOR does not point at an ELF header')
    trim, step = int(m['TRIM_KB']), int(m['TRIM_STEP'])
    pieces = m['PIECES']
    for i, d in enumerate(pieces):
        with open(os.path.join(root, 'lib', 'libdb%s.a' % d), 'rb') as f:
            piece = f.read()
        if i == len(pieces) - 1:
            out += piece[:len(piece) - trim * 1024]
        else:
            out += piece[:(len(piece) // 1024 - trim) * 1024]
            trim += step
    # the section header table ends the file: a cheap integrity check
    shoff, = struct.unpack_from('<I', out, 0x20)
    shnum, = struct.unpack_from('<H', out, 0x30)
    if shoff + 40 * shnum != len(out):
        sys.exit('assembled executable is %d bytes, ELF header expects %d' % (len(out), shoff + 40 * shnum))
    return out


def build_geca(image, parts, root, m):
    with open(image, 'rb') as f:
        f.seek((parts[0][0] + int(m['GECA_SECTOR'])) * 512)
        stub = f.read(1024)
    with open(os.path.join(root, 'lib', 'libdb0.a'), 'rb') as f:
        return stub + f.read()


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
