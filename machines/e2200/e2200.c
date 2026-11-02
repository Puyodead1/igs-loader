/*
 * E2200 machine plugin.  The PC side of the E2200 (Percussion Master 3) is
 * the E2100's: the same 2.6.32.8 "James" kernel family, the same FPGA card
 * behind /dev/pccard0 (ioctls, BARs, asset cipher) and the same IGS036
 * loader and request/response library in the game (P2A_Comm_*,
 * Igs036P2A).  What differs is outside the PC: the drum I/O board, which
 * the IGS036 relays (games/percussionmaster3).  So this is machines/e2100
 * under its own name, with the game interface of machines/e2100/e2100.h.
 * docs/IGS_E2200.md.
 */
#define MACHINE_NAME "e2200"
#include "../e2100/e2100.c"
