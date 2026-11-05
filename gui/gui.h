/*
 * igs-loader-gui: a launcher for this platform's loader (wiki Loader,
 * "GUI").  app.cpp is the UI, task.cpp the runs, ini.cpp igs-loader.ini,
 * setbuild.cpp the set builder (mkromset/); sys_win.cpp / sys_posix.cpp the
 * host, main_win.cpp (Win32 + WGL) / main_sdl.cpp (SDL2) the window.
 */
#ifndef IGS_GUI_H
#define IGS_GUI_H

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

typedef std::vector<std::pair<std::string, std::string>> KV;

/* ---- the host (sys_*.cpp) ------------------------------------------------------------- */

std::string sys_exe_dir();
bool sys_exists(const std::string &p);
bool sys_is_dir(const std::string &p);
bool sys_mkdirs(const std::string &p);
long long sys_mtime(const std::string &p);
std::vector<std::string> sys_list_dir(const std::string &dir);
bool sys_replace(const std::string &from, const std::string &to);     /* rename over */
bool sys_remove_tree(const std::string &p);
std::string sys_getenv(const char *name);
KV sys_environ();
std::string sys_which(const std::string &exe);                         /* "" if not on PATH */
void sys_open(const std::string &path_or_url);                         /* the desktop's default app */
bool sys_pick(bool folder, const char *title, std::string &out);       /* a file or folder dialog */
bool sys_can_pick();
void sys_sleep_ms(int ms);

/* a child process: its output (stdout and stderr) on one pipe */
struct Spawn {
    void *process = nullptr, *job = nullptr, *out = nullptr;           /* Windows */
    int pid = 0, fd = -1;                                              /* POSIX: also the process group */
};
struct ProcSpec {
    std::vector<std::string> argv;
    KV env;                         /* added to (or replacing in) this process's environment */
    std::string cwd;
    int stop_signal = 15;           /* POSIX: the first signal Stop sends the process group */
    bool minimized = false;         /* Windows: first show minimized, without focus */
};
bool sys_spawn(const ProcSpec &s, Spawn &h, std::string &err);
bool sys_spawn_detached(const std::vector<std::string> &argv, const KV &env, const std::string &cwd,
                        std::string &err);
int sys_read(Spawn &h, char *buf, int n);                              /* blocks; <= 0 at the end */
int sys_wait(Spawn &h);                                                /* exit status */
void sys_signal(Spawn &h, int sig);                                    /* POSIX: to the process group */
void sys_kill(Spawn &h);                                               /* hard: the job / the group */
void sys_close(Spawn &h);

/* run to the end, output captured (the file dialogs on Linux) */
int run_capture(const ProcSpec &s, std::string &out);

/* ---- runs with a live log (task.cpp) --------------------------------------------------- */

struct Task {
    const char *name;
    std::mutex m;
    std::string log;                /* guarded by m */
    std::vector<int> lines;         /* line starts in log */
    std::atomic<int> state{0};      /* IDLE, RUNNING, STOPPING */
    std::atomic<bool> stop_req{false};
    volatile int cancel = 0;        /* an in-process run's cancel flag */
    int exit_code = 0;
    std::string what;               /* "heroofrobots_m100fa", "build scratch_m101cn", ... */
    std::string logfile;
    ProcSpec spec;
    Spawn h;
    bool inproc = false;
    long long started = 0;
    std::atomic<long long> nread{0}, last_read{0};     /* bytes from the pipe, and when (ms) */
    int esc = 0;                    /* append(): inside an ANSI escape */
    FILE *file = nullptr;           /* the log file, while running */
    std::thread th_;

    explicit Task(const char *n) : name(n) {}
    ~Task() { if (th_.joinable()) th_.detach(); }
    enum { IDLE, RUNNING, STOPPING };
    bool busy() const { return state != IDLE; }
    bool start(const ProcSpec &s, const std::string &what, const std::string &logfile);
    bool run_fn(const std::string &what, const std::string &logfile, std::function<int(Task &)> fn);
    void stop();
    void append(const char *p, size_t n);
    void line(const char *s);
    void note(const std::string &s);
    void clear();
private:
    bool begin(const std::string &what, const std::string &logfile);
    void end(int code);
};

/* ---- the set builder, mkromset/ in-process (setbuild.cpp) ----------------------------- */

bool setbuild_available(std::string &why);
/* 0 = built; log gets each line; *cancel set = stop.  The software lists must stay loaded meanwhile. */
int setbuild_run(const std::string &id, const std::string &image, const std::string &outdir, const std::string &format,
                 int mx, bool no_verify, std::function<void(const char *)> log, volatile int *cancel);
extern std::atomic<unsigned long long> setbuild_done, setbuild_total;      /* progress of the phase */
extern std::string setbuild_phase;                                         /* guarded by setbuild_m */
extern std::mutex setbuild_m;

/* ---- igs-loader.ini, comments and unknown lines kept (ini.cpp) ------------------------ */

struct Ini {
    std::string path;
    std::vector<std::string> lines;
    bool crlf = false;

    bool load(const std::string &p);
    bool save();
    /* section "" is the globals (before the first [section]) */
    std::string get(const std::string &sec, const std::string &key, bool *found = nullptr) const;
    KV section(const std::string &sec) const;
    void set(const std::string &sec, const std::string &key, const std::string &value);   /* "" removes */
};

/* ---- the window (main_*.cpp) and the UI (app.cpp) ------------------------------------- */

unsigned gfx_texture(const unsigned char *rgba, int w, int h, unsigned old);       /* GL texture */
void gfx_free(unsigned tex);

void app_init(int argc, char **argv);
void app_frame();                   /* the UI, between NewFrame and Render */
bool app_close_request();           /* the window's close button: true = close now */
bool app_quit();                    /* the UI asks to quit */
std::string app_shot();             /* a .bmp to write this frame (test scripts), or "" */
void app_write_bmp(const std::string &path, const unsigned char *rgba, int w, int h);
void app_shutdown();
bool app_background();              /* IGS_BACKGROUND: show without focus */
float app_font_size();
std::string app_font_file();

#endif
