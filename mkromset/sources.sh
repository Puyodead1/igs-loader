# igs-mkromset's sources and flags, sourced by build.sh and win/build-win.sh
# (on top of their romset/ objects, $RS).  Objects are named after the whole
# path: zlib's zip.c and libchdr's LzmaDec.c share names with romset/ and
# third_party/lzma files.
MKR_LZ=third_party/lzma
MKR_ZL=third_party/zlib
MKR_CH=third_party/libchdr
MKR_INC="-Imkromset -Iromset -I$MKR_LZ -I$MKR_ZL -I$MKR_ZL/minizip -I$MKR_CH/include -I$MKR_CH/deps/zstd-1.5.7"
# libchdr: zlib from third_party/zlib, zstd and its LZMA decoder (renamed CHDR_*) bundled
MKR_DEF="-DCHDR_SYSTEM_ZLIB -DWANT_RAW_DATA_SECTOR=1 -DWANT_SUBCODE=1 -DVERIFY_BLOCK_CRC=1 -DLOWRAM_TARGET=0 \
-D_LARGEFILE64_SOURCE"
MKR_SRC="mkromset/build.c mkromset/vfs.c mkromset/image.c mkromset/fs_rofs.c mkromset/fs_rofs24.c
    mkromset/fs_squashfs.c mkromset/fs_ext2.c mkromset/exe.c mkromset/pack_7z.c mkromset/pack_zip.c
    $MKR_LZ/LzmaEnc.c $MKR_LZ/Lzma2Enc.c $MKR_LZ/LzFind.c $MKR_LZ/LzFindMt.c $MKR_LZ/LzFindOpt.c
    $MKR_LZ/MtCoder.c $MKR_LZ/MtDec.c $MKR_LZ/Threads.c $MKR_LZ/Alloc.c $MKR_LZ/Aes.c $MKR_LZ/AesOpt.c
    $MKR_LZ/Sha1.c $MKR_LZ/Sha1Opt.c $MKR_LZ/Sha256.c $MKR_LZ/Sha256Opt.c $MKR_LZ/Xz.c $MKR_LZ/XzDec.c
    $MKR_LZ/XzCrc64.c $MKR_LZ/XzCrc64Opt.c
    $MKR_ZL/adler32.c $MKR_ZL/compress.c $MKR_ZL/crc32.c $MKR_ZL/deflate.c $MKR_ZL/inffast.c $MKR_ZL/inflate.c
    $MKR_ZL/inftrees.c $MKR_ZL/trees.c $MKR_ZL/uncompr.c $MKR_ZL/zutil.c $MKR_ZL/minizip/zip.c $MKR_ZL/minizip/ioapi.c
    $MKR_CH/src/libchdr_bitstream.c $MKR_CH/src/libchdr_cdrom.c $MKR_CH/src/libchdr_chd.c
    $MKR_CH/src/libchdr_codec_avhuff.c $MKR_CH/src/libchdr_codec_cdfl.c $MKR_CH/src/libchdr_codec_cdlz.c
    $MKR_CH/src/libchdr_codec_cdzl.c $MKR_CH/src/libchdr_codec_cdzs.c $MKR_CH/src/libchdr_codec_flac.c
    $MKR_CH/src/libchdr_codec_huff.c $MKR_CH/src/libchdr_codec_lzma.c $MKR_CH/src/libchdr_codec_zlib.c
    $MKR_CH/src/libchdr_codec_zstd.c $MKR_CH/src/libchdr_flac.c $MKR_CH/src/libchdr_huffman.c
    $MKR_CH/deps/lzma-26.02/src/LzmaDec.c $MKR_CH/deps/zstd-1.5.7/zstddeclib.c"

# mkr_objs <compiler> <flags> <object dir>: compile what changed, print the objects
mkr_objs() {
    local s o objs=
    mkdir -p "$3"
    for s in $MKR_SRC; do
        o=$3/$(echo "${s%.*}" | tr '/' '_').o
        # third-party code gets no warnings of ours
        case $s in mkromset/*) w="-Wall";; *) w="-w";; esac
        # ours include mkr.h, mkromset.h and romset.h
        if [ ! "$o" -nt "$s" ] || { [ "${s#mkromset/}" != "$s" ] && { [ ! "$o" -nt mkromset/mkr.h ] ||
            [ ! "$o" -nt mkromset/mkromset.h ] || [ ! "$o" -nt romset/romset.h ]; }; }; then
            $1 $2 $w $MKR_INC $MKR_DEF -c -o "$o" "$s" || return 1
        fi
        objs="$objs $o"
    done
    echo "$objs"
}
