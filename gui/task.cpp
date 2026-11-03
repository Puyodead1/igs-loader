/*
 * Runs with a live log: a game, Verify, the igs-online server and the card
 * tool are child processes (one thread reads the pipe into the log and its
 * log file); a set build runs in-process (run_fn), cancelled by a flag.
 * On Linux the child is in its own session, so Stop signals the process
 * group; on Windows a job object holds it.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gui.h"

#define LOG_MAX (4 << 20)

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int run_capture(const ProcSpec &s, std::string &out)
{
    Spawn h;
    std::string err;
    char buf[4096];
    int n;

    out.clear();
    if (!sys_spawn(s, h, err)) {
        out = err;
        return -1;
    }
    while ((n = sys_read(h, buf, sizeof(buf))) > 0)
        out.append(buf, n);
    n = sys_wait(h);
    sys_close(h);
    return n;
}

/* ---- the log ------------------------------------------------------------------------------- */

void Task::append(const char *p, size_t n)
{
    std::lock_guard<std::mutex> l(m);

    /* colour escapes and CRs dropped */
    for (size_t i = 0; i < n; i++) {
        char c = p[i];

        if (esc) {
            if (esc == 1 && c != '[')
                esc = 0;
            else if (esc == 1)
                esc = 2;
            else if (c >= '@' && c <= '~')
                esc = 0;
            continue;
        }
        if (c == 27) {
            esc = 1;
            continue;
        }
        if (c == '\r')
            continue;
        if (lines.empty() || (!log.empty() && log.back() == '\n'))
            lines.push_back((int)log.size());
        if (c == '\t')
            log.append(4, ' ');
        else
            log.push_back(c);
    }
    if (log.size() > LOG_MAX) {
        size_t cut = log.find('\n', log.size() / 2);

        log.erase(0, cut == std::string::npos ? log.size() / 2 : cut + 1);
        lines.clear();
        for (size_t i = 0; i < log.size();) {
            size_t nl = log.find('\n', i);

            lines.push_back((int)i);
            i = nl == std::string::npos ? log.size() : nl + 1;
        }
    }
    if (file) {
        fwrite(p, 1, n, file);
        fflush(file);
    }
}

void Task::line(const char *s)
{
    append(s, strlen(s));
    if (!*s || s[strlen(s) - 1] != '\n')
        append("\n", 1);
}

void Task::note(const std::string &s)
{
    line(("[igs-loader-gui] " + s).c_str());
}

void Task::clear()
{
    std::lock_guard<std::mutex> l(m);
    log.clear();
    lines.clear();
}

/* ---- running ------------------------------------------------------------------------------- */

bool Task::begin(const std::string &w, const std::string &lf)
{
    if (busy())
        return false;
    if (th_.joinable())
        th_.join();
    what = w;
    logfile = lf;
    exit_code = 0;
    stop_req = false;
    cancel = 0;
    nread = last_read = 0;
    started = now_ms();
    clear();
    file = logfile.empty() ? nullptr : fopen(logfile.c_str(), "wb");
    state = RUNNING;
    return true;
}

void Task::end(int code)
{
    exit_code = code;
    note(std::string(stop_req ? "stopped" : "finished") + ", status " + std::to_string(code));
    if (file)
        fclose(file);
    file = nullptr;
    state = IDLE;
}

bool Task::start(const ProcSpec &s, const std::string &w, const std::string &lf)
{
    if (!begin(w, lf))
        return false;
    spec = s;
    inproc = false;
    th_ = std::thread([this] {
        std::string err, cmd;
        char buf[8192];
        int n;

        for (auto &kv : spec.env)
            cmd += kv.first + "=" + kv.second + " ";
        for (auto &a : spec.argv)
            cmd += a + " ";
        note(cmd);
        if (!sys_spawn(spec, h, err)) {
            note("cannot start: " + err);
            end(-1);
            return;
        }
        while ((n = sys_read(h, buf, sizeof(buf))) > 0) {
            nread += n;
            last_read = now_ms() - started;
            append(buf, n);
        }
        n = sys_wait(h);
        sys_close(h);
        end(n);
    });
    return true;
}

bool Task::run_fn(const std::string &w, const std::string &lf, std::function<int(Task &)> fn)
{
    if (!begin(w, lf))
        return false;
    inproc = true;
    th_ = std::thread([this, fn] { end(fn(*this)); });
    return true;
}

void Task::stop()
{
    if (state != RUNNING)
        return;
    state = STOPPING;
    stop_req = true;
    if (inproc) {
        cancel = 1;                     /* the build checks it between steps and files */
        return;
    }
    std::thread([this] {
#ifdef _WIN32
        sys_kill(h);
#else
        sys_signal(h, spec.stop_signal);
        for (long long t = now_ms(); state != IDLE && now_ms() - t < 8000;)
            sys_sleep_ms(50);
        if (state != IDLE)
            sys_kill(h);
#endif
    }).detach();
}
