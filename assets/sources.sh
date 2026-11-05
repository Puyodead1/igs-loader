# libigsa and igs-assets (assets/), sourced by build.sh and win/build-win.sh.
# Links with the set builder's objects ($MKR_OBJ: zlib, the FPGA cipher) and
# the rom-set objects ($RS: software lists for the asset masks).
IGSA_INC="-Iassets -Iromset -Imkromset -Ithird_party/zlib -Ithird_party/stb"
IGSA_SRC="assets/igsa.c assets/image.c assets/lzss.c assets/formats.c $(ls assets/fmt_*.c)"
