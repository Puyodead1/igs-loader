/*
 * E3000 machine plugin.  The E3000 cabinets (Speed Driver 3, Power Truck,
 * Speed Rider 2, MuziBox) run E2100-generation software on a newer PC: the
 * same FPGA card behind /dev/pccard0 and the same IGS036 loader and
 * transports (P2A_Comm_*).  So this is machines/e2100 under its own name,
 * with the game interface of machines/e2100/e2100.h.  wiki E3000.
 */
#define MACHINE_NAME "e3000"
#include "../e2100/e2100.c"
