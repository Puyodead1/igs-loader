/*
 * Stand-in for the cabinet's /lib/libcasmcard.so (smart-card reader).  The
 * E2000 executables link it by that absolute path but call nothing in it.
 * Built with SONAME "/lib/libcasmcard.so" and preloaded, it satisfies the
 * NEEDED entry without touching the host's /lib (ld.so matches a loaded
 * object's SONAME against NEEDED names).  The Windows loader preloads it as
 * a DLL and takes the name from igs_soname.
 */
#ifdef _WIN32
__declspec(dllexport)
#endif
const char igs_soname[] = "/lib/libcasmcard.so";
