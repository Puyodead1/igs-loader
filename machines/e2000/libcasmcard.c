/*
 * Stand-in for the cabinet's /lib/libcasmcard.so (smart-card reader).  The
 * E2000 executables link it by that absolute path but call nothing in it.
 * Built with SONAME "/lib/libcasmcard.so" and preloaded, it satisfies the
 * NEEDED entry without touching the host's /lib (ld.so matches a loaded
 * object's SONAME against NEEDED names).
 */
int igs_casmcard_stub;
