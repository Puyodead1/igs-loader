/*
 * Stand-in for the cabinet's /lib/libcasmcard.so (smart-card reader).  The
 * E2000 executables link it by that absolute path.  Built with SONAME
 * "/lib/libcasmcard.so" and preloaded, it satisfies the NEEDED entry without
 * touching the host's /lib (ld.so matches a loaded object's SONAME against
 * NEEDED names).  The Windows loader preloads it as a DLL and takes the name
 * from igs_soname.
 *
 * Gold Captain and Pirate Revenge call nothing in it.  Go Go Ball 2's member
 * card reader (app/c/creader.c) calls the PC/SC-style Cas* functions: here
 * the card service is absent, which the game takes as "no card reader".
 *
 * Speed Rider links it as plain "libcasmcard.so" (its own copy sits in
 * bin_linux and talks to pcscd): built again with CASMCARD_SONAME
 * "libcasmcard.so" as libcasmcard-nocard.
 */
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#ifndef CASMCARD_SONAME
#define CASMCARD_SONAME "/lib/libcasmcard.so"
#endif

#define SCARD_E_NO_SERVICE 0x8010001DL

EXPORT const char igs_soname[] = CASMCARD_SONAME;

EXPORT long CasEstablishContext(unsigned long scope, const void *r1, const void *r2, long *ctx)
{
    (void)scope; (void)r1; (void)r2;
    if (ctx)
        *ctx = 0;
    return SCARD_E_NO_SERVICE;
}

EXPORT long CasReleaseContext(long ctx)
{
    (void)ctx;
    return 0;
}

EXPORT long CasListReaders(long ctx, const char *groups, char *readers, unsigned long *len)
{
    (void)ctx; (void)groups; (void)readers;
    if (len)
        *len = 0;
    return SCARD_E_NO_SERVICE;
}

EXPORT long CasConnect(long ctx, const char *reader, unsigned long share, unsigned long proto,
                       long *card, unsigned long *active)
{
    (void)ctx; (void)reader; (void)share; (void)proto; (void)card; (void)active;
    return SCARD_E_NO_SERVICE;
}

EXPORT long CasDisconnect(long card, unsigned long disposition)
{
    (void)card; (void)disposition;
    return 0;
}

EXPORT long CasStatus(long card, char *names, unsigned long *len, unsigned long *state,
                      unsigned long *proto, unsigned char *atr, unsigned long *atr_len)
{
    (void)card; (void)names; (void)state; (void)proto; (void)atr;
    if (len)
        *len = 0;
    if (atr_len)
        *atr_len = 0;
    return SCARD_E_NO_SERVICE;
}

/* SLE4442 memory-card calls (cdecl: the caller's arguments are ignored); only reached with a card;
   also Speed Driver 3's CasTransmit and CasGetRdrType, Power Truck's
   CasReconnect and CasGetStatusChange */
#define SLE_FAIL(name) EXPORT long name(void) { return SCARD_E_NO_SERVICE; }
SLE_FAIL(CasTransmit)
SLE_FAIL(CasGetRdrType)
SLE_FAIL(CasReconnect)
SLE_FAIL(CasGetStatusChange)
SLE_FAIL(SLE4442_Read_Main_Memory)
SLE_FAIL(SLE4442_Update_Main_MemoryA)
SLE_FAIL(SLE4442_Update_Main_Memory)
SLE_FAIL(SLE4442_Read_Protection_Memory)
SLE_FAIL(SLE4442_Write_Protection_Memory)
SLE_FAIL(SLE4442_Update_Security_Memory)
SLE_FAIL(SLE4442_Compare_Verification_Data)
