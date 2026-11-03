/*
 * The UI: the game list (romset/ in-process), a game's page (details,
 * snapshot, launch, per-game tunables from the software list, online play),
 * global options, settings, set building, the cache, and the logs.
 *
 * Storage is igs-loader.ini: the loaders' globals (rompath=, cachepath=,
 * hashpath=, chdman=), [<id>] sections with a game's variables (exported by
 * both loaders, so command-line runs honour them) and [gui] for the GUI's
 * own settings and the global options, which it passes as environment.
 *
 * --script <file> runs commands (select, launch, shot, ...) through the
 * same code as the buttons: test runs.
 */
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "gui.h"
#include "imgui.h"
#include "imgui_internal.h"          /* TableSetColumnSortDirection */
#include "misc/cpp/imgui_stdlib.h"
#include "romset.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNM
#define STBI_NO_STDIO_WARNINGS
#include "stb_image.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

static const char *const kinds[] = { "missing", "folder", "7z", "zip" };
static const char doc_url[] = "https://github.com/Puyodead1/igs-loader/blob/main/";

/* ---- state ------------------------------------------------------------------------------ */

static std::string exedir, root, list_error, status;
static double status_until;
static Ini ini;
static long long ini_mtime;

struct Row {
    const sw_info *sw;
    int kind;
    std::string set;
};
static std::vector<Row> rows;
static std::vector<int> view;
static std::string sel;                         /* the selected id */

/* this platform's loader: igs-loader.exe next to the GUI, or run.sh */
static std::string loader_path;
static bool have_loader;

static Task game("Game"), server("Server"), build("Build"), tool("Tool");
static Task *const tasks[] = { &game, &server, &build, &tool };
static std::string game_id, server_id;
static int log_tab;                             /* the log pane's tab to bring forward */
static bool log_tab_set;

/* cache sizes: -2 not known yet, -1 none */
struct CacheSize { long long host = -2; };
static std::map<std::string, CacheSize> cache;
static std::mutex cache_m;
static std::atomic<bool> cache_busy{ false };
static std::thread cache_th;

/* the snapshot shown on the game's page */
static unsigned snap_tex;
static int snap_w, snap_h;
static std::string snap_file, snap_for;
static long long snap_mtime;
static double snap_checked;

static int tab = 0, tab_req = -1;               /* Games, Options, Settings, Build, Cache */
static bool quit_req, quit_pending;
static std::string confirm_text, confirm_title;
static std::function<void()> confirm_action;
static bool confirm_open;

static double now_s()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void say(const std::string &s)
{
    status = s;
    status_until = now_s() + 8;
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");

    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::vector<std::string> split(const std::string &s, char c)
{
    std::vector<std::string> out;
    size_t a = 0, b;

    while ((b = s.find(c, a)) != std::string::npos) {
        out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    out.push_back(s.substr(a));
    return out;
}

static std::string lower(std::string s)
{
    for (auto &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

/* a path from the ini in this host's form (T:\x <-> /mnt/t/x), relative ones under the root */
static std::string host_path(std::string p)
{
    p = trim(p);
    if (p.empty())
        return p;
#ifdef _WIN32
    if (!p.compare(0, 5, "/mnt/") && p.size() >= 6 && (p.size() == 6 || p[6] == '/'))
        p = std::string(1, (char)toupper(p[5])) + ":" + (p.size() > 6 ? p.substr(6) : "\\");
    std::replace(p.begin(), p.end(), '/', '\\');
    bool abs = (p.size() > 1 && p[1] == ':') || p[0] == '\\';
#else
    if (p.size() >= 2 && isalpha((unsigned char)p[0]) && p[1] == ':')
        p = "/mnt/" + std::string(1, (char)tolower(p[0])) + p.substr(2);
    std::replace(p.begin(), p.end(), '\\', '/');
    bool abs = p[0] == '/';
#endif
    return abs ? p : root + SEP + p;
}

static std::string fmt_size(long long n)
{
    char b[32];

    if (n < 0)
        return n == -1 ? "-" : "?";
    if (n >= 1LL << 30)
        snprintf(b, sizeof(b), "%.2f GB", n / 1073741824.0);
    else
        snprintf(b, sizeof(b), "%.1f MB", n / 1048576.0);
    return b;
}

/* ---- the ini --------------------------------------------------------------------------------- */

/* re-read when changed on disk (a text editor, a CLI tool) */
static void ini_poll()
{
    long long m = sys_mtime(rs_ini());

    if (m != ini_mtime) {
        ini.load(rs_ini());
        ini_mtime = m;
    }
}

static bool ini_set(const std::string &sec, const std::string &key, const std::string &value)
{
    bool found;

    ini.load(rs_ini());
    std::string cur = ini.get(sec, key, &found);
    if (found ? cur == value : value.empty())
        return true;
    ini.set(sec, key, value);
    if (!ini.save()) {
        say("cannot write " + std::string(rs_ini()));
        return false;
    }
    ini_mtime = sys_mtime(rs_ini());
    return true;
}

static std::string gui_get(const std::string &k, const std::string &def = "")
{
    bool found;
    std::string v = ini.get("gui", k, &found);

    return found ? v : def;
}

/* ---- the list -------------------------------------------------------------------------------- */

static void refresh()
{
    std::lock_guard<std::mutex> l(cache_m);

    /* a running build reads the lists: reload when it ends (app_frame) */
    if (build.busy()) {
        say("the game list reloads when the build ends");
        return;
    }
    rs_config(root.c_str());
    ini.load(rs_ini());
    ini_mtime = sys_mtime(rs_ini());
    sw_free();
    rows.clear();
    list_error.clear();
    if (sw_load(rs_hashpath())) {
        list_error = rs_error;
        return;
    }
    for (int i = 0; i < sw_count(); i++) {
        char p[RS_PATH];
        Row r;

        r.sw = sw_at(i);
        r.kind = rs_locate(r.sw->name, p, sizeof(p));
        r.set = p;
        rows.push_back(r);
    }
    if (sel.empty() && !rows.empty())
        sel = rows[0].sw->name;
}

static Row *find_row(const std::string &id)
{
    for (auto &r : rows)
        if (id == r.sw->name)
            return &r;
    return nullptr;
}

static Row *selected() { return find_row(sel); }

/* ---- the global options (IGS_LOADER.md, Usage and Windows) --------------------------------------- */

enum { H_WIN = 1, H_LINUX = 2, H_ALL = 3 };
#ifdef _WIN32
#define H_THIS H_WIN
#else
#define H_THIS H_LINUX
#endif
struct Opt { const char *key, *type, *def, *text; int host; };
static const Opt opts[] = {
    { "IGS_FULLSCREEN", "bool", "0", "keep the game's fullscreen mode (Windows: an X11 game's frame is scaled to the monitor)", H_ALL },
    { "IGS_FPS_CAP", "int", "60", "frame-rate cap (0 = off)", H_ALL },
    { "IGS_FPS", "bool", "0", "log the frame rate every 5 s", H_ALL },
    { "IGS_VERBOSE", "bool", "0", "log every message between the game and the emulated hardware", H_ALL },
    { "IGS_TITLE", "str", "-", "window title instead of the game's name (0 keeps the game's own)", H_ALL },
    { "IGS_VSYNC", "bool", "0", "also wait for vblank", H_WIN },
    { "IGS_BACKGROUND", "bool", "0", "open the game's window without taking the keyboard focus", H_WIN },
    { "IGS_SCREEN", "str", "-", "the X screen's size as the cabinet's, WxH", H_ALL },
    { "IGS_WRAP", "str", "-", "run the game under a command, e.g. gdb -q --args", H_LINUX },
    { "IGS_X11_GRAB", "bool", "0", "keep the game's X keyboard and pointer grabs", H_LINUX },
    { "IGS_AUDIO_CHUNK", "int", "-", "audio buffer in samples (default: the game's)", H_ALL },
    { "IGS_AUDIO_LOG", "bool", "0", "log the game's audio calls", H_ALL },
    { "IGS_NO_AUDIO", "bool", "0", "no audio device; audio drains at the real rate", H_ALL },
    { "IGS_DSP_LATENCY_MS", "int", "60", "OSS /dev/dsp latency, ms", H_ALL },
    { "IGS_SHOT_EVERY", "int", "600", "frames between snapshots (Snapshots below)", H_ALL },
    { "IGS_SCRIPT", "str", "-", "press keys on a timetable, sec:key[+],... (testing)", H_ALL },
    { "IGS_MISSING_OK", "bool", "0", "start even with unresolved symbols", H_WIN },
    { "IGS_LDDEBUG", "enum:1|2", "-", "loader detail: 1 modules, inits, dlopen/dlsym; 2 every symbol binding", H_WIN },
    { "SDL_VIDEODRIVER", "str", "-", "SDL video driver (default wayland, or the game's)", H_LINUX },
    { "SDL_AUDIODRIVER", "str", "-", "SDL audio driver (default pulseaudio, or the game's)", H_LINUX },
};

static std::string snap_dir() { return host_path(gui_get("snap", "snap")); }

/* the environment for a run of id: the global options (unless the game's section or the real
 * environment has them), snapshots */
static KV launch_env(const std::string &id)
{
    KV env, sec = ini.section(id);
    auto has = [&](const KV &kv, const std::string &k) {
        for (auto &e : kv)
            if (e.first == k)
                return true;
        return false;
    };

    for (auto &o : opts) {
        std::string v = gui_get(o.key);

        if (!(o.host & H_THIS) || v.empty() || has(sec, o.key) || !sys_getenv(o.key).empty())
            continue;
        env.push_back({ o.key, v });
    }
    if (gui_get("snapshots") == "1" && !has(sec, "IGS_SHOT") && sys_getenv("IGS_SHOT").empty()) {
        std::string d = snap_dir() + SEP + id;

        sys_mkdirs(d);
        env.push_back({ "IGS_SHOT", d + SEP + id + "-" });
    }
    return env;
}

/* ---- running --------------------------------------------------------------------------------- */

static std::string logs_dir()
{
    std::string d = root + SEP "logs";

    sys_mkdirs(d);
    return d;
}

static void launch(bool verify)
{
    Row *r = selected();
    ProcSpec s;
    std::string id;

    if (game.busy()) {
        say("a game is running: one at a time");
        return;
    }
    if (!r || !have_loader) {
        say(r ? "no loader here (" + loader_path + ")" : "no game selected");
        return;
    }
    id = r->sw->name;
    s.env = launch_env(id);
#ifdef _WIN32
    s.argv = { loader_path };
    s.cwd = exedir;
    s.minimized = app_background();
#else
    s.argv = { "bash", loader_path };
#endif
    if (verify) {
        s.argv.push_back("--verify");
        s.env.erase(std::remove_if(s.env.begin(), s.env.end(),
                                   [](const std::pair<std::string, std::string> &kv) { return kv.first == "IGS_SHOT"; }),
                    s.env.end());
    }
    s.argv.push_back(id);
    game.start(s, (verify ? "verify " : "") + id, logs_dir() + SEP + id + (verify ? ".verify.log" : ".log"));
    game_id = id;
    log_tab = 0;
    log_tab_set = true;
    say(std::string(verify ? "verifying " : "running ") + id);
}

static void stop_game()
{
    if (game.busy()) {
        game.stop();
        say("stopping " + game_id);
    }
}

/* <info name="online" value="<igs-online package> <port>"/> */
static bool online_info(const sw_info *sw, std::string &pkg, int &port)
{
    const char *v = sw_info_value(sw, "online", NULL);
    char p[64];

    if (!v || sscanf(v, "%63s %d", p, &port) != 2)
        return false;
    pkg = p;
    return true;
}

static std::string online_addr(const sw_info *sw)
{
    std::string pkg;
    int port = 10000;

    online_info(sw, pkg, port);
    return gui_get(std::string("online.") + sw->name, "127.0.0.1:" + std::to_string(port));
}

static bool server_configured() { return !gui_get("online_dir").empty(); }

#ifdef _WIN32
static std::string python() { return gui_get("python", "python"); }
#else
static std::string python() { return gui_get("python", "python3"); }
#endif

static void start_server(const sw_info *sw)
{
    std::string pkg, addr = online_addr(sw), dir = host_path(gui_get("online_dir"));
    int port = 10000;
    ProcSpec s;

    if (server.busy() || !online_info(sw, pkg, port))
        return;
    if (addr.find(':') == std::string::npos)
        addr += ":" + std::to_string(port);
    if (!sys_is_dir(dir + SEP "igs_online")) {
        say("no igs_online package in " + dir);
        return;
    }
    s.argv = { python(), "-u", "-m", "igs_online", pkg, "--listen", addr };
    s.cwd = dir;
    server.start(s, pkg + " " + addr, logs_dir() + SEP "online-" + pkg + ".log");
    server_id = sw->name;
    log_tab = 1;
    log_tab_set = true;
    say("igs-online " + pkg + " on " + addr);
}

/* <info name="tool" value="<program> [Group] label"/>: a helper next to the loaders */
struct Tool { std::string prog, group, label, exe; bool window; };

static bool tool_info(const sw_info *sw, Tool &t)
{
    const char *v = sw_info_value(sw, "tool", NULL);
    size_t a, b;

    if (!v)
        return false;
    std::string s = v;
    a = s.find(' ');
    t.prog = s.substr(0, a);
    t.label = a == std::string::npos ? t.prog : trim(s.substr(a));
    if (!t.label.empty() && t.label[0] == '[' && (b = t.label.find(']')) != std::string::npos) {
        t.group = t.label.substr(1, b - 1);
        t.label = trim(t.label.substr(b + 1));
    }
#ifdef _WIN32
    t.window = sys_exists(t.exe = exedir + "\\" + t.prog + "-gui.exe");
    if (!t.window && !sys_exists(t.exe = exedir + "\\" + t.prog + ".exe"))
        t.exe.clear();
#else
    t.window = false;
    if (!sys_exists(t.exe = root + "/build/" + t.prog) && !sys_exists(t.exe = exedir + "/" + t.prog))
        t.exe.clear();
#endif
    return true;
}

static std::string tool_args_buf = "info";

static void run_tool(const sw_info *sw, const std::string &args)
{
    Tool t;
    std::string err;
    KV env = ini.section(sw->name);
    std::vector<std::string> argv;

    if (!tool_info(sw, t) || t.exe.empty())
        return;
    argv = { t.exe, "--root", root, "--game", sw->name };
    if (t.window) {
        for (auto &a : split(args, ' '))
            if (!a.empty())
                argv.push_back(a);
        if (!sys_spawn_detached(argv, env, exedir, err))
            say(err);
        else
            say("started " + t.prog);
        return;
    }
    if (tool.busy())
        return;
    for (auto &a : split(args, ' '))
        if (!a.empty())
            argv.push_back(a);
    argv.push_back("--yes");
    ProcSpec s;
    s.argv = argv;
    s.env = env;
    tool.start(s, t.prog + " " + args, logs_dir() + SEP + t.prog + ".log");
    log_tab = 3;
    log_tab_set = true;
}

/* ---- set building (mkromset/, in-process: setbuild.cpp) ---------------------------------------- */

static std::string b_id, b_image, b_out, b_format = "7z";
static int b_mx = 9;
static bool b_noverify;

static void start_build()
{
    std::string why;

    if (build.busy() || b_id.empty() || b_image.empty() || !setbuild_available(why))
        return;
    std::string out = host_path(b_out.empty() ? split(rs_rompath(), ';')[0] : b_out);
    sys_mkdirs(out);
    std::string id = b_id, image = host_path(b_image), fmt = b_format;
    int mx = b_mx;
    bool nv = b_noverify;
    build.run_fn("build " + id, logs_dir() + SEP "build-" + id + ".log", [=](Task &t) {
        t.note("build " + id + " from " + image + " into " + out + " (" + fmt + (nv ? ", no verify" : "") + ")");
        return setbuild_run(id, image, out, fmt, mx, nv, [&t](const char *line) { t.line(line); }, &t.cancel);
    });
    log_tab = 2;
    log_tab_set = true;
    say("building " + id);
}

/* ---- the cache ------------------------------------------------------------------------------- */

static void cache_refresh()
{
    std::vector<std::pair<std::string, std::string>> dirs;

    if (cache_busy)
        return;
    for (auto &r : rows) {
        char d[RS_PATH];

        rs_cache_dir(r.sw->name, d, sizeof(d));
        dirs.push_back({ r.sw->name, d });
    }
    if (cache_th.joinable())
        cache_th.join();
    cache_busy = true;
    cache_th = std::thread([dirs] {
        std::map<std::string, CacheSize> c;

        for (auto &d : dirs)
            c[d.first].host = sys_is_dir(d.second) ? rs_tree_size(d.second.c_str()) : -1;
        std::lock_guard<std::mutex> l(cache_m);
        cache = c;
        cache_busy = false;
    });
}

static void cache_reset(const std::string &id)
{
    if (game.busy() && game_id == id) {
        say(id + " is running");
        return;
    }
    if (rs_cache_reset(id.c_str()))
        say(rs_error);
    else
        say("deleted the extracted copy of " + id);
    cache_refresh();
}

static void ask(const std::string &title, const std::string &text, std::function<void()> fn)
{
    confirm_title = title;
    confirm_text = text;
    confirm_action = fn;
    confirm_open = true;
}

static void ask_cache_reset(const std::string &id)
{
    char d[RS_PATH];

    rs_cache_dir(id.c_str(), d, sizeof(d));
    ask("Reset the cache", "Delete the extracted copy of " + id + " (" + d + ")?\n\n"
        "The game's settings, bookkeeping and rankings are kept in it and go with it.\n"
        "The next launch extracts the set again.", [id] { cache_reset(id); });
}

/* ---- the snapshot ---------------------------------------------------------------------------- */

static bool image_ext(const std::string &n)
{
    std::string e = lower(n.substr(n.find_last_of('.') + 1));

    return e == "png" || e == "jpg" || e == "jpeg" || e == "ppm";
}

/* snap/<id>.png (MAME), else the newest image in snap/<id>/ (IGS_SHOT frames) */
static void snap_update(const std::string &id)
{
    std::string best, d = snap_dir();
    long long bt = -1;

    if (id == snap_for && now_s() - snap_checked < 1.5)
        return;
    snap_checked = now_s();
    for (const char *e : { ".png", ".jpg", ".ppm" })
        if (best.empty() && sys_exists(d + SEP + id + e))
            best = d + SEP + id + e;
    if (best.empty())
        for (auto &n : sys_list_dir(d + SEP + id)) {
            std::string p = d + SEP + id + SEP + n;
            long long t = sys_mtime(p);

            if (image_ext(n) && t > bt) {
                bt = t;
                best = p;
            }
        }
    long long mt = best.empty() ? -1 : sys_mtime(best);
    if (id == snap_for && best == snap_file && mt == snap_mtime)
        return;
    int w, h, n;
    unsigned char *px = best.empty() ? nullptr : stbi_load(best.c_str(), &w, &h, &n, 4);
    if (!px && id == snap_for && !best.empty())
        return;                         /* half written: keep the last one */
    snap_for = id;
    snap_file = best;
    snap_mtime = mt;
    if (!px) {
        gfx_free(snap_tex);
        snap_tex = 0;
        return;
    }
    snap_tex = gfx_texture(px, w, h, snap_tex);
    snap_w = w;
    snap_h = h;
    stbi_image_free(px);
}

/* ---- widgets --------------------------------------------------------------------------------- */

/* a text field kept while edited; true (and v) when an edit is done */
static std::map<std::string, std::string> edits;
static std::set<std::string> editing;

static bool text_field(const std::string &uid, const std::string &hint, std::string &v, ImGuiInputTextFlags fl = 0)
{
    std::string &b = edits[uid];

    if (!editing.count(uid))
        b = v;
    ImGui::InputTextWithHint(("##" + uid).c_str(), hint.c_str(), &b, fl);
    if (ImGui::IsItemActive())
        editing.insert(uid);
    else
        editing.erase(uid);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        v = trim(b);
        return true;
    }
    return false;
}

/* one value of type int, hex, bool, str or enum:a|b; v "" = unset (the default) */
static bool value_field(const std::string &uid, const std::string &type, const std::string &def, std::string &v)
{
    bool unset_def = def == "-";

    if (type == "bool") {
        bool d = def == "1", on = v.empty() ? d : v != "0";

        if (ImGui::Checkbox(("##" + uid).c_str(), &on)) {
            v = on == d ? "" : on ? "1" : "0";
            return true;
        }
        return false;
    }
    if (!type.compare(0, 5, "enum:")) {
        std::vector<std::string> ch = split(type.substr(5), '|');
        std::string shown = v.empty() ? (unset_def ? "(unset)" : def + " (default)") : v;
        bool changed = false;

        if (ImGui::BeginCombo(("##" + uid).c_str(), shown.c_str())) {
            if (ImGui::Selectable(unset_def ? "(unset)" : (def + " (default)").c_str(), v.empty())) {
                v.clear();
                changed = true;
            }
            for (auto &c : ch)
                if (c != def && ImGui::Selectable(c.c_str(), v == c)) {
                    v = c;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }
    std::string nv = v;
    ImGuiInputTextFlags fl = type == "int" ? ImGuiInputTextFlags_CharsDecimal : 0;
    if (text_field(uid, unset_def ? "unset" : "default " + def, nv, fl)) {
        char *e = nullptr;

        if (nv == def)
            nv.clear();
        if ((type == "int" || type == "hex") && !nv.empty()) {
            strtoll(nv.c_str(), &e, type == "int" ? 10 : 0);
            if (*e) {
                say(nv + (type == "int" ? " is not a number" : " is not a number (0x24, 36)"));
                return false;
            }
        }
        v = nv;
        return true;
    }
    return false;
}

static void help_marker(const char *text)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/* ---- the Games tab --------------------------------------------------------------------------- */

static std::string filter_text, filter_machine = "all";
static bool only_available, show_clones = true, group_clones = true;
static int sort_col = 0;
static bool sort_desc;
static int sort_req = -1;               /* a script's sort: through the table, like a header click */

static std::string col_value(const Row &r, int c)
{
    switch (c) {
    case 0: return r.sw->name;
    case 1: return r.sw->description;
    case 2: return r.sw->list;
    case 3: return r.sw->year ? r.sw->year : "";
    case 4: return r.sw->cloneof ? r.sw->cloneof : "";
    default: return kinds[r.kind];
    }
}

static bool matches(const Row &r)
{
    std::string f = lower(trim(filter_text));

    if (filter_machine != "all" && filter_machine != r.sw->list)
        return false;
    if (only_available && r.kind == RS_NONE)
        return false;
    if (!show_clones && r.sw->cloneof)
        return false;
    if (f.empty())
        return true;
    for (int c = 0; c < 6; c++)
        if (lower(col_value(r, c)).find(f) != std::string::npos)
            return true;
    return lower(r.sw->publisher ? r.sw->publisher : "").find(f) != std::string::npos;
}

static void build_view()
{
    std::vector<int> v;
    auto cmp = [](int a, int b) {
        std::string x = lower(col_value(rows[a], sort_col)), y = lower(col_value(rows[b], sort_col));

        if (x == y)
            return strcmp(rows[a].sw->name, rows[b].sw->name) < 0;
        return sort_desc ? x > y : x < y;
    };

    for (int i = 0; i < (int)rows.size(); i++)
        if (matches(rows[i]))
            v.push_back(i);
    std::sort(v.begin(), v.end(), cmp);
    view.clear();
    if (!group_clones) {
        view = v;
        return;
    }
    /* clones right after their parent (a clone whose parent is filtered out stays on its own) */
    std::set<std::string> shown;
    for (int i : v)
        shown.insert(rows[i].sw->name);
    for (int i : v) {
        const sw_info *s = rows[i].sw;

        if (s->cloneof && shown.count(s->cloneof))
            continue;
        view.push_back(i);
        for (int j : v)
            if (rows[j].sw->cloneof && !strcmp(rows[j].sw->cloneof, s->name))
                view.push_back(j);
    }
}

static void games_list()
{
    static const char *const heads[] = { "Id", "Description", "Machine", "Year", "Parent", "Set" };
    const ImGuiTableFlags tf = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    std::vector<std::string> machines = { "all" };

    for (auto &r : rows)
        if (std::find(machines.begin(), machines.end(), r.sw->list) == machines.end())
            machines.push_back(r.sw->list);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##search", "Search (id, name, year, ...)", &filter_text);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    if (ImGui::BeginCombo("##machine", filter_machine.c_str())) {
        for (auto &m : machines)
            if (ImGui::Selectable(m.c_str(), m == filter_machine))
                filter_machine = m;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Sets found", &only_available);
    ImGui::SameLine();
    ImGui::Checkbox("Clones", &show_clones);
    ImGui::SameLine();
    ImGui::Checkbox("Group", &group_clones);
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        refresh();
    if (!list_error.empty())
        ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", list_error.c_str());

    if (!ImGui::BeginTable("games", 6, tf))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (int c = 0; c < 6; c++)
        ImGui::TableSetupColumn(heads[c], c == 1 ? ImGuiTableColumnFlags_WidthStretch : (c == 0 ? ImGuiTableColumnFlags_DefaultSort : 0));
    ImGui::TableHeadersRow();
    if (sort_req >= 0) {
        ImGui::TableSetColumnSortDirection(sort_req, sort_desc ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending, false);
        sort_req = -1;
    }
    if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs())
        if (ss->SpecsDirty && ss->SpecsCount) {
            sort_col = ss->Specs[0].ColumnIndex;
            sort_desc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            ss->SpecsDirty = false;
        }
    build_view();
    ImGuiListClipper clip;
    clip.Begin((int)view.size());
    while (clip.Step())
        for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
            Row &r = rows[view[k]];
            bool is_sel = sel == r.sw->name;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(r.sw->name);
            std::string label = (group_clones && r.sw->cloneof && k > 0 ? "  " : "") + std::string(r.sw->name);
            if (r.kind == RS_NONE)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                sel = r.sw->name;
                if (ImGui::IsMouseDoubleClicked(0) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                    ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown))
                    launch(false);
            }
            if (ImGui::IsItemFocused() && !is_sel)
                sel = r.sw->name;           /* keyboard and gamepad move the selection */
            for (int c = 1; c < 6; c++) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TextUnformatted(col_value(r, c).c_str());
            }
            if (r.kind == RS_NONE)
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
    ImGui::EndTable();
}

static void tunables(const sw_info *sw)
{
    std::vector<std::string> groups = { "" };
    Tool t;
    bool has_tool = tool_info(sw, t);

    for (int i = 0; i < sw->ntun; i++)
        if (std::find(groups.begin(), groups.end(), sw->tun[i].group) == groups.end())
            groups.push_back(sw->tun[i].group);
    if (has_tool && std::find(groups.begin(), groups.end(), t.group) == groups.end())
        groups.push_back(t.group);
    for (auto &g : groups) {
        bool open = g.empty() ? sw->ntun > 0 : ImGui::CollapsingHeader(g.c_str(), g == "Testing" ? 0 : ImGuiTreeNodeFlags_DefaultOpen);

        if (!open)
            continue;
        if (g.empty())
            ImGui::SeparatorText("Options");
        if (ImGui::BeginTable(("tun" + g).c_str(), 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("k");
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11);
            ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthStretch);
            for (int i = 0; i < sw->ntun; i++) {
                const sw_tunable &tu = sw->tun[i];
                std::string v = ini.get(sw->name, tu.name);

                if (g != tu.group)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(tu.name);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1);
                if (value_field(std::string(sw->name) + "." + tu.name, tu.type, tu.def, v))
                    ini_set(sw->name, tu.name, v);
                ImGui::TableSetColumnIndex(2);
                ImGui::AlignTextToFramePadding();
                ImGui::TextWrapped("%s", tu.text);
            }
            ImGui::EndTable();
        }
        if (has_tool && g == t.group) {
            if (t.exe.empty()) {
                ImGui::TextDisabled("%s: %s not found", t.label.c_str(), t.prog.c_str());
            } else if (t.window) {
                if (ImGui::Button(("Open " + t.label).c_str()))
                    run_tool(sw, "");
            } else {
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
                ImGui::InputTextWithHint("##toolargs", "info, list, write <card>, points <n>, ...", &tool_args_buf);
                ImGui::SameLine();
                ImGui::BeginDisabled(tool.busy());
                if (ImGui::Button(("Run " + t.label).c_str()))
                    run_tool(sw, tool_args_buf);
                ImGui::EndDisabled();
            }
        }
    }
}

/* the section's other variables (per-game overrides of the global options, anything else) */
static void extra_vars(const sw_info *sw)
{
    static std::string nk, nv;
    KV sec = ini.section(sw->name);
    std::string pkg;
    int port;
    bool any = false;

    for (auto &kv : sec) {
        bool known = kv.first == "IGS_NET" && online_info(sw, pkg, port);

        for (int i = 0; i < sw->ntun; i++)
            known |= kv.first == sw->tun[i].name;
        if (known)
            continue;
        if (!any)
            ImGui::SeparatorText("Other variables");
        any = true;
        ImGui::PushID(kv.first.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kv.first.c_str());
        ImGui::SameLine(ImGui::GetFontSize() * 12);
        std::string v = kv.second;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (text_field(std::string(sw->name) + ".x." + kv.first, "", v))
            ini_set(sw->name, kv.first, v);
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            ini_set(sw->name, kv.first, "");
        ImGui::PopID();
    }
    if (!any)
        ImGui::SeparatorText("Other variables");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::InputTextWithHint("##nk", "NAME", &nk, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##nv", "value", &nv);
    ImGui::SameLine();
    if (ImGui::Button("Add") && !trim(nk).empty() && !trim(nv).empty()) {
        ini_set(sw->name, trim(nk), trim(nv));
        nk.clear();
        nv.clear();
    }
    ImGui::SameLine();
    help_marker("Saved in the game's [id] section of igs-loader.ini; both loaders export it as environment for "
                "this game (variables already set win), e.g. IGS_FPS_CAP=0 for this game only.");
}

static void online_panel(const sw_info *sw)
{
    std::string pkg, addr = online_addr(sw), net = ini.get(sw->name, "IGS_NET");
    int port;
    bool on = !net.empty();

    if (!online_info(sw, pkg, port))
        return;
    ImGui::SeparatorText("Online");
    if (ImGui::Checkbox("Online (IGS_NET)", &on))
        ini_set(sw->name, "IGS_NET", on ? addr : "");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (text_field(std::string("net.") + sw->name, "host:port", addr)) {
        ini_set("gui", std::string("online.") + sw->name, addr);
        if (on)
            ini_set(sw->name, "IGS_NET", addr);
    }
    ImGui::SameLine();
    help_marker("The game connects to an igs-online server at this address instead of staying offline "
                "(saved as IGS_NET in the game's section, so command-line runs go online too).");
    if (!server_configured())
        return;
    bool mine = server.busy() && server_id == sw->name;
    ImGui::Text("Server (igs-online %s):", pkg.c_str());
    ImGui::SameLine();
    if (server.busy()) {
        ImGui::TextColored(ImVec4(.4f, 1, .4f, 1), "running%s", mine ? "" : (" for " + server_id).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Stop server"))
            server.stop();
    } else {
        ImGui::TextDisabled("stopped");
        ImGui::SameLine();
        if (ImGui::Button("Start server"))
            start_server(sw);
    }
}

static void game_page()
{
    Row *r = selected();
    const sw_info *sw;
    float w = ImGui::GetContentRegionAvail().x;

    if (!r) {
        ImGui::TextDisabled("no game selected");
        return;
    }
    sw = r->sw;
    ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextUnformatted(sw->description);
    ImGui::PopFont();

    /* launch */
    bool running = game.busy();
    if (!have_loader) {
        ImGui::TextColored(ImVec4(1, .6f, .3f, 1), "no loader: %s", loader_path.c_str());
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(running || !have_loader);
    if (ImGui::Button("Launch (F5)"))
        launch(false);
    ImGui::SameLine();
    if (ImGui::Button("Verify (F6)"))
        launch(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(game.state != Task::RUNNING);
    if (ImGui::Button("Stop (F8)"))
        stop_game();
    ImGui::EndDisabled();
    if (running) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(.4f, 1, .4f, 1), "%s: %s", game.state == Task::STOPPING ? "stopping" : "running",
                           game.what.c_str());
    }

    if (!ImGui::BeginChild("page", ImVec2(0, 0)))
        return ImGui::EndChild();
    /* details */
    if (ImGui::BeginTable("info", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k");
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        auto line = [](const char *k, const std::string &v) {
            if (v.empty())
                return;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", k);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", v.c_str());
        };
        std::lock_guard<std::mutex> l(cache_m);
        CacheSize cs = cache.count(sw->name) ? cache[sw->name] : CacheSize();

        line("Id", sw->name);
        line("Machine", sw->list);
        line("Year", sw->year ? sw->year : "");
        line("Publisher", sw->publisher ? sw->publisher : "");
        line("Version", sw->version ? sw->version : "");
        line("Disk", std::string(sw->disk ? sw->disk : "") + (sw->disk_sha1 ? std::string("  sha1 ") + sw->disk_sha1 : ""));
        line("Set", std::string(kinds[r->kind]) + (r->set.empty() ? "" : "  " + r->set));
        if (r->kind == RS_7Z || r->kind == RS_ZIP)
            line("Cache", fmt_size(cs.host));
        ImGui::EndTable();
    }
    if (sw->cloneof) {
        ImGui::TextDisabled("Clone of");
        ImGui::SameLine();
        if (ImGui::SmallButton(sw->cloneof))
            sel = sw->cloneof;
    }
    bool first = true;
    for (auto &o : rows)
        if (o.sw->cloneof && !strcmp(o.sw->cloneof, sw->name)) {
            if (first)
                ImGui::TextDisabled("Clones");
            first = false;
            ImGui::SameLine();
            if (ImGui::SmallButton(o.sw->name))
                sel = o.sw->name;
        }
    if (const char *u = sw_info_value(sw, "usage", NULL)) {
        std::string doc = trim(split(u, ';')[0]), local = root + SEP + doc;

#ifdef _WIN32
        std::replace(local.begin(), local.end(), '/', '\\');
#endif
        ImGui::TextDisabled("Controls and settings:");
        ImGui::SameLine();
        if (ImGui::SmallButton(doc.c_str()))
            sys_open(sys_exists(local) ? local : doc_url + doc);
    }

    /* the snapshot */
    snap_update(sw->name);
    if (snap_tex) {
        float iw = std::min(w, (float)snap_w), ih = iw * snap_h / snap_w, maxh = ImGui::GetFontSize() * 18;

        if (ih > maxh) {
            ih = maxh;
            iw = ih * snap_w / snap_h;
        }
        ImGui::Image((ImTextureID)(intptr_t)snap_tex, ImVec2(iw, ih));
        ImGui::TextDisabled("%s", snap_file.c_str());
    } else {
        ImGui::TextDisabled("no snapshot (%s%s%s.png, or Snapshots in Options)", snap_dir().c_str(), SEP, sw->name);
    }

    online_panel(sw);
    tunables(sw);
    extra_vars(sw);
    ImGui::EndChild();
}

static void games_tab()
{
    float lw = ImGui::GetContentRegionAvail().x * 0.52f;

    if (ImGui::BeginChild("list", ImVec2(lw, 0), ImGuiChildFlags_ResizeX))
        games_list();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("details", ImVec2(0, 0)))
        game_page();
    ImGui::EndChild();
}

/* ---- the Options tab ------------------------------------------------------------------------- */

static void options_tab()
{
    ImGui::TextWrapped("Passed to every game as environment by this launcher (a game's own section and variables "
                       "already set win). Stored in [gui] of %s.", rs_ini());
    bool snaps = gui_get("snapshots") == "1";
    if (ImGui::Checkbox("Snapshots", &snaps))
        ini_set("gui", "snapshots", snaps ? "1" : "");
    ImGui::SameLine();
    ImGui::TextDisabled("save frames to %s%s<id>%s (IGS_SHOT, every IGS_SHOT_EVERY frames); the game page shows the newest",
                        snap_dir().c_str(), SEP, SEP);
    if (!ImGui::BeginTable("opts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("Variable");
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 12);
    ImGui::TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (auto &o : opts) {
        std::string v = gui_get(o.key);

        if (!(o.host & H_THIS))
            continue;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(o.key);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        if (value_field(std::string("opt.") + o.key, o.type, o.def, v))
            ini_set("gui", o.key, v);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextWrapped("%s", o.text);
    }
    ImGui::EndTable();
}

/* ---- the Settings tab ------------------------------------------------------------------------ */

struct Settings {
    std::vector<std::string> rompath;
    std::string cachepath, hashpath, chdman, snap, online_dir, python, font_size;
};
static Settings st;
static bool st_loaded;

static void settings_load()
{
    ini.load(rs_ini());
    st.rompath.clear();
    for (auto &p : split(ini.get("", "rompath"), ';'))
        if (!trim(p).empty())
            st.rompath.push_back(trim(p));
    st.cachepath = ini.get("", "cachepath");
    st.hashpath = ini.get("", "hashpath");
    st.chdman = ini.get("", "chdman");
    st.snap = gui_get("snap");
    st.online_dir = gui_get("online_dir");
    st.python = gui_get("python");
    st.font_size = gui_get("font_size");
    st_loaded = true;
}

static void settings_save()
{
    std::string rp;

    for (auto &p : st.rompath)
        if (!trim(p).empty())
            rp += (rp.empty() ? "" : ";") + trim(p);
    bool ok = ini_set("", "rompath", rp) && ini_set("", "cachepath", trim(st.cachepath)) &&
              ini_set("", "hashpath", trim(st.hashpath)) && ini_set("", "chdman", trim(st.chdman)) &&
              ini_set("gui", "snap", trim(st.snap)) && ini_set("gui", "online_dir", trim(st.online_dir)) &&
              ini_set("gui", "python", trim(st.python)) && ini_set("gui", "font_size", trim(st.font_size));
    refresh();
    settings_load();
    if (ok)
        say(std::string("saved ") + rs_ini());
}

static void path_row(const char *label, std::string &v, int pick, const char *hint)
{
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 11);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30);
    ImGui::InputTextWithHint("##v", hint, &v);
    if (pick && sys_can_pick()) {
        ImGui::SameLine();
        if (ImGui::Button("..."))
            sys_pick(pick == 2, label, v);
    }
    ImGui::PopID();
}

static void settings_tab()
{
    if (!st_loaded)
        settings_load();
    ImGui::Text("Settings file: %s", rs_ini());
    ImGui::TextDisabled("Comments and unknown lines are kept.  Paths may be T:\\... or /mnt/t/...; relative ones are "
                        "under %s.", root.c_str());
    ImGui::SeparatorText("Rom sets (both loaders)");
    for (size_t i = 0; i < st.rompath.size(); i++) {
        ImGui::PushID((int)i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(i ? "" : "rompath");
        ImGui::SameLine(ImGui::GetFontSize() * 11);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30);
        ImGui::InputText("##p", &st.rompath[i]);
        if (sys_can_pick()) {
            ImGui::SameLine();
            if (ImGui::Button("..."))
                sys_pick(true, "rompath", st.rompath[i]);
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("up", ImGuiDir_Up) && i)
            std::swap(st.rompath[i], st.rompath[i - 1]);
        ImGui::SameLine();
        if (ImGui::ArrowButton("dn", ImGuiDir_Down) && i + 1 < st.rompath.size())
            std::swap(st.rompath[i], st.rompath[i + 1]);
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            st.rompath.erase(st.rompath.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 10, 0));
    ImGui::SameLine();
    if (ImGui::Button("Add a rompath folder"))
        st.rompath.push_back("");
    path_row("cachepath", st.cachepath, 2, "default: LOCALAPPDATA / ~/.cache");
    path_row("hashpath", st.hashpath, 2, "hash");
    path_row("chdman", st.chdman, 1, "on PATH (set building from a .chd)");
    ImGui::SeparatorText("This launcher ([gui])");
    path_row("snapshots", st.snap, 2, "snap");
    path_row("igs-online", st.online_dir, 2, "the igs-online folder (online play server)");
    path_row("python", st.python, 1, "python for igs-online (default: python / python3)");
    path_row("font size", st.font_size, 0, "16 (after a restart)");
    ImGui::Separator();
    if (ImGui::Button("Save"))
        settings_save();
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
        settings_load();
    ImGui::SameLine();
    ImGui::TextDisabled("Loader: %s%s", loader_path.c_str(), have_loader ? "" : " (missing)");
}

/* ---- the Build tab --------------------------------------------------------------------------- */

static void build_tab()
{
    const sw_info *sw = b_id.empty() ? nullptr : sw_find(b_id.c_str());
    std::string why;

    ImGui::TextWrapped("Builds a rom set from a disk image (.img) or a MAME CHD (.chd; needs chdman) with the "
                       "software list's recipe. 4 to 45 minutes per set.");
    if (!setbuild_available(why)) {
        ImGui::TextColored(ImVec4(1, .6f, .3f, 1), "Unavailable: %s", why.c_str());
        return;
    }
    ImGui::BeginDisabled(build.busy());
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Game");
    ImGui::SameLine(ImGui::GetFontSize() * 11);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    if (ImGui::BeginCombo("##id", b_id.empty() ? "(choose)" : b_id.c_str())) {
        for (auto &r : rows)
            if (sw_feature(r.sw, "build", NULL) && ImGui::Selectable(r.sw->name, b_id == r.sw->name))
                b_id = r.sw->name;
        ImGui::EndCombo();
    }
    if (sw) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s, expects %s (sha1 %s)", sw->description, sw->disk ? sw->disk : "?",
                            sw->disk_sha1 ? sw->disk_sha1 : "?");
    }
    path_row("Image", b_image, 1, ".img or .chd");
    path_row("Output", b_out, 2, (std::string("default: ") + split(rs_rompath(), ';')[0]).c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Format");
    ImGui::SameLine(ImGui::GetFontSize() * 11);
    for (const char *f : { "7z", "zip", "dir" }) {
        if (ImGui::RadioButton(f, b_format == f))
            b_format = f;
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::InputInt("--mx", &b_mx);
    b_mx = std::max(0, std::min(9, b_mx));
    ImGui::SameLine();
    ImGui::Checkbox("--no-verify", &b_noverify);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(build.busy() || b_id.empty() || b_image.empty());
    if (ImGui::Button("Build"))
        start_build();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(build.state != Task::RUNNING);
    if (ImGui::Button("Cancel"))
        build.stop();
    ImGui::EndDisabled();
    if (build.busy()) {
        std::string phase;
        unsigned long long done = setbuild_done, total = setbuild_total;
        char over[96];

        {
            std::lock_guard<std::mutex> l(setbuild_m);
            phase = setbuild_phase;
        }
        if (total)
            snprintf(over, sizeof(over), "%s  %.0f / %.0f MB", phase.c_str(), done / 1048576.0, total / 1048576.0);
        else
            snprintf(over, sizeof(over), "%s  %.0f MB", phase.c_str(), done / 1048576.0);
        ImGui::SameLine();
        ImGui::ProgressBar(total ? (float)done / total : -1.0f * (float)ImGui::GetTime(), ImVec2(ImGui::GetFontSize() * 24, 0), over);
    }
}

/* ---- the Cache tab --------------------------------------------------------------------------- */

static void cache_tab()
{
    ImGui::TextWrapped("An archive set (.7z, .zip) is extracted into the cache on its first launch: %s. The game's "
                       "settings, bookkeeping and rankings live in the extracted copy; a reset deletes them with it.",
                       rs_cachepath());
    ImGui::BeginDisabled(cache_busy);
    if (ImGui::Button(cache_busy ? "Counting ..." : "Refresh sizes"))
        cache_refresh();
    ImGui::EndDisabled();
    if (!ImGui::BeginTable("cache", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("Id");
    ImGui::TableSetupColumn("Set");
    ImGui::TableSetupColumn("Extracted");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    std::lock_guard<std::mutex> l(cache_m);
    for (auto &r : rows) {
        CacheSize cs = cache.count(r.sw->name) ? cache[r.sw->name] : CacheSize();

        ImGui::TableNextRow();
        ImGui::PushID(r.sw->name);
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(r.sw->name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(kinds[r.kind]);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(fmt_size(cs.host).c_str());
        ImGui::TableSetColumnIndex(3);
        if (cs.host >= 0 && ImGui::SmallButton("Reset"))
            ask_cache_reset(r.sw->name);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/* ---- the logs -------------------------------------------------------------------------------- */

static void log_view(Task &t)
{
    ImGui::BeginDisabled(t.state != Task::RUNNING);
    if (ImGui::SmallButton("Stop"))
        t.stop();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        t.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        std::lock_guard<std::mutex> l(t.m);
        ImGui::SetClipboardText(t.log.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(t.logfile.empty() || !sys_exists(t.logfile));
    if (ImGui::SmallButton("Open log file"))
        sys_open(t.logfile);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", t.what.c_str(), t.busy() ? " (running)" : "");
    if (ImGui::BeginChild("text", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        std::lock_guard<std::mutex> l(t.m);
        bool bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2;
        ImGuiListClipper clip;

        clip.Begin((int)t.lines.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const char *a = t.log.data() + t.lines[i];
                const char *e = i + 1 < (int)t.lines.size() ? t.log.data() + t.lines[i + 1] : t.log.data() + t.log.size();

                ImGui::TextUnformatted(a, e > a && e[-1] == '\n' ? e - 1 : e);
            }
        if (bottom)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

static void log_pane()
{
    if (!ImGui::BeginTabBar("logs"))
        return;
    for (int i = 0; i < 4; i++) {
        Task &t = *tasks[i];
        std::string label = std::string(t.name) + (t.busy() ? " *" : "") + "###" + t.name;
        ImGuiTabItemFlags fl = log_tab_set && log_tab == i ? ImGuiTabItemFlags_SetSelected : 0;

        if (ImGui::BeginTabItem(label.c_str(), nullptr, fl)) {
            log_view(t);
            ImGui::EndTabItem();
        }
    }
    log_tab_set = false;
    ImGui::EndTabBar();
}

/* ---- test scripts (--script <file>) ------------------------------------------------------------ */

static std::vector<std::string> script;
static size_t script_pc;
static double script_wait;
static std::string shot_path;
static std::function<bool()> script_until;

static Task *task_named(const std::string &n)
{
    for (auto t : tasks)
        if (lower(t->name) == n)
            return t;
    return nullptr;
}

static void dump_state(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "w");
    Row *r = selected();

    if (!f)
        return;
    fprintf(f, "root %s\nini %s\nrompath %s\ncache %s\nloader %s (%s)\n", root.c_str(), rs_ini(), rs_rompath(),
            rs_cachepath(), loader_path.c_str(), have_loader ? "found" : "missing");
    fprintf(f, "view %d of %d (filter '%s' machine %s sort %d%s)\n", (int)view.size(), (int)rows.size(),
            filter_text.c_str(), filter_machine.c_str(), sort_col, sort_desc ? " desc" : "");
    {
        std::lock_guard<std::mutex> l(cache_m);
        for (int i : view) {
            CacheSize cs = cache.count(rows[i].sw->name) ? cache[rows[i].sw->name] : CacheSize();

            fprintf(f, "  %-26s %-6s %-5s %-8s cache %s\n", rows[i].sw->name, rows[i].sw->list,
                    rows[i].sw->year ? rows[i].sw->year : "", kinds[rows[i].kind], fmt_size(cs.host).c_str());
        }
    }
    if (r) {
        fprintf(f, "selected %s (%s)\n", r->sw->name, r->sw->description);
        for (int i = 0; i < r->sw->ntun; i++)
            fprintf(f, "  tunable %s %s default %s group '%s' value '%s'\n", r->sw->tun[i].name, r->sw->tun[i].type,
                    r->sw->tun[i].def, r->sw->tun[i].group, ini.get(r->sw->name, r->sw->tun[i].name).c_str());
        for (auto &kv : ini.section(r->sw->name))
            fprintf(f, "  section %s=%s\n", kv.first.c_str(), kv.second.c_str());
        fprintf(f, "  snapshot %s\n", snap_file.c_str());
    }
    for (auto t : tasks)
        fprintf(f, "task %s state %d exit %d what '%s' read %lld bytes, last at %lld ms\n", t->name, t->state.load(),
                t->exit_code, t->what.c_str(), t->nread.load(), t->last_read.load());
    fprintf(f, "status %s\n", status.c_str());
    fclose(f);
}

static void script_step()
{
    if (script_pc >= script.size() || now_s() < script_wait)
        return;
    if (script_until) {
        if (!script_until())
            return;
        script_until = nullptr;
    }
    std::string line = trim(script[script_pc++]), cmd, a1, rest;
    if (line.empty() || line[0] == '#')
        return;
    size_t sp = line.find(' ');
    cmd = line.substr(0, sp);
    rest = sp == std::string::npos ? "" : trim(line.substr(sp));
    std::vector<std::string> a = split(rest, ' ');
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : std::string(); };
    auto val = [](const std::string &v) { return v == "-" ? std::string() : v; };
    Row *r = selected();

    fprintf(stderr, "igs-loader-gui script: %s\n", line.c_str());
    if (cmd == "select")
        sel = rest;
    else if (cmd == "filter")
        filter_text = val(rest);
    else if (cmd == "machine")
        filter_machine = rest;
    else if (cmd == "sort") {
        sort_req = atoi(arg(0).c_str());
        sort_desc = arg(1) == "desc";
    } else if (cmd == "tab")
        tab_req = rest == "options" ? 1 : rest == "settings" ? 2 : rest == "build" ? 3 : rest == "cache" ? 4 : 0;
    else if (cmd == "launch")
        launch(false);
    else if (cmd == "verify")
        launch(true);
    else if (cmd == "stop")
        stop_game();
    else if (cmd == "wait")
        script_wait = now_s() + atof(rest.c_str());
    else if (cmd == "waitidle") {
        Task *t = task_named(arg(0));
        double until = now_s() + atof(arg(1).c_str());
        script_until = [t, until] { return !t || !t->busy() || now_s() > until; };
    } else if (cmd == "waitlog") {
        Task *t = task_named(arg(0));
        double until = now_s() + atof(arg(1).c_str());
        std::string text = trim(rest.substr(std::min(rest.size(), arg(0).size() + arg(1).size() + 2)));
        script_until = [t, until, text] {
            if (!t || now_s() > until)
                return true;
            std::lock_guard<std::mutex> l(t->m);
            return t->log.find(text) != std::string::npos;
        };
    } else if (cmd == "tunable" && r) {
        ini_set(r->sw->name, arg(0), val(arg(1)));
    } else if (cmd == "option") {
        ini_set("gui", arg(0), val(arg(1)));
    } else if (cmd == "online" && r) {
        if (!arg(1).empty())
            ini_set("gui", std::string("online.") + r->sw->name, arg(1));
        ini_set(r->sw->name, "IGS_NET", arg(0) == "on" ? online_addr(r->sw) : "");
    } else if (cmd == "server" && r) {
        if (arg(0) == "start")
            start_server(r->sw);
        else
            server.stop();
    } else if (cmd == "setting") {
        std::string k = arg(0), v = val(trim(rest.substr(std::min(rest.size(), k.size() + 1))));
        if (!st_loaded)
            settings_load();
        if (k == "rompath")
            st.rompath = v.empty() ? std::vector<std::string>() : split(v, ';');
        else if (k == "cachepath")
            st.cachepath = v;
        else if (k == "hashpath")
            st.hashpath = v;
        else if (k == "chdman")
            st.chdman = v;
        else if (k == "snap")
            st.snap = v;
        else if (k == "online_dir")
            st.online_dir = v;
        else if (k == "python")
            st.python = v;
    } else if (cmd == "save")
        settings_save();
    else if (cmd == "build") {
        b_id = arg(0);
        b_image = arg(1);
        b_format = arg(2);
        b_out = arg(3);
        b_noverify = arg(4) == "noverify";
        start_build();
    } else if (cmd == "cancel")
        build.stop();
    else if (cmd == "cache" && arg(0) == "refresh") {
        cache_refresh();
        script_until = [] { return !cache_busy; };
    } else if (cmd == "cache" && arg(0) == "reset")
        ask_cache_reset(arg(1));
    else if (cmd == "confirm") {
        if (confirm_action)
            confirm_action();
        confirm_action = nullptr;
        confirm_open = false;
    } else if (cmd == "tool" && r)
        run_tool(r->sw, rest);
    else if (cmd == "shot")
        shot_path = rest;
    else if (cmd == "dump")
        dump_state(rest);
    else if (cmd == "refresh")
        refresh();
    else if (cmd == "quit")
        quit_req = true;
    else
        fprintf(stderr, "igs-loader-gui script: unknown command %s\n", cmd.c_str());
}

/* ---- the frame ------------------------------------------------------------------------------- */

static void confirm_popup()
{
    if (confirm_open && !ImGui::IsPopupOpen("confirm"))
        ImGui::OpenPopup("confirm");
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 32, 0));
    if (ImGui::BeginPopupModal("confirm", nullptr, ImGuiWindowFlags_NoTitleBar)) {
        if (!confirm_open)
            ImGui::CloseCurrentPopup();         /* answered by a script */
        ImGui::SeparatorText(confirm_title.c_str());
        ImGui::TextWrapped("%s", confirm_text.c_str());
        ImGui::Separator();
        if (ImGui::Button("Yes")) {
            if (confirm_action)
                confirm_action();
            confirm_action = nullptr;
            confirm_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            confirm_action = nullptr;
            confirm_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void app_frame()
{
    static float log_h = 0;
    static bool was_building, first = true;
    ImGuiViewport *vp = ImGui::GetMainViewport();

    if (first)
        cache_refresh();                /* the sizes on the game pages */
    first = false;
    script_step();
    ini_poll();
    if (was_building && !build.busy())
        refresh();                      /* a new set on the rompath */
    was_building = build.busy();
    if (quit_pending) {
        bool any = false;

        for (auto t : tasks)
            any |= t->busy();
        if (!any)
            quit_req = true;
    }

    /* F5 launch, F6 verify, F8 stop; gamepad Start / Back */
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        launch(false);
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
        launch(true);
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false))
        stop_game();

    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (log_h <= 0)
        log_h = vp->WorkSize.y * 0.28f;
    float top = ImGui::GetContentRegionAvail().y - log_h - ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("top", ImVec2(0, std::max(top, 100.0f)))) {
        if (ImGui::BeginTabBar("tabs")) {
            static const char *const names[] = { "Games", "Options", "Settings", "Build", "Cache" };

            for (int i = 0; i < 5; i++) {
                if (ImGui::BeginTabItem(names[i], nullptr, tab_req == i ? ImGuiTabItemFlags_SetSelected : 0)) {
                    if (tab != i && i == 4)
                        cache_refresh();
                    if (tab != i && i == 2)
                        settings_load();
                    tab = i;
                    switch (i) {
                    case 0: games_tab(); break;
                    case 1: options_tab(); break;
                    case 2: settings_tab(); break;
                    case 3: build_tab(); break;
                    case 4: cache_tab(); break;
                    }
                    ImGui::EndTabItem();
                }
            }
            tab_req = -1;
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    /* the splitter */
    ImGui::InvisibleButton("split", ImVec2(-1, ImGui::GetStyle().ItemSpacing.y * 2));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive())
        log_h = std::max(60.0f, std::min(vp->WorkSize.y - 160, log_h - ImGui::GetIO().MouseDelta.y));
    if (ImGui::BeginChild("bottom", ImVec2(0, -ImGui::GetFrameHeightWithSpacing())))
        log_pane();
    ImGui::EndChild();
    /* the status line */
    if (now_s() < status_until)
        ImGui::TextUnformatted(status.c_str());
    else
        ImGui::TextDisabled("%d games, %d sets found.  F5 launch, F6 verify, F8 stop; Enter or double-click launches.",
                            (int)rows.size(), (int)std::count_if(rows.begin(), rows.end(), [](const Row &r) { return r.kind != RS_NONE; }));
    confirm_popup();
    ImGui::End();
}

/* ---- setup and the window ------------------------------------------------------------------- */

static std::string find_root()
{
    std::string d = exedir;

    /* next to the GUI, its parent (build/), or two up (build\win\) */
    for (int i = 0; i < 3; i++) {
        size_t p;

        if (sys_is_dir(d + SEP "hash"))
            return d;
        if ((p = d.find_last_of("\\/")) == std::string::npos)
            break;
        d = d.substr(0, p);
    }
    return exedir;
}

void app_init(int argc, char **argv)
{
    exedir = sys_exe_dir();
    root = find_root();
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "r");
            char line[2048];

            while (f && fgets(line, sizeof(line), f))
                script.push_back(line);
            if (f)
                fclose(f);
        }
    rs_quiet = 1;
    refresh();
#ifdef _WIN32
    loader_path = exedir + "\\igs-loader.exe";
    have_loader = sys_exists(loader_path);
#else
    loader_path = root + "/run.sh";
    have_loader = sys_exists(loader_path) && sys_exists(root + "/build/igs-romset");
#endif
}

bool app_close_request()
{
    bool any = false;

    for (auto t : tasks)
        any |= t->busy();
    if (!any)
        return true;
    ask("Quit", "A game, server, build or tool is still running. Stop it and quit?", [] {
        for (auto t : tasks)
            t->stop();
        quit_pending = true;
    });
    return false;
}

bool app_quit() { return quit_req; }

std::string app_shot()
{
    std::string s = shot_path;

    shot_path.clear();
    return s;
}

/* rows bottom-up, as glReadPixels gives them: a bottom-up .bmp */
void app_write_bmp(const std::string &path, const unsigned char *rgba, int w, int h)
{
    int stride = (w * 3 + 3) & ~3, size = 54 + stride * h;
    std::vector<unsigned char> b(size, 0);
    auto put32 = [&](int at, int v) { for (int i = 0; i < 4; i++) b[at + i] = (unsigned char)(v >> (8 * i)); };

    b[0] = 'B';
    b[1] = 'M';
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    b[26] = 1;
    b[28] = 24;
    put32(34, stride * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const unsigned char *p = rgba + ((size_t)y * w + x) * 4;
            unsigned char *q = &b[54 + (size_t)y * stride + x * 3];

            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
        }
    if (FILE *f = fopen(path.c_str(), "wb")) {
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
    }
}

void app_shutdown()
{
    for (auto t : tasks)
        t->stop();
    for (int i = 0; i < 200; i++) {
        bool any = false;

        for (auto t : tasks)
            any |= t->busy();
        if (!any)
            break;
        sys_sleep_ms(50);
    }
    if (cache_th.joinable())
        cache_th.join();
    for (auto t : tasks)
        if (t->th_.joinable())
            t->th_.join();
}

bool app_background() { return !sys_getenv("IGS_BACKGROUND").empty(); }

float app_font_size()
{
    float s = (float)atof(gui_get("font_size", "16").c_str());

    return s >= 8 && s <= 64 ? s : 16;
}

std::string app_font_file()
{
#ifdef _WIN32
    return sys_getenv("WINDIR") + "\\Fonts\\segoeui.ttf";
#else
    for (const char *f : { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                           "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf" })
        if (sys_exists(f))
            return f;
    return "";
#endif
}
