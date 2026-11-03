/*
 * The Build tab's set builder: mkromset/ (the C port of romtool/mkromset.py)
 * linked in and run on the task's thread, with its log, progress and cancel
 * flag.  Built without it (no -DIGS_GUI_MKROMSET), the tab says so.
 */
#include <cstring>

#include "gui.h"
#include "romset.h"

#ifdef IGS_GUI_MKROMSET
#include "mkromset.h"

std::atomic<unsigned long long> setbuild_done, setbuild_total;
std::string setbuild_phase;
std::mutex setbuild_m;

bool setbuild_available(std::string &) { return true; }

struct ctx { std::function<void(const char *)> *log; };

static void on_log(void *c, int level, const char *line)
{
    std::string s = level == MKR_ERROR ? std::string("ERROR: ") + line : line;  /* warnings carry their own prefix */

    (*((ctx *)c)->log)(s.c_str());
}

static void on_progress(void *, const char *phase, uint64_t done, uint64_t total)
{
    {
        std::lock_guard<std::mutex> l(setbuild_m);
        setbuild_phase = phase ? phase : "";
    }
    setbuild_done = done;
    setbuild_total = total;
}

int setbuild_run(const std::string &id, const std::string &image, const std::string &outdir, const std::string &format,
                 int mx, bool no_verify, std::function<void(const char *)> log, volatile int *cancel)
{
    struct mkr_options o;
    ctx c = { &log };
    char err[512] = "";
    int r;

    mkr_options_init(&o);
    o.id = id.c_str();
    o.sw = sw_find(id.c_str());
    o.image = image.c_str();
    o.output = outdir.c_str();
    o.format = mkr_format_parse(format.c_str());
    o.mx = mx;
    o.no_verify = no_verify;
    o.log = on_log;
    o.progress = on_progress;
    o.ctx = &c;
    o.cancel = cancel;
    setbuild_done = setbuild_total = 0;
    r = mkr_build(&o, err, sizeof(err));
    if (r == MKR_CANCELLED)
        log("cancelled");
    return r;                           /* a failure's reason is already in the log (MKR_ERROR) */
}

#else

std::atomic<unsigned long long> setbuild_done, setbuild_total;
std::string setbuild_phase;
std::mutex setbuild_m;

bool setbuild_available(std::string &why)
{
    why = "this build has no set builder (mkromset/)";
    return false;
}

int setbuild_run(const std::string &, const std::string &, const std::string &, const std::string &, int, bool,
                 std::function<void(const char *)> log, volatile int *)
{
    log("no set builder (mkromset/) in this build");
    return -1;
}
#endif
