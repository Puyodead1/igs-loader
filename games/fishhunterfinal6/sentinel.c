/*
 * Sentinel Keys dongle HLE (sentinel.h).
 *
 * The client's API functions are obfuscated, but each starts with
 *   55 89 E5 83 EC 38 C7 45 D8 <n> 00 00 00   (push ebp; ...; mov [ebp-28h], n)
 * where n is the function's number in the SDK's API: 1 SFNTGetLicense,
 * 7 SFNTReadRawData, 11 SFNTDecrypt, 15 SFNTReleaseLicense (the ones the
 * games call).  The key holds a raw data object, stored AES-128 encrypted,
 * and an AES feature that decrypts 16-byte blocks of it.
 */
#include <string.h>

#include "sentinel.h"
#include "../../third_party/lzma/Aes.h"

/* SP_STATUS: 0 is success; the games test only that */
#define SP_SUCCESS          0
#define SP_ERR_INVALID_PARM 1
#define SP_ERR_NO_LICENSE   2
#define SP_ERR_BAD_FEATURE  3

#define HANDLE 0x534B0001u     /* the one license handle */

static const struct igs_core *core;
static struct sentinel_key key;
static uint8_t stored[SNT_RAW_MAX];          /* the data object as the key stores it */
static int licensed;
static UInt32 aes_enc[AES_NUM_IVMRK_WORDS] __attribute__((aligned(16)));
static UInt32 aes_dec[AES_NUM_IVMRK_WORDS] __attribute__((aligned(16)));

/* one 16-byte block, ECB (CBC with a zero IV over one block) */
static void aes_block(UInt32 *ivaes, AES_CODE_FUNC *fn, const uint8_t *in, uint8_t *out)
{
    static const Byte zero[16];
    /* the game's threads give no 16-byte stack alignment: a static block */
    static Byte b[16] __attribute__((aligned(16)));

    memcpy(b, in, 16);
    AesCbc_Init(ivaes, zero);
    (*fn)(ivaes, b, 1);
    memcpy(out, b, 16);
}

/* 1: SFNTGetLicense(developer id, software key, license id, flags, &handle) */
static int snt_get_license(uint32_t dev, const void *swkey, uint32_t lic, uint32_t flags, uint32_t *handle)
{
    (void)swkey;
    if (!handle)
        return SP_ERR_INVALID_PARM;
    if (dev != key.developer_id) {
        core->log("sentinel: no key for developer 0x%08x", dev);
        return SP_ERR_NO_LICENSE;
    }
    *handle = HANDLE;
    licensed = 1;
    if (core->verbose)
        core->log("sentinel: license %u (flags 0x%x)", lic, flags);
    return SP_SUCCESS;
}

/* 7: SFNTReadRawData(handle, feature, buffer, offset, length) */
static int snt_read_raw(uint32_t h, uint32_t fid, uint8_t *buf, uint32_t off, uint32_t len)
{
    if (h != HANDLE || !licensed || !buf)
        return SP_ERR_INVALID_PARM;
    if (fid != key.raw_feature || off > key.raw_len || len > key.raw_len - off)
        return SP_ERR_BAD_FEATURE;
    memcpy(buf, stored + off, len);
    return SP_SUCCESS;
}

/* 11: SFNTDecrypt(handle, feature, cipher block, plain block) */
static int snt_decrypt(uint32_t h, uint32_t fid, const uint8_t *in, uint8_t *out)
{
    if (h != HANDLE || !licensed || !in || !out)
        return SP_ERR_INVALID_PARM;
    if (fid != key.aes_feature)
        return SP_ERR_BAD_FEATURE;
    aes_block(aes_dec, &g_AesCbc_Decode, in, out);
    return SP_SUCCESS;
}

/* 15: SFNTReleaseLicense(handle) */
static int snt_release(uint32_t h)
{
    if (h != HANDLE)
        return SP_ERR_INVALID_PARM;
    licensed = 0;
    return SP_SUCCESS;
}

/* the executable's code: its PF_X PT_LOAD segments, from the ELF header at 0x8048000 */
static uint32_t find_api(uint32_t n)
{
    static const uint8_t sig[] = { 0x55, 0x89, 0xE5, 0x83, 0xEC, 0x38, 0xC7, 0x45, 0xD8 };
    const uint8_t *eh = (const uint8_t *)(uintptr_t)0x08048000;
    uint32_t phoff, i, found = 0;
    uint16_t phnum, phent;

    if (memcmp(eh, "\x7f" "ELF", 4))
        return 0;
    memcpy(&phoff, eh + 0x1C, 4);
    memcpy(&phent, eh + 0x2A, 2);
    memcpy(&phnum, eh + 0x2C, 2);
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = eh + phoff + i * phent;
        uint32_t type, vaddr, filesz, flags, a;

        memcpy(&type, ph, 4);
        memcpy(&vaddr, ph + 8, 4);
        memcpy(&filesz, ph + 16, 4);
        memcpy(&flags, ph + 24, 4);
        if (type != 1 || !(flags & 1))
            continue;
        for (a = vaddr; a + 13 <= vaddr + filesz; a++) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)a;

            if (p[0] == 0x55 && !memcmp(p, sig, sizeof(sig)) && p[9] == n && !p[10] && !p[11] && !p[12]) {
                if (found)
                    return 0;                /* ambiguous */
                found = a;
            }
        }
    }
    return found;
}

int sentinel_install(const struct igs_core *c, const struct sentinel_key *k)
{
    static const struct { uint32_t n; void *fn; } api[] = {
        { 1, (void *)snt_get_license }, { 7, (void *)snt_read_raw },
        { 11, (void *)snt_decrypt }, { 15, (void *)snt_release },
    };
    uint32_t i;
    int hooked = 0;

    core = c;
    key = *k;
    if (key.raw_len > SNT_RAW_MAX || key.raw_len % 16)
        return -1;
    AesGenTables();
    Aes_SetKey_Enc(aes_enc + 4, key.aes_key, 16);
    Aes_SetKey_Dec(aes_dec + 4, key.aes_key, 16);
    for (i = 0; i < key.raw_len; i += 16)
        aes_block(aes_enc, &g_AesCbc_Encode, key.raw + i, stored + i);
    for (i = 0; i < sizeof(api) / sizeof(api[0]); i++) {
        uint32_t a = find_api(api[i].n);

        if (!a || !core->hook || core->hook(a, api[i].fn)) {
            core->log("sentinel: API %u not found", api[i].n);
            return -1;
        }
        hooked++;
    }
    return hooked;
}
