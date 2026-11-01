/*
 * hero-nfc-gui: the Hero of Robots NFC card tool in a window (Windows only;
 * the CLI, hero-nfc.c, is the cross-platform path).  Experimental.
 *
 * Browses the card list with the cards' artwork, shows the tag on the reader
 * live, and runs the CLI's commands on it (write, points, erase, format, dump,
 * restore), each confirmed in a dialog.  Options as the CLI's (--fake,
 * --reader, --csv, --set, --game, --root, --yes, --force), plus --shot <bmp>:
 * render the window into a .bmp and exit (tests).
 *
 * Artwork: media/localization/ui/common/card/<printed id>_I1.dds next to the
 * card list, DXT5 (DXT1 also read), 256x360, not encrypted.
 */
#define HERO_NFC_NO_MAIN
#include "hero-nfc.c"

#include <commctrl.h>
#include <commdlg.h>

enum {
    ID_SEARCH = 100, ID_LIST, ID_WRITE, ID_FORCE, ID_POINTS, ID_SETPOINTS, ID_ERASE, ID_FORMAT, ID_DUMP, ID_RESTORE,
    ID_STATUS, ID_TIMER, ID_SHOT
};

/* layout (client pixels) */
#define LIST_X 8
#define LIST_W 420
#define CARD_X 440
#define CARD_Y 8
#define CARD_W 256
#define CARD_H 360
#define TAG_X 712
#define TAG_IMG_Y 34
#define TAG_IMG_W 192
#define TAG_IMG_H 270
#define WIN_W 1050
#define WIN_H 640

struct image { int w, h; uint32_t *px; };

static HWND hwnd, h_search, h_list, h_card_text, h_write, h_force, h_tag_text, h_points, h_status, h_reader;
static struct image card_img, tag_img;
static int sel = -1;                    /* the selected card (index in cards) */
static int reader_ok, busy;
static char tag_code[13] = "\1", tag_text[1024], tag_uid[24], errors[2048];
static const char *shot_file;

/* ---- text ------------------------------------------------------------------------ */

static wchar_t *wide(const char *s)
{
    static wchar_t buf[4][2048];
    static int k;
    wchar_t *w = buf[k++ & 3];

    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 2048);
    return w;
}

static void set_text(HWND h, const char *s)
{
    SetWindowTextW(h, wide(s));
}

static void get_text(HWND h, char *out, int n)
{
    wchar_t w[512];

    GetWindowTextW(h, w, 512);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
}

static void status(const char *s)
{
    SendMessageW(h_status, SB_SETTEXTW, 0, (LPARAM)wide(s));
}

static void on_err(const char *msg)
{
    size_t n = strlen(errors);

    snprintf(errors + n, sizeof(errors) - n, "%s%s", n ? "\n" : "", msg);
}

static int on_confirm(const char *what)
{
    return MessageBoxW(hwnd, wide(what), L"hero-nfc: confirm",
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

/* ---- artwork (DDS, DXT1 / DXT5), composited on the window colour -------------- */

static void rgb565(uint16_t c, int *o)
{
    o[0] = (c >> 11 & 31) * 255 / 31;
    o[1] = (c >> 5 & 63) * 255 / 63;
    o[2] = (c & 31) * 255 / 31;
}

static void decode_block(const uint8_t *s, int dxt5, uint32_t bg, uint32_t *out, int stride)
{
    int a[8], c[4][3], p, k;
    uint64_t abits = 0;
    uint32_t idx;
    uint16_t c0, c1;

    if (dxt5) {
        a[0] = s[0];
        a[1] = s[1];
        for (k = 2; k < 8; k++)
            a[k] = a[0] > a[1] ? ((8 - k) * a[0] + (k - 1) * a[1]) / 7 :
                   k < 6 ? ((6 - k) * a[0] + (k - 1) * a[1]) / 5 : k == 6 ? 0 : 255;
        for (k = 0; k < 6; k++)
            abits |= (uint64_t)s[2 + k] << (8 * k);
        s += 8;
    }
    c0 = s[0] | s[1] << 8;
    c1 = s[2] | s[3] << 8;
    rgb565(c0, c[0]);
    rgb565(c1, c[1]);
    for (k = 0; k < 3; k++)
        if (dxt5 || c0 > c1) {
            c[2][k] = (2 * c[0][k] + c[1][k]) / 3;
            c[3][k] = (c[0][k] + 2 * c[1][k]) / 3;
        } else {
            c[2][k] = (c[0][k] + c[1][k]) / 2;
            c[3][k] = -1;               /* DXT1 transparent */
        }
    idx = s[4] | s[5] << 8 | s[6] << 16 | (uint32_t)s[7] << 24;
    for (p = 0; p < 16; p++) {
        int ci = idx >> (2 * p) & 3, al = dxt5 ? a[abits >> (3 * p) & 7] : c[ci][0] < 0 ? 0 : 255, rgb[3];

        for (k = 0; k < 3; k++) {
            int b = bg >> (16 - 8 * k) & 255, v = c[ci][k] < 0 ? 0 : c[ci][k];

            rgb[k] = (v * al + b * (255 - al)) / 255;
        }
        out[(p / 4) * stride + p % 4] = rgb[0] << 16 | rgb[1] << 8 | rgb[2];
    }
}

static int load_dds(const char *path, struct image *im)
{
    static uint8_t buf[2 << 20];
    COLORREF face = GetSysColor(COLOR_BTNFACE);
    uint32_t bg = GetRValue(face) << 16 | GetGValue(face) << 8 | GetBValue(face);
    FILE *f = fopen(path, "rb");
    size_t n;
    int w, h, dxt5, bx, by, bs;

    free(im->px);
    memset(im, 0, sizeof(*im));
    if (!f)
        return -1;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 128 || memcmp(buf, "DDS ", 4) || (memcmp(buf + 84, "DXT5", 4) && memcmp(buf + 84, "DXT1", 4)))
        return -1;
    memcpy(&h, buf + 12, 4);
    memcpy(&w, buf + 16, 4);
    dxt5 = !memcmp(buf + 84, "DXT5", 4);
    bs = dxt5 ? 16 : 8;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || w % 4 || h % 4 ||
        128 + (size_t)(w / 4) * (h / 4) * bs > n)
        return -1;
    im->px = malloc((size_t)w * h * 4);
    for (by = 0; by < h / 4; by++)
        for (bx = 0; bx < w / 4; bx++)
            decode_block(buf + 128 + ((size_t)by * (w / 4) + bx) * bs, dxt5, bg,
                         im->px + (size_t)by * 4 * w + bx * 4, w);
    im->w = w;
    im->h = h;
    return 0;
}

/* the artwork of a card: <card list dir>/../ui/common/card/<printed>_I1.dds */
static int load_card_image(const struct card *c, struct image *im)
{
    char path[RS_PATH + 64], *s;

    snprintf(path, sizeof(path), "%s", csv_path);
    if ((s = strrchr(path, '/')) || (s = strrchr(path, '\\')))
        *s = 0;
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/../ui/common/card/%s_I1.dds", c->printed);
    return c->printed[0] ? load_dds(path, im) : (load_dds("", im), -1);
}

static void draw_image(HDC dc, const struct image *im, int x, int y, int w, int h, const char *none)
{
    RECT r = { x, y, x + w, y + h };

    if (!im->px) {
        FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
        FrameRect(dc, &r, GetSysColorBrush(COLOR_BTNSHADOW));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, wide(none), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    {
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), im->w, -im->h, 1, 32, BI_RGB } };

        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, x, y, w, h, 0, 0, im->w, im->h, im->px, &bi, DIB_RGB_COLORS, SRCCOPY);
    }
}

/* ---- the card list ---------------------------------------------------------------- */

static void fill_list(void)
{
    char filter[256];
    int i, row = 0;

    get_text(h_search, filter, sizeof(filter));
    SendMessageW(h_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(h_list);
    for (i = 0; i < ncards; i++) {
        const struct card *c = &cards[i];
        LVITEMW it = { LVIF_TEXT | LVIF_PARAM };
        char name[128];

        if (filter[0] && !ci_contains(c->code, filter) && !ci_contains(c->kind, filter) &&
            !ci_contains(c->printed, filter) && !ci_contains(c->soft, filter) &&
            !ci_contains(c->name, filter) && !(c->boss && !strcasecmp(filter, "boss")))
            continue;
        it.iItem = row;
        it.pszText = wide(c->code);
        it.lParam = i;
        SendMessageW(h_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.mask = LVIF_TEXT;
        it.iSubItem = 1;
        it.pszText = wide(c->kind);
        SendMessageW(h_list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2;
        it.pszText = wide(c->printed);
        SendMessageW(h_list, LVM_SETITEMW, 0, (LPARAM)&it);
        snprintf(name, sizeof(name), "%s%s", c->name, c->boss ? "  [boss]" : "");
        it.iSubItem = 3;
        it.pszText = wide(name);
        SendMessageW(h_list, LVM_SETITEMW, 0, (LPARAM)&it);
        row++;
    }
    SendMessageW(h_list, WM_SETREDRAW, TRUE, 0);
    if (row == 1)                       /* the search found one card: select it */
        ListView_SetItemState(h_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    {
        char s[300];

        snprintf(s, sizeof(s), "%d of %d cards", row, ncards);
        status(s);
    }
}

static void select_card(int i)
{
    char s[300];
    RECT r = { CARD_X, CARD_Y, CARD_X + CARD_W, CARD_Y + CARD_H };

    sel = i;
    if (i < 0) {
        load_dds("", &card_img);
        set_text(h_card_text, "");
    } else {
        const struct card *c = &cards[i];

        load_card_image(c, &card_img);
        snprintf(s, sizeof(s), "%s\n%s  %s  %s%s", c->name, c->kind, c->code, c->printed,
                 c->boss ? "\nboss card: the game cannot scan it" : "");
        set_text(h_card_text, s);
    }
    EnableWindow(h_write, i >= 0);
    InvalidateRect(hwnd, &r, FALSE);
}

/* ---- the tag on the reader -------------------------------------------------------- */

static void refresh_tag(void)
{
    struct snap s;
    char t[1024], b[200], d[200];
    const char *code = "";
    int r;

    if (busy || !reader_ok)
        return;
    hush = 1;
    r = snapshot(&s);
    hush = 0;
    if (r) {
        snprintf(t, sizeof(t), "%s", r == NOTAG ? "no tag on the reader" : "the tag did not read");
        tag_uid[0] = 0;
    } else {
        snprintf(tag_uid, sizeof(tag_uid), "%s", s.uidhex);
        snprintf(t, sizeof(t), "UID %s\n%s\n", s.uidhex, tag_type(&s, b, sizeof(b)));
        switch (s.state) {
        case TAG_BLANK:
            strcat(t, "blank");
            break;
        case TAG_FOREIGN:
            strcat(t, s.cc_ok && s.hdr[15] ? "read-only: the game leaves it alone" :
                      "not a loader tag: the game leaves it alone");
            break;
        default:
            code = s.t.code;
            if (code[0]) {
                describe_code(d, sizeof(d), code);
                snprintf(t + strlen(t), sizeof(t) - strlen(t), "loader tag: card %s\n", d);
            } else {
                strcat(t, s.count ? "loader tag, no card\n" : "loader tag, record damaged\n");
            }
            if (s.t.points >= 0)
                snprintf(t + strlen(t), sizeof(t) - strlen(t), "points %d\n", s.t.points);
            else
                strcat(t, "no points\n");
            snprintf(t + strlen(t), sizeof(t) - strlen(t), "writes %u", (unsigned)s.count);
        }
    }
    if (strcmp(t, tag_text)) {
        snprintf(tag_text, sizeof(tag_text), "%s", t);
        set_text(h_tag_text, t);
    }
    if (strcmp(code, tag_code)) {
        const struct card *c = code[0] ? card_by_code(code) : NULL;
        RECT rc = { TAG_X, TAG_IMG_Y, TAG_X + TAG_IMG_W, TAG_IMG_Y + TAG_IMG_H };

        snprintf(tag_code, sizeof(tag_code), "%s", code);
        if (c)
            load_card_image(c, &tag_img);
        else
            load_dds("", &tag_img);
        InvalidateRect(hwnd, &rc, FALSE);
    }
}

/* run a CLI command with the dialogs; report how it went */
static void run(const char *what, int r)
{
    if (r == OK) {
        status(what);
    } else if (errors[0]) {
        status(errors);
        MessageBoxW(hwnd, wide(errors), L"hero-nfc", MB_OK | MB_ICONWARNING);
    } else {
        status(r == FAIL ? "cancelled" : "failed");
    }
}

static void command(int id)
{
    char buf[MAX_PATH] = "", title[64];
    OPENFILENAMEA of = { sizeof(of) };
    int force = opt.force;

    if (busy)
        return;
    busy = 1;
    errors[0] = 0;
    opt.force = force || SendMessageW(h_force, BM_GETCHECK, 0, 0) == BST_CHECKED;
    of.hwndOwner = hwnd;
    of.lpstrFile = buf;
    of.nMaxFile = sizeof(buf);
    of.lpstrFilter = "Tag dumps (*.bin)\0*.bin\0All files\0*.*\0";
    of.lpstrDefExt = "bin";
    switch (id) {
    case ID_WRITE:
        if (sel >= 0)
            run("card written", cmd_write(cards[sel].code));
        break;
    case ID_SETPOINTS:
        get_text(h_points, buf, sizeof(buf));
        run("points written", cmd_points(buf));
        break;
    case ID_ERASE:
        run("tag erased", cmd_erase());
        break;
    case ID_FORMAT:
        run("tag formatted", cmd_format());
        break;
    case ID_DUMP:
        snprintf(buf, sizeof(buf), "tag-%s.bin", tag_uid[0] ? tag_uid : "unknown");
        of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        snprintf(title, sizeof(title), "Dump the tag to");
        of.lpstrTitle = title;
        if (GetSaveFileNameA(&of)) {
            opt.force = 1;              /* the dialog asked about overwriting */
            run("tag dumped", cmd_dump(buf));
        }
        break;
    case ID_RESTORE:
        of.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameA(&of))
            run("tag restored", cmd_restore(buf));
        break;
    }
    opt.force = force;
    if (rd == &pcsc_reader)
        pcsc_drop();
    busy = 0;
    tag_text[0] = 0;                    /* redisplay */
    refresh_tag();
}

/* ---- the window -------------------------------------------------------------------- */

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    static HFONT font;
    HWND c;

    if (!font) {
        NONCLIENTMETRICSW m = { sizeof(m) };

        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0);
        font = CreateFontIndirectW(&m.lfMessageFont);
    }
    c = CreateWindowExW(!wcscmp(cls, WC_EDITW) || !wcscmp(cls, WC_LISTVIEWW) ? WS_EX_CLIENTEDGE : 0, cls, text,
                        WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd, (HMENU)(intptr_t)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static void create_children(void)
{
    static const struct { const wchar_t *name; int w; } cols[] = {
        { L"Code", 80 }, { L"Kind", 56 }, { L"Printed", 64 }, { L"Name", 196 } };
    int i, y;

    h_search = child(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, LIST_X, 8, LIST_W, 24, ID_SEARCH);
    SendMessageW(h_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search: code, name, printed id, kind, boss");
    h_list = child(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                   LIST_X, 38, LIST_W, WIN_H - 70, ID_LIST);
    ListView_SetExtendedListViewStyle(h_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < 4; i++) {
        LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH, 0, cols[i].w, (wchar_t *)cols[i].name };

        SendMessageW(h_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
    y = CARD_Y + CARD_H + 8;
    h_card_text = child(WC_STATICW, L"", SS_LEFT, CARD_X, y, CARD_W, 56, 0);
    h_write = child(WC_BUTTONW, L"Write this card to the tag", BS_PUSHBUTTON | WS_TABSTOP, CARD_X, y + 62,
                    CARD_W, 30, ID_WRITE);
    EnableWindow(h_write, FALSE);
    h_force = child(WC_BUTTONW, L"Force (boss cards, other tags, other dumps)", BS_AUTOCHECKBOX | WS_TABSTOP,
                    CARD_X, y + 100, CARD_W, 22, ID_FORCE);
    if (opt.force)
        SendMessageW(h_force, BM_SETCHECK, BST_CHECKED, 0);

    child(WC_BUTTONW, L"Tag on the reader", BS_GROUPBOX, TAG_X - 12, 8, WIN_W - TAG_X - 8 + 12 - 8, 520, 0);
    h_tag_text = child(WC_STATICW, L"", SS_LEFT, TAG_X, TAG_IMG_Y + TAG_IMG_H + 8, WIN_W - TAG_X - 24, 110, 0);
    y = TAG_IMG_Y + TAG_IMG_H + 124;
    h_points = child(WC_EDITW, L"100", ES_AUTOHSCROLL | WS_TABSTOP, TAG_X, y, 90, 24, ID_POINTS);
    child(WC_BUTTONW, L"Set points", BS_PUSHBUTTON | WS_TABSTOP, TAG_X + 98, y, 100, 24, ID_SETPOINTS);
    child(WC_STATICW, L"n, +n or -n", SS_LEFT, TAG_X + 206, y + 4, 100, 20, 0);
    y += 36;
    child(WC_BUTTONW, L"Erase", BS_PUSHBUTTON | WS_TABSTOP, TAG_X, y, 72, 28, ID_ERASE);
    child(WC_BUTTONW, L"Format", BS_PUSHBUTTON | WS_TABSTOP, TAG_X + 78, y, 72, 28, ID_FORMAT);
    child(WC_BUTTONW, L"Dump...", BS_PUSHBUTTON | WS_TABSTOP, TAG_X + 156, y, 72, 28, ID_DUMP);
    child(WC_BUTTONW, L"Restore...", BS_PUSHBUTTON | WS_TABSTOP, TAG_X + 234, y, 76, 28, ID_RESTORE);
    h_reader = child(WC_STATICW, L"", SS_LEFT | SS_PATHELLIPSIS, TAG_X, 540, WIN_W - TAG_X - 16, 40, 0);
    h_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                               (HMENU)ID_STATUS, NULL, NULL);
}

static void save_shot(void)
{
    RECT r;
    HDC wdc, dc;
    HBITMAP bm;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER) } };
    BITMAPFILEHEADER fh = { 0x4D42 };
    uint32_t *px;
    FILE *f;

    GetClientRect(hwnd, &r);
    wdc = GetDC(hwnd);
    dc = CreateCompatibleDC(wdc);
    bi.bmiHeader.biWidth = r.right;
    bi.bmiHeader.biHeight = -r.bottom;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bm = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    SelectObject(dc, bm);
    PrintWindow(hwnd, dc, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + r.right * r.bottom * 4;
    if ((f = fopen(shot_file, "wb"))) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
        fwrite(px, 4, (size_t)r.right * r.bottom, f);
        fclose(f);
    }
    DeleteDC(dc);
    DeleteObject(bm);
    ReleaseDC(hwnd, wdc);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEARCH && HIWORD(wp) == EN_CHANGE)
            fill_list();
        else if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) != ID_FORCE)
            command(LOWORD(wp));
        return 0;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;

        if (n->idFrom == ID_LIST && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;

            if (lv->uNewState & LVIS_SELECTED && !(lv->uOldState & LVIS_SELECTED)) {
                LVITEMW it = { LVIF_PARAM, lv->iItem };

                SendMessageW(h_list, LVM_GETITEMW, 0, (LPARAM)&it);
                select_card((int)it.lParam);
            }
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == ID_TIMER) {
            refresh_tag();
        } else if (wp == ID_SHOT) {
            KillTimer(h, ID_SHOT);
            save_shot();
            DestroyWindow(h);
        }
        return 0;
    case WM_SIZE:
        SendMessageW(h_status, WM_SIZE, 0, 0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);

        draw_image(dc, &card_img, CARD_X, CARD_Y, CARD_W, CARD_H,
                   sel < 0 ? "select a card" : "no artwork for this card");
        draw_image(dc, &tag_img, TAG_X, TAG_IMG_Y, TAG_IMG_W, TAG_IMG_H,
                   tag_code[0] ? "no artwork" : "no card on the tag");
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    WNDCLASSW wc = { 0 };
    const char *pos[3] = { 0 };
    char **argv, s[1200];
    int argc = 0, npos = 0, i, r;
    RECT wr = { 0, 0, WIN_W, WIN_H + 24 };
    MSG m;

    (void)prev;
    (void)cmdline;
    InitCommonControlsEx(&icc);
    argv = utf8_argv(&argc, NULL);
    err_hook = on_err;
    confirm_hook = on_confirm;
    for (i = 1; i < argc; i++)          /* --shot <file> is the GUI's own */
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            shot_file = argv[i + 1];
            memmove(argv + i, argv + i + 2, (argc - i - 1) * sizeof(*argv));
            argc -= 2;
            break;
        }
    if ((r = parse_args(argc, argv, pos, &npos)) >= 0 || npos) {
        MessageBoxW(NULL, wide(errors[0] ? errors : "hero-nfc-gui takes the options of hero-nfc, no command"),
                    L"hero-nfc-gui", MB_OK | MB_ICONERROR);
        return USAGE;
    }

    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"hero-nfc-gui";
    RegisterClassW(&wc);
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    hwnd = CreateWindowW(L"hero-nfc-gui", L"Hero of Robots NFC cards (experimental)",
                         WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                         shot_file ? -4000 : CW_USEDEFAULT, shot_file ? -4000 : CW_USEDEFAULT,
                         wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, inst, NULL);
    create_children();

    if (load_cards(0)) {
        status(errors[0] ? errors : "no card list");
    } else {
        fill_list();
    }
    errors[0] = 0;
    reader_ok = open_reader() == OK;
    if (reader_ok)
        snprintf(s, sizeof(s), "Reader: %s", rd == &fake_reader ? reader_desc() : "PC/SC, first reader matching");
    else
        snprintf(s, sizeof(s), "No reader: %s", errors);
    if (reader_ok && rd == &pcsc_reader)
        snprintf(s + strlen(s), sizeof(s) - strlen(s), " \"%s\"", pcsc_want());
    set_text(h_reader, s);
    errors[0] = 0;
    refresh_tag();
    SetTimer(hwnd, ID_TIMER, 700, NULL);
    if (shot_file) {
        if (ncards) {                   /* show a card, as a user would */
            ListView_SetItemState(h_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetTimer(hwnd, ID_SHOT, 1500, NULL);
    } else {
        ShowWindow(hwnd, show);
    }
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (reader_ok && rd == &pcsc_reader)
        pcsc_drop();
    return 0;
}
