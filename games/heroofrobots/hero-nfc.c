/*
 * hero-nfc: creates, inspects, edits and erases Hero of Robots NFC cards
 * (NTAG215 on an ACR122U, or the fake reader's tag.bin) in the loader's tag
 * format.  Experimental, like the game side.  wiki Hero of Robots, NFC.
 *
 * The readers and the format are the game's own (nfc_tag.c).  Every write
 * asks first (--yes skips), touches user pages 4..129 only (format: also
 * the capability container of a tag that has none), and is read back.
 *
 * Exit codes: 0 ok, 1 failed or declined, 2 usage, 3 no reader or no tag.
 */
#ifdef _WIN32
#include <windows.h>
#include <pthread.h>            /* clock_gettime (winpthreads) */
#define RTLD_NOW 2
static void *dlopen(const char *path, int flags)
{
    (void)flags;
    return (void *)LoadLibraryA(path);
}
static void *dlsym(void *h, const char *name)
{
    return (void *)GetProcAddress((HMODULE)h, name);
}
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <stdarg.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <sys/stat.h>

static int verbose;
#define NLOG(...) do { if (verbose) { fputs("hero-nfc: ", stderr); fprintf(stderr, __VA_ARGS__); \
                                      fputc('\n', stderr); } } while (0)

#include "nfc_tag.c"
#include "romset.h"

#define DEFAULT_GAME "heroofrobots_m100fa"
#define CSV_REL "media/localization/Card/QRCardRes.csv"
#define SPECIAL_CODE "012900908"

enum { OK, FAIL, USAGE, NOTAG };

static struct {
    const char *reader, *fake, *csv, *set, *game, *root;
    int wait, yes, force, unformatted;
} opt;

static int err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int hush;                        /* load_cards(1): no message when the list is missing */

/* the GUI (hero-nfc-gui.c): messages and confirmations */
static void (*err_hook)(const char *msg);
static int (*confirm_hook)(const char *what);

static int err(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    if (hush)
        return FAIL;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (err_hook)
        err_hook(msg);
    else
        fprintf(stderr, "hero-nfc: %s\n", msg);
    return FAIL;
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static int file_exists(const char *p)
{
    struct stat st;

    return !stat(p, &st) && !(st.st_mode & S_IFDIR);
}

static int dir_exists(const char *p)
{
    struct stat st;

    return !stat(p, &st) && (st.st_mode & S_IFDIR);
}

static void hex(char *out, const uint8_t *p, int n)
{
    int i;

    for (i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02X", p[i]);
    out[2 * n] = 0;
}

static int confirm(const char *what)
{
    char line[64] = "", *a = line;

    if (confirm_hook)
        return opt.yes || confirm_hook(what);
    printf("%s\n", what);
    if (opt.yes)
        return 1;
    printf("Proceed? [y/N] ");
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) && !memcmp(a, "\xEF\xBB\xBF", 3))
        a += 3;                         /* PowerShell pipes a UTF-8 BOM */
    if (!isatty(fileno(stdin)))
        printf("%.1s\n", a);            /* the piped answer, as if typed */

    if (*a != 'y' && *a != 'Y') {
        puts("cancelled");
        return 0;
    }
    return 1;
}

/* ---- the card list (QRCardRes.csv) ------------------------------------------------ */

struct card {
    char kind[8], code[16], soft[16], printed[16], name[96];
    int boss;                           /* no numeric code: cannot be scanned */
};

static struct card *cards;
static int ncards, cards_tried;
static char csv_path[RS_PATH];

static void loader_root(char *root, size_t n)
{
    char p[RS_PATH], *s;
    int i;

    if (opt.root) {
        snprintf(root, n, "%s", opt.root);
        return;
    }
#ifdef _WIN32
    /* next to the exe (a release), or two up (build\win\) */
    GetModuleFileNameA(NULL, p, sizeof(p));
    if ((s = strrchr(p, '\\')))
        *s = 0;
    snprintf(root, n, "%s\\hash", p);
    if (dir_exists(root)) {
        snprintf(root, n, "%s", p);
        return;
    }
    for (i = 0; i < 2 && (s = strrchr(p, '\\')); i++)
        *s = 0;
#else
    /* build/hero-nfc: the parent of build/ */
    ssize_t len = readlink("/proc/self/exe", p, sizeof(p) - 1);

    p[len > 0 ? len : 0] = 0;
    for (i = 0; i < 2 && (s = strrchr(p, '/')); i++)
        *s = 0;
#endif
    snprintf(root, n, "%s", p);
}

static int try_csv(const char *dir, const char *rel)
{
    snprintf(csv_path, sizeof(csv_path), "%s/%s", dir, rel);
    return file_exists(csv_path);
}

/* --csv, --set, else the rom set on the rompath (a folder, or an archive's extracted copy) */
static int find_csv(void)
{
    char root[RS_PATH], set[RS_PATH];
    const char *game = opt.game ? opt.game : DEFAULT_GAME;
    int kind;

    if (opt.csv) {
        snprintf(csv_path, sizeof(csv_path), "%s", opt.csv);
        return file_exists(csv_path) ? 0 : err("%s not found", opt.csv);
    }
    if (opt.set) {
        if (try_csv(opt.set, "Robots/" CSV_REL) || try_csv(opt.set, CSV_REL) ||
            try_csv(opt.set, "../" CSV_REL) || try_csv(opt.set, "QRCardRes.csv"))
            return 0;
        return err("no %s in %s", "QRCardRes.csv", opt.set);
    }
    loader_root(root, sizeof(root));
    rs_config(root);
    kind = rs_locate(game, set, sizeof(set));
    if (kind == RS_DIR && try_csv(set, "Robots/" CSV_REL))
        return 0;
    if (kind != RS_NONE) {
        snprintf(set, sizeof(set), "%s/%s", rs_cachepath(), game);
        if (try_csv(set, "Robots/" CSV_REL))
            return 0;
        return err("the set %s is not extracted yet (run the game once, or use --set / --csv)", game);
    }
    return err("no rom set %s on the rompath %s (use --set <dir> or --csv <file>)", game,
               rs_rompath()[0] ? rs_rompath() : "(empty)");
}

static void field(char *dst, size_t n, const char *s, const char *e)
{
    while (e > s && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n'))
        e--;
    snprintf(dst, n, "%.*s", (int)(e - s), s);
}

static int is_code9(const char *s)
{
    int i;

    for (i = 0; i < 9; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return !s[9];
}

/* 0: loaded; quiet: no message when missing */
static int load_cards(int quiet)
{
    char line[512];
    FILE *f;
    int r;

    if (cards_tried)
        return ncards ? 0 : -1;
    cards_tried = 1;
    hush = quiet && !opt.csv && !opt.set;
    r = find_csv();
    hush = 0;
    if (r || !(f = fopen(csv_path, "r")))
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char *col[6], *p = line;
        struct card c;
        int n = 0;

        for (col[n++] = p; n < 6 && (p = strchr(p, ',')); col[n++] = ++p)
            ;
        if (n < 6 || (strncmp(line, "ROBOT,", 6) && strncmp(line, "POWER,", 6) &&
                      strncmp(line, "SKILL,", 6)))
            continue;
        memset(&c, 0, sizeof(c));
        field(c.kind, sizeof(c.kind), col[0], col[1] - 1);
        field(c.code, sizeof(c.code), col[2], col[3] - 1);
        field(c.soft, sizeof(c.soft), col[3], col[4] - 1);
        field(c.printed, sizeof(c.printed), col[4], col[5] - 1);
        p = strchr(col[5], ',');
        field(c.name, sizeof(c.name), col[5], p ? p : col[5] + strlen(col[5]));
        if (!c.code[0])
            continue;
        c.boss = !is_code9(c.code);
        if (!(ncards & 255))
            cards = realloc(cards, (ncards + 256) * sizeof(*cards));
        cards[ncards++] = c;
    }
    fclose(f);
    NLOG("%d cards in %s", ncards, csv_path);
    return ncards ? 0 : err("no cards in %s", csv_path);
}

static const struct card *card_by_code(const char *code)
{
    int i;

    for (i = 0; i < ncards; i++)
        if (!strcmp(cards[i].code, code))
            return &cards[i];
    return NULL;
}

static const char *kind_of_code(const char *code)
{
    if (!is_code9(code) || strncmp(code, "0102", 4))
        return "?";
    return code[6] == '1' ? "ROBOT" : code[6] == '2' ? "POWER" : code[6] == '3' ? "SKILL" : "?";
}

static void describe_code(char *out, size_t n, const char *code)
{
    const struct card *c = load_cards(1) ? NULL : card_by_code(code);

    if (c)
        snprintf(out, n, "%s %s (%s, %s)", code, c->name, c->kind, c->printed);
    else if (!strcmp(code, SPECIAL_CODE))
        snprintf(out, n, "%s (special: the game's built-in card 7)", code);
    else if (ncards)
        snprintf(out, n, "%s (not in the card list)", code);
    else
        snprintf(out, n, "%s (%s)", code, kind_of_code(code));
}

static int cmd_list(const char *filter)
{
    int i, shown = 0, boss;

    if (load_cards(0))
        return FAIL;
    for (i = 0; i < ncards; i++) {
        const struct card *c = &cards[i];

        if (filter && !ci_contains(c->code, filter) && !ci_contains(c->kind, filter) &&
            !ci_contains(c->printed, filter) && !ci_contains(c->soft, filter) &&
            !ci_contains(c->name, filter) && !(c->boss && !strcasecmp(filter, "boss")))
            continue;
        printf("%-9s  %-5s  %-7s  %s%s\n", c->code, c->kind, c->printed, c->name,
               c->boss ? "  [boss: not writable]" : "");
        shown++;
    }
    for (i = boss = 0; i < ncards; i++)
        boss += cards[i].boss;
    printf("%d shown; %d cards, %d of them boss cards (%s)\n", shown, ncards, boss, csv_path);
    return shown ? OK : FAIL;
}

/* a code, printed id, software id or name -> the code to write */
static int resolve_card(const char *arg, char *code)
{
    const struct card *hit = NULL;
    int i, hits = 0;

    if (strlen(arg) > 12)
        return err("%s: a card code has at most 12 characters", arg);
    if (load_cards(1)) {
        if (is_code9(arg) && (!strcmp(arg, SPECIAL_CODE) || strcmp(kind_of_code(arg), "?"))) {
            fprintf(stderr, "hero-nfc: no card list: %s not checked\n", arg);
            strcpy(code, arg);
            return 0;
        }
        if (opt.force) {
            strcpy(code, arg);
            return 0;
        }
        return err("%s: not a card code (without the card list only codes 0102SSKNN; --force writes it anyway)",
                   arg);
    }
    if (!strcmp(arg, SPECIAL_CODE)) {
        strcpy(code, arg);
        return 0;
    }
    if ((hit = card_by_code(arg)))      /* a code decides (boss rows share theirs) */
        hits = 1;
    for (i = 0; i < ncards && !card_by_code(arg); i++) {
        const struct card *c = &cards[i];

        if (!strcasecmp(c->printed, arg) || !strcasecmp(c->soft, arg) ||
            !strcasecmp(c->name, arg)) {
            if (!hits++)
                hit = c;
            else if (hits == 2)
                printf("%-9s  %-5s  %-7s  %s\n", hit->code, hit->kind, hit->printed, hit->name);
            if (hits > 1)
                printf("%-9s  %-5s  %-7s  %s\n", c->code, c->kind, c->printed, c->name);
        }
    }
    if (hits > 1)
        return err("%s matches %d cards: give the code", arg, hits);
    if (!hit) {
        if (opt.force) {
            fprintf(stderr, "hero-nfc: %s is not in the card list (--force)\n", arg);
            strcpy(code, arg);
            return 0;
        }
        return err("%s: no such card (hero-nfc list shows them; --force writes it anyway)", arg);
    }
    if (hit->boss && !opt.force)
        return err("%s (%s) is a boss card without a numeric code: the game cannot scan it (--force writes it anyway)",
                   hit->code, hit->name);
    strcpy(code, hit->code);
    return 0;
}

/* ---- the reader and the tag ------------------------------------------------------- */

static const struct reader *rd;

struct snap {
    uint8_t uid[10];
    int uid_len;
    uint8_t hdr[16];                    /* pages 0..3 */
    int cc_ok, user_pages, last_page;   /* last_page: the last writable user page */
    int state;
    struct hero_nfc_tag t;
    uint32_t count;
    char uidhex[24];
};

static int open_reader(void)
{
    const char *fake = opt.fake ? opt.fake : getenv("IGS_NFC_FAKE");

    if (opt.reader && !*opt.reader)
        return err("--reader needs part of a reader name");
    nfc_reader_want = opt.reader;
    if (fake && *fake) {
        if (!dir_exists(fake))
            return err("fake reader: %s is not a folder", fake);
        nfc_fake_dir = fake;
        rd = &fake_reader;
    } else {
        rd = &pcsc_reader;
    }
    if (rd->open())
        { err("no PC/SC (%s not found)", PCSC_LIB); return NOTAG; }
    return OK;
}

static const char *reader_desc(void)
{
    static char d[1100];

    if (rd == &fake_reader)
        snprintf(d, sizeof(d), "fake (%s)", fake_path);
    else
        snprintf(d, sizeof(d), "PC/SC \"%s\"", pc.reader);
    return d;
}

/* the tag on the reader (waits with --wait) */
static int get_tag(uint8_t *uid, int *uid_len)
{
    int said = 0;

    for (;;) {
        if (rd->poll(uid, uid_len))
            return OK;
        if (!opt.wait) {
            if (rd == &pcsc_reader && !pc.have_ctx) {
                err("PC/SC unavailable (no smart-card service?)");
                return NOTAG;
            }
            if (rd == &pcsc_reader && !pc.reader[0]) {
                err("no PC/SC reader matching \"%s\" (hero-nfc readers lists them)", pcsc_want());
                return NOTAG;
            }
            err("no tag on the reader (%s); --wait waits for one", reader_desc());
            return NOTAG;
        }
        if (!said++)
            fprintf(stderr, "hero-nfc: waiting for %s (Ctrl+C stops)\n",
                    rd == &pcsc_reader && !pc.reader[0] ? "the reader" : "a tag");
        if (said == 2 && rd == &pcsc_reader && pc.reader[0])
            fprintf(stderr, "hero-nfc: reader %s; place a tag\n", reader_desc());
        sleep_ms(250);
    }
}

static int snapshot(struct snap *s)
{
    int r;

    memset(s, 0, sizeof(*s));
    if ((r = get_tag(s->uid, &s->uid_len)))
        return r;
    hex(s->uidhex, s->uid, s->uid_len);
    if (rd->read16(0, s->hdr))
        return err("tag %s: reading pages 0..3 failed", s->uidhex);
    s->state = tag_examine(rd, &s->t, &s->count, &s->user_pages);
    s->user_pages = s->hdr[12] == 0xE1 ? s->hdr[14] * 2 : 0;
    s->cc_ok = s->user_pages >= AREA / 4;             /* NDEF; read-only when hdr[15] != 0 */
    /* NTAG21x: the CC's size is the NDEF area, a little short of user memory */
    s->last_page = !s->cc_ok ? LAST_USER_PAGE : s->hdr[14] == 0x12 ? 39 : s->hdr[14] == 0x3E ? 129 :
                   3 + s->user_pages;
    if (s->last_page > LAST_USER_PAGE)
        s->last_page = LAST_USER_PAGE;
    return OK;
}

static const char *tag_type(const struct snap *s, char *b, size_t n)
{
    const uint8_t *cc = s->hdr + 12;

    if (!s->cc_ok && !cc[0] && !cc[1] && !cc[2] && !cc[3])
        snprintf(b, n, "Type 2 tag without an NDEF capability container (hero-nfc format adds one)");
    else if (cc[0] != 0xE1)
        snprintf(b, n, "Type 2 tag, not NDEF (CC %02X %02X %02X %02X)", cc[0], cc[1], cc[2], cc[3]);
    else
        snprintf(b, n, "%s, %d-byte NDEF area (CC %02X %02X %02X %02X%s)",
                 cc[2] == 0x12 ? "NTAG213" : cc[2] == 0x3E ? "NTAG215" : cc[2] == 0x6D ? "NTAG216" :
                 "Type 2 tag", cc[2] * 8, cc[0], cc[1], cc[2], cc[3], cc[3] ? ", read-only" : "");
    return b;
}

/* the tag's whole image, as far as it reads (dump) */
static int tag_pages(const struct snap *s)
{
    switch (s->cc_ok ? s->hdr[14] : 0x3E) {
    case 0x12: return 45;
    case 0x3E: return 135;
    case 0x6D: return 231;
    }
    return 4 + s->user_pages;
}

/* pages first..last of img into the tag where they differ, each read back; -1 failed */
static int apply(const uint8_t *img, int first, int last, int *written)
{
    uint8_t cur[16];
    int p;

    *written = 0;
    if (first < FIRST_PAGE || last > LAST_USER_PAGE)
        return err("refusing to write pages %d..%d (user memory is 4..%d)", first, last, LAST_USER_PAGE), -1;
    for (p = first; p <= last; p++) {
        if (rd->read16(p, cur))
            return err("reading page %d failed", p), -1;
        if (!memcmp(cur, img + 4 * p, 4))
            continue;
        if (rd->write4(p, img + 4 * p))
            return err("writing page %d failed (after %d pages)", p, *written), -1;
        if (rd->read16(p, cur) || memcmp(cur, img + 4 * p, 4))
            return err("page %d did not read back as written", p), -1;
        ++*written;
    }
    return 0;
}

static int read_range(uint8_t *img, int first, int last)
{
    uint8_t b[16];
    int p;

    for (p = first; p <= last; p += 4) {
        int at = p + 3 > last ? last - 3 : p;

        if (at < 0 || rd->read16(at, b))
            return -1;
        memcpy(img + 4 * at, b, 16);
    }
    return 0;
}

static void print_content(const struct snap *s)
{
    char d[200];

    switch (s->state) {
    case TAG_BLANK:
        puts("content  blank");
        break;
    case TAG_FOREIGN:
        puts(s->cc_ok && s->hdr[15] ? "content  read-only (the game leaves it alone)" :
             s->cc_ok ? "content  not a loader tag (the game leaves it alone)"
                      : "content  no NDEF data (the game leaves it alone)");
        break;
    case TAG_LOADER:
        if (s->t.code[0]) {
            describe_code(d, sizeof(d), s->t.code);
            printf("content  loader tag, card %s\n", d);
        } else {
            puts(s->count ? "content  loader tag, no card" : "content  loader tag, record damaged (reads as no card, no points)");
        }
        if (s->t.points >= 0)
            printf("points   %d\n", s->t.points);
        else
            puts("points   none");
        printf("writes   %u\n", (unsigned)s->count);
        break;
    }
}

static int cmd_info(void)
{
    struct snap s;
    char b[160];
    uint8_t dyn[16];
    int r;

    if ((r = snapshot(&s)))
        return r;
    printf("reader   %s\n", reader_desc());
    printf("uid      %s\n", s.uidhex);
    printf("type     %s\n", tag_type(&s, b, sizeof(b)));
    if (s.hdr[10] || s.hdr[11])
        printf("locks    static lock bytes %02X %02X: some pages are read-only\n", s.hdr[10], s.hdr[11]);
    else if (s.cc_ok && s.hdr[14] == 0x3E && !rd->read16(130, dyn) && (dyn[0] || dyn[1] || dyn[2]))
        printf("locks    dynamic lock bytes %02X %02X %02X: some pages are read-only\n", dyn[0], dyn[1], dyn[2]);
    else
        puts("locks    none");
    print_content(&s);
    return OK;
}

static int require_ndef(const struct snap *s)
{
    if (!s->cc_ok)
        return err("tag %s has no NDEF capability container: hero-nfc format first", s->uidhex);
    if (s->hdr[15])
        return err("tag %s is read-only (CC access byte %02X)", s->uidhex, s->hdr[15]);
    return OK;
}

/* write a record and show the result */
static int commit_record(struct snap *s, const char *code, int points)
{
    struct snap after;
    int r;

    if (tag_commit(rd, s->user_pages, code, points, s->count + 1))
        return err("writing the tag failed (or it did not read back): the record may be damaged; try again");
    if ((r = snapshot(&after)))
        return r;
    if (after.state != TAG_LOADER || strcmp(after.t.code, code) || after.t.points != points)
        return err("the tag reads back differently");
    print_content(&after);
    return OK;
}

static int cmd_write(const char *arg)
{
    struct snap s;
    char code[13], d[200], q[400];
    int r, points;

    if ((r = resolve_card(arg, code)))
        return r;
    if ((r = snapshot(&s)) || (r = require_ndef(&s)))
        return r;
    if (s.state == TAG_FOREIGN)
        return err("tag %s holds other data: hero-nfc erase --force clears it first", s.uidhex);
    points = s.state == TAG_LOADER ? s.t.points : -1;
    describe_code(d, sizeof(d), code);
    if (s.state == TAG_LOADER && s.t.code[0])
        snprintf(q, sizeof(q), "Tag %s: replace card %s with %s", s.uidhex, s.t.code, d);
    else
        snprintf(q, sizeof(q), "Tag %s (%s): write card %s", s.uidhex,
                 s.state == TAG_BLANK ? "blank" : "loader tag, no card", d);
    if (points >= 0)
        snprintf(q + strlen(q), sizeof(q) - strlen(q), ", keeping %d points", points);
    if (!confirm(q))
        return FAIL;
    return commit_record(&s, code, points);
}

static int cmd_points(const char *arg)
{
    struct snap s;
    char q[200], *end;
    long v;
    int r, rel = arg[0] == '+' || arg[0] == '-';

    v = strtol(arg, &end, 10);
    if (*end || end == arg)
        { err("points: %s is not a number (n, +n or -n)", arg); return USAGE; }
    if ((r = snapshot(&s)) || (r = require_ndef(&s)))
        return r;
    if (s.state == TAG_FOREIGN)
        return err("tag %s holds other data: hero-nfc erase --force clears it first", s.uidhex);
    if (rel) {
        if (s.state != TAG_LOADER || s.t.points < 0)
            return err("tag %s has no points yet: set them with hero-nfc points <n>", s.uidhex);
        v += s.t.points;
    }
    if (v < 0 || v > 0xFFFF)
        return err("points must end up in 0..65535 (got %ld)", v);
    if (s.state == TAG_LOADER && s.t.points >= 0)
        snprintf(q, sizeof(q), "Tag %s: points %d -> %ld", s.uidhex, s.t.points, v);
    else
        snprintf(q, sizeof(q), "Tag %s (%s): set points to %ld", s.uidhex,
                 s.state == TAG_BLANK ? "blank" : "loader tag", v);
    if (!confirm(q))
        return FAIL;
    return commit_record(&s, s.state == TAG_LOADER ? s.t.code : "", (int)v);
}

static int cmd_erase(void)
{
    static uint8_t img[1024];
    struct snap s;
    char q[200];
    int r, last, n;

    if ((r = snapshot(&s)))
        return r;
    if (s.state == TAG_BLANK) {
        printf("tag %s is already blank\n", s.uidhex);
        return OK;
    }
    if (s.cc_ok && s.hdr[15])
        return err("tag %s is read-only (CC access byte %02X)", s.uidhex, s.hdr[15]);
    if (s.state == TAG_FOREIGN && !opt.force)
        return err("tag %s is not a loader tag: --force erases it anyway (user pages 4..%d)", s.uidhex,
                   s.last_page);
    /* the loader's record lies in the first 64 user bytes; anything else: all user memory */
    last = s.state == TAG_LOADER ? FIRST_PAGE + AREA / 4 - 1 : s.last_page;
    if (last > s.last_page)
        last = s.last_page;
    memset(img, 0, sizeof(img));
    img[16] = 0x03;                     /* an empty NDEF message, then the terminator */
    img[18] = 0xFE;
    snprintf(q, sizeof(q), "Tag %s: erase user pages 4..%d (%s) to an empty NDEF message", s.uidhex, last,
             s.state == TAG_LOADER ? "the loader's record" : "NOT a loader tag");
    if (!confirm(q))
        return FAIL;
    if (apply(img, FIRST_PAGE, last, &n))
        return FAIL;
    if ((r = snapshot(&s)))
        return r;
    printf("%d pages written\n", n);
    print_content(&s);
    return s.state == TAG_BLANK ? OK : err("the tag does not read as blank");
}

/* NDEF capability container (one-time programmable) for a tag that has none, and an empty message */
static int cmd_format(void)
{
    static uint8_t img[1024];
    struct snap s;
    uint8_t b[16], *cc = s.hdr + 12, size;
    char q[300];
    int r, n;

    if ((r = snapshot(&s)))
        return r;
    if (s.cc_ok) {
        if (s.hdr[15])
            return err("tag %s is read-only (CC access byte %02X)", s.uidhex, s.hdr[15]);
        if (s.state != TAG_BLANK)
            return err("tag %s is already formatted and holds data (hero-nfc erase clears it)", s.uidhex);
        if (rd->read16(FIRST_PAGE, b))
            return err("reading page 4 failed");
        if (b[0]) {                     /* a TLV already (empty NDEF, terminator, lock control) */
            printf("tag %s is already formatted (empty NDEF)\n", s.uidhex);
            return OK;
        }
        size = 0;
    } else if (!cc[0] && !cc[1] && !cc[2] && !cc[3]) {
        /* the size, by which pages exist: NTAG216 231, NTAG215 135, NTAG213 45 */
        size = !rd->read16(227, b) ? 0x6D : !rd->read16(131, b) ? 0x3E : !rd->read16(41, b) ? 0x12 : 0;
        if (!size)
            return err("tag %s: unknown size (not an NTAG213/215/216): not formatted", s.uidhex);
    } else {
        return err("tag %s has a non-NDEF capability container (%02X %02X %02X %02X); it is one-time programmable: cannot format",
                   s.uidhex, cc[0], cc[1], cc[2], cc[3]);
    }
    if (size)
        snprintf(q, sizeof(q), "Tag %s: write the capability container E1 10 %02X 00 (%s; PERMANENT, one-time "
                 "programmable) and an empty NDEF message", s.uidhex, size,
                 size == 0x6D ? "NTAG216" : size == 0x3E ? "NTAG215" : "NTAG213");
    else
        snprintf(q, sizeof(q), "Tag %s: write an empty NDEF message to page 4", s.uidhex);
    if (!confirm(q))
        return FAIL;
    if (size) {
        uint8_t w[4] = { 0xE1, 0x10, size, 0x00 };

        if (rd->write4(3, w) || rd->read16(0, b) || memcmp(b + 12, w, 4))
            return err("writing the capability container failed");
    }
    memset(img, 0, sizeof(img));
    if (rd->read16(FIRST_PAGE, img + 16))
        return err("reading page 4 failed");
    img[16] = 0x03;
    img[17] = 0x00;
    img[18] = 0xFE;
    img[19] = 0x00;
    if (apply(img, FIRST_PAGE, FIRST_PAGE, &n))
        return FAIL;
    if ((r = snapshot(&s)))
        return r;
    print_content(&s);
    return s.state == TAG_BLANK ? OK : err("the tag does not read as blank");
}

static int cmd_dump(const char *file)
{
    static uint8_t img[1024];
    struct snap s;
    char def[64];
    int r, pages, p, bad = 0;
    FILE *f;

    if ((r = snapshot(&s)))
        return r;
    if (!file) {
        snprintf(def, sizeof(def), "tag-%s.bin", s.uidhex);
        file = def;
    }
    if (file_exists(file) && !opt.yes && !opt.force)
        return err("%s exists (--yes overwrites it)", file);
    pages = tag_pages(&s);
    memset(img, 0, sizeof(img));
    for (p = 0; p < pages; p += 4) {
        int at = p + 4 > pages ? pages - 4 : p;
        uint8_t b[16];

        if (rd->read16(at, b))
            bad++;
        else
            memcpy(img + 4 * at, b, 16);
    }
    if (!(f = fopen(file, "wb")) || fwrite(img, 4, pages, f) != (size_t)pages) {
        if (f)
            fclose(f);
        return err("cannot write %s: %s", file, strerror(errno));
    }
    if (fclose(f))
        return err("cannot write %s", file);
    printf("tag %s: %d pages (%d bytes) to %s%s\n", s.uidhex, pages, pages * 4, file,
           bad ? " (some blocks did not read: zeros)" : "");
    if (rd == &pcsc_reader)
        puts("(the password pages read as zeros on a real tag)");
    return OK;
}

static int cmd_restore(const char *file)
{
    static uint8_t img[1024], cur[1024];
    struct snap s, b;
    uint8_t fuid[7];
    long size;
    int r, last, n, p, diff = 0;
    char q[300], d[200];
    FILE *f;

    if (!(f = fopen(file, "rb")))
        return err("cannot open %s: %s", file, strerror(errno));
    size = (long)fread(img, 1, sizeof(img), f);
    fclose(f);
    if (size < 4 * (FIRST_PAGE + 1) || size % 4)
        return err("%s is not a tag dump (%ld bytes)", file, size);
    if ((r = snapshot(&s)))
        return r;
    if (s.cc_ok && s.hdr[15])
        return err("tag %s is read-only (CC access byte %02X)", s.uidhex, s.hdr[15]);
    memcpy(fuid, img, 3);
    memcpy(fuid + 3, img + 4, 4);
    if (s.uid_len != 7 || memcmp(fuid, s.uid, 7)) {
        char fh[16];

        hex(fh, fuid, 7);
        if (!opt.force)
            return err("%s is from tag %s, not %s (--force restores it anyway)", file, fh, s.uidhex);
        fprintf(stderr, "hero-nfc: %s is from tag %s (--force)\n", file, fh);
    }
    last = size / 4 - 1;
    if (last > s.last_page)
        last = s.last_page;
    /* what the dump holds, as the game would read it */
    memset(&b, 0, sizeof(b));
    b.state = parse_area(img + 16, &b.t, &b.count);
    if (b.state == TAG_LOADER && b.t.code[0])
        describe_code(d, sizeof(d), b.t.code);
    else
        snprintf(d, sizeof(d), "%s", b.state == TAG_BLANK ? "blank" : b.state == TAG_LOADER ? "loader tag, no card" :
                 "not a loader tag");
    if (read_range(cur, FIRST_PAGE, last))
        return err("reading the tag failed");
    for (p = FIRST_PAGE; p <= last; p++)
        diff += !!memcmp(cur + 4 * p, img + 4 * p, 4);
    if (!diff) {
        printf("tag %s already matches %s (pages 4..%d)\n", s.uidhex, file, last);
        return OK;
    }
    snprintf(q, sizeof(q), "Tag %s: restore user pages 4..%d from %s (%d pages differ; the dump holds: %s)",
             s.uidhex, last, file, diff, d);
    if (!confirm(q))
        return FAIL;
    if (apply(img, FIRST_PAGE, last, &n))
        return FAIL;
    printf("%d pages written\n", n);
    if ((r = snapshot(&s)))
        return r;
    print_content(&s);
    return OK;
}

/* a new tag.bin in the fake reader's folder: an NTAG215 as shipped */
static int cmd_fake_new(void)
{
    static const uint8_t tail[20] = { 0x00, 0x00, 0x00, 0xBD, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x05, 0x00, 0x00,
                                      0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 };
    uint8_t img[TAG_BYTES] = { 0 }, *u = img;
    uint32_t x = (uint32_t)(now_s() * 1000003);
    const char *fake = opt.fake ? opt.fake : getenv("IGS_NFC_FAKE");
    char path[1100], h[16];
    FILE *f;
    int i;

    if (!fake || !*fake)
        { err("fake-new needs --fake <dir> (or IGS_NFC_FAKE)"); return USAGE; }
    if (!dir_exists(fake))
        return err("%s is not a folder", fake);
    snprintf(path, sizeof(path), "%s/tag.bin", fake);
    if (file_exists(path) && !opt.force)
        return err("%s exists: a tag is on the fake reader (--force replaces it)", path);
    /* UID 04 xx xx BCC0 xx xx xx xx BCC1 */
    u[0] = 0x04;
    for (i = 1; i < 8; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        if (i != 3)
            u[i] = x >> 8;
    }
    u[3] = 0x88 ^ u[0] ^ u[1] ^ u[2];
    u[8] = u[4] ^ u[5] ^ u[6] ^ u[7];
    u[9] = 0x48;
    if (!opt.unformatted) {
        memcpy(u + 12, "\xE1\x10\x3E\x00", 4);
        memcpy(u + 16, "\x03\x00\xFE\x00", 4);
    }
    memcpy(u + 130 * 4, tail, sizeof(tail));
    if (!(f = fopen(path, "wb")) || fwrite(img, 1, TAG_BYTES, f) != TAG_BYTES) {
        if (f)
            fclose(f);
        return err("cannot write %s", path);
    }
    fclose(f);
    memmove(img + 3, img + 4, 4);
    hex(h, img, 7);
    printf("new %s NTAG215 %s in %s\n", opt.unformatted ? "unformatted" : "blank", h, path);
    return OK;
}

static int cmd_readers(void)
{
    char names[4096], *n;
    int any = 0;

    rd = &pcsc_reader;
    nfc_reader_want = opt.reader;
    if (pcsc_open())
        { err("no PC/SC (%s not found)", PCSC_LIB); return NOTAG; }
    if (pcsc_names(names, sizeof(names)))
        { err("PC/SC unavailable (no smart-card service?)"); return NOTAG; }
    for (n = names; *n; n += strlen(n) + 1) {
        int m = ci_contains(n, pcsc_want());

        printf("%s%s\n", n, m && !any ? "   <- used" : "");
        any |= m;
    }
    if (!any)
        printf("(none matches \"%s\")\n", pcsc_want());
    return any ? OK : NOTAG;
}

static void usage(void)
{
    puts("hero-nfc: Hero of Robots NFC cards (NTAG215, loader format; experimental)\n"
         "\n"
         "  list [filter]       the cards in QRCardRes.csv (code, kind, printed id, name)\n"
         "  info                the tag on the reader\n"
         "  write <card>        put a card on a blank or loader tag (code, printed id or name)\n"
         "  points <n|+n|-n>    set or change the tag's points\n"
         "  erase               a loader tag back to blank (--force: any tag, user pages only)\n"
         "  format              NDEF capability container and empty message for an unformatted tag\n"
         "  dump [file]         save the tag's pages (default tag-<UID>.bin)\n"
         "  restore <file>      write a dump's user pages back (same tag; --force: any)\n"
         "  readers             the PC/SC readers (none is opened)\n"
         "  fake-new            a new blank tag.bin for the fake reader (--unformatted: no CC)\n"
         "\n"
         "  --reader <name>     PC/SC reader, part of its name (default ACR122; HERO_NFC_READER)\n"
         "  --fake <dir>        the fake reader: <dir>/tag.bin (IGS_NFC_FAKE)\n"
         "  --wait              wait for the reader and a tag\n"
         "  --yes, -y           do not ask before writing\n"
         "  --force             write boss / unknown cards, erase other tags, restore another tag's dump\n"
         "  --csv <file>        the card list; --set <dir> a rom set folder; --game <id> (default "
         DEFAULT_GAME ")\n"
         "  --root <dir>        the loader folder (igs-loader.ini, rompath)\n"
         "  -v                  log the reader's messages\n"
         "\n"
         "exit: 0 ok, 1 failed or declined, 2 usage, 3 no reader or no tag");
}

#ifdef _WIN32
/* the arguments as UTF-8 (card names) */
static char **utf8_argv(int *argc, char **argv)
{
    wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), argc);
    int i;

    if (!wv)
        return argv;
    argv = calloc(*argc + 1, sizeof(*argv));
    for (i = 0; i < *argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, NULL, 0, NULL, NULL);

        argv[i] = malloc(n);
        WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, argv[i], n, NULL, NULL);
    }
    return argv;
}
#endif

/* options into opt, the rest into pos[0..2]; -1 to go on, else the exit code */
static int parse_args(int argc, char **argv, const char **pos, int *npos)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char **val = !strcmp(a, "--reader") ? &opt.reader : !strcmp(a, "--fake") ? &opt.fake :
                           !strcmp(a, "--csv") ? &opt.csv : !strcmp(a, "--set") ? &opt.set :
                           !strcmp(a, "--game") ? &opt.game : !strcmp(a, "--root") ? &opt.root : NULL;

        if (val) {
            if (i + 1 >= argc) {
                err("%s needs a value", a);
                return USAGE;
            }
            *val = argv[++i];
        } else if (!strcmp(a, "--wait")) {
            opt.wait = 1;
        } else if (!strcmp(a, "--yes") || !strcmp(a, "-y")) {
            opt.yes = 1;
        } else if (!strcmp(a, "--force")) {
            opt.force = 1;
        } else if (!strcmp(a, "--unformatted")) {
            opt.unformatted = 1;
        } else if (!strcmp(a, "-v")) {
            verbose = 1;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return OK;
        } else if (a[0] == '-' && !(a[1] >= '0' && a[1] <= '9')) {
            err("unknown option %s (--help)", a);
            return USAGE;
        } else if (*npos < 3) {
            pos[(*npos)++] = a;
        } else {
            err("too many arguments (--help)");
            return USAGE;
        }
    }
    return -1;
}

#ifndef HERO_NFC_NO_MAIN
int main(int argc, char **argv)
{
    const char *pos[3] = { 0 };
    int npos = 0, r;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    argv = utf8_argv(&argc, argv);
#endif
    if ((r = parse_args(argc, argv, pos, &npos)) >= 0)
        return r;
    if (!pos[0]) {
        usage();
        return USAGE;
    }
    if (!strcmp(pos[0], "list") && npos <= 2)
        return cmd_list(pos[1]);
    if (!strcmp(pos[0], "readers") && npos == 1)
        return cmd_readers();
    if (!strcmp(pos[0], "fake-new") && npos == 1)
        return cmd_fake_new();
    if (strcmp(pos[0], "info") && strcmp(pos[0], "write") && strcmp(pos[0], "points") &&
        strcmp(pos[0], "erase") && strcmp(pos[0], "format") && strcmp(pos[0], "dump") &&
        strcmp(pos[0], "restore")) {
        if (!strcmp(pos[0], "list") || !strcmp(pos[0], "readers") || !strcmp(pos[0], "fake-new"))
            fprintf(stderr, "hero-nfc: %s: wrong arguments (--help)\n", pos[0]);
        else
            fprintf(stderr, "hero-nfc: unknown command %s (--help)\n", pos[0]);
        return USAGE;
    }
    if ((!strcmp(pos[0], "dump") && npos > 2) || (!strcmp(pos[0], "info") && npos != 1) ||
        (!strcmp(pos[0], "erase") && npos != 1) || (!strcmp(pos[0], "format") && npos != 1) ||
        ((!strcmp(pos[0], "write") || !strcmp(pos[0], "points") || !strcmp(pos[0], "restore")) && npos != 2)) {
        fprintf(stderr, "hero-nfc: %s: wrong arguments (--help)\n", pos[0]);
        return USAGE;
    }
    if ((r = open_reader()))
        return r;
    if (!strcmp(pos[0], "info"))
        r = cmd_info();
    else if (!strcmp(pos[0], "write"))
        r = cmd_write(pos[1]);
    else if (!strcmp(pos[0], "points"))
        r = cmd_points(pos[1]);
    else if (!strcmp(pos[0], "erase"))
        r = cmd_erase();
    else if (!strcmp(pos[0], "format"))
        r = cmd_format();
    else if (!strcmp(pos[0], "dump"))
        r = cmd_dump(pos[1]);
    else
        r = cmd_restore(pos[1]);
    if (rd == &pcsc_reader)
        pcsc_drop();
    return r;
}
#endif
