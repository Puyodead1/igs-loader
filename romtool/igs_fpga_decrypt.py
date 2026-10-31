"""Decrypt IGS E2100 "FPGA_AES" protected assets (sound/*.wav, action/*.rom).

The FPGA cipher is not AES. Each 16-byte ECB block is:
  plaintext bytes -> byte shuffle -> two 64-bit LE halves rotated left
  (low half by 1, high half by 3) -> XOR with a constant mask.
Recovered from known WAV header plaintext; see IGS_SOUND_CRYPTO.md.
Only the mask differs between games (it follows the ASIC's runtime key);
it comes from the game's software-list entry (hash/<machine>.xml, feature
asset_mask): --game <id>.
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import softlist   # noqa: E402

MASK = bytes.fromhex('eea8543c6a16835d0900d7739f6bf77f')  # Harpoon Lagoon
# plaintext byte i comes from unrotated byte PERM[i]
PERM = [14, 10, 8, 1, 0, 12, 9, 4, 3, 6, 11, 7, 2, 13, 5, 15]
FLIP = {1, 4, 6, 7, 10, 12, 13, 15}  # unrotated slots whose bit 7 is inverted
M64 = (1 << 64) - 1


def _rotr(x, r):
    return ((x >> r) | (x << (64 - r))) & M64


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def decrypt_block(c):
    d = bytes(a ^ b for a, b in zip(c, MASK))
    u = (_rotr(int.from_bytes(d[:8], 'little'), 1).to_bytes(8, 'little')
         + _rotr(int.from_bytes(d[8:], 'little'), 3).to_bytes(8, 'little'))
    u = bytes(b ^ 0x80 if i in FLIP else b for i, b in enumerate(u))
    return bytes(u[PERM[i]] for i in range(16))


def encrypt_block(p):
    u = bytearray(16)
    for i in range(16):
        u[PERM[i]] = p[i]
    u = bytes(b ^ 0x80 if i in FLIP else b for i, b in enumerate(u))
    d = (_rotl(int.from_bytes(u[:8], 'little'), 1).to_bytes(8, 'little')
         + _rotl(int.from_bytes(u[8:], 'little'), 3).to_bytes(8, 'little'))
    return bytes(a ^ b for a, b in zip(d, MASK))


def decrypt(data):
    if len(data) % 16:
        data += bytes(16 - len(data) % 16)
    try:
        import numpy as np
    except ImportError:
        return b''.join(decrypt_block(data[i:i + 16]) for i in range(0, len(data), 16))
    a = np.frombuffer(data, dtype=np.uint8).reshape(-1, 16) ^ np.frombuffer(MASK, dtype=np.uint8)
    h = a.copy().view('<u8')
    h[:, 0] = (h[:, 0] >> np.uint64(1)) | (h[:, 0] << np.uint64(63))
    h[:, 1] = (h[:, 1] >> np.uint64(3)) | (h[:, 1] << np.uint64(61))
    u = h.view(np.uint8).reshape(-1, 16)
    u[:, sorted(FLIP)] ^= 0x80
    return u[:, PERM].tobytes()


if __name__ == '__main__':
    args = sys.argv[1:]
    if len(args) == 4 and args[0] == '--game':
        MASK = bytes.fromhex(softlist.get(args[1]).feature('asset_mask'))
        args = args[2:]
    if len(args) != 2:
        sys.exit('usage: igs_fpga_decrypt.py [--game <id>] <input file|dir> <output file|dir>\n'
                 'ids: ' + ', '.join(sorted(softlist.load())))
    src, dst = args
    if os.path.isdir(src):
        os.makedirs(dst, exist_ok=True)
        for name in sorted(os.listdir(src)):
            with open(os.path.join(src, name), 'rb') as f:
                out = decrypt(f.read())
            with open(os.path.join(dst, name), 'wb') as f:
                f.write(out)
    else:
        with open(src, 'rb') as f:
            out = decrypt(f.read())
        with open(dst, 'wb') as f:
            f.write(out)
