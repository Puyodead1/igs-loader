/*
 * The Linux host: files, child processes (each in its own session, so Stop
 * signals the whole process group), zenity or kdialog for file dialogs,
 * xdg-open.
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gui.h"

extern char **environ;

std::string sys_exe_dir()
{
    char p[4096];
    ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
    char *s;

    p[n > 0 ? n : 0] = 0;
    if ((s = strrchr(p, '/')))
        *s = 0;
    return p;
}

bool sys_exists(const std::string &p)
{
    struct stat st;

    return lstat(p.c_str(), &st) == 0;
}

bool sys_is_dir(const std::string &p)
{
    struct stat st;

    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool sys_mkdirs(const std::string &p)
{
    for (size_t i = 1; i <= p.size(); i++)
        if (i == p.size() || p[i] == '/') {
            std::string d = p.substr(0, i);

            if (!sys_is_dir(d) && mkdir(d.c_str(), 0755) && errno != EEXIST)
                return false;
        }
    return sys_is_dir(p);
}

long long sys_mtime(const std::string &p)
{
    struct stat st;

    return stat(p.c_str(), &st) ? -1 : (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

std::vector<std::string> sys_list_dir(const std::string &dir)
{
    std::vector<std::string> out;
    DIR *d = opendir(dir.c_str());
    struct dirent *e;

    if (!d)
        return out;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            out.push_back(e->d_name);
    closedir(d);
    return out;
}

bool sys_replace(const std::string &from, const std::string &to) { return rename(from.c_str(), to.c_str()) == 0; }

bool sys_remove_tree(const std::string &p)
{
    struct stat st;

    if (lstat(p.c_str(), &st))
        return true;
    if (!S_ISDIR(st.st_mode))
        return unlink(p.c_str()) == 0;
    chmod(p.c_str(), 0700);
    for (auto &n : sys_list_dir(p))
        sys_remove_tree(p + "/" + n);
    return rmdir(p.c_str()) == 0;
}

std::string sys_getenv(const char *name)
{
    const char *v = getenv(name);

    return v ? v : "";
}

KV sys_environ()
{
    KV out;

    for (char **e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');

        if (eq)
            out.push_back({ std::string(*e, eq - *e), eq + 1 });
    }
    return out;
}

std::string sys_which(const std::string &exe)
{
    std::string path = sys_getenv("PATH");

    if (exe.find('/') != std::string::npos)
        return access(exe.c_str(), X_OK) ? "" : exe;
    for (size_t a = 0; a <= path.size();) {
        size_t b = path.find(':', a);
        std::string d = path.substr(a, b == std::string::npos ? std::string::npos : b - a), f = d + "/" + exe;

        if (!d.empty() && !access(f.c_str(), X_OK))
            return f;
        if (b == std::string::npos)
            break;
        a = b + 1;
    }
    return "";
}

void sys_open(const std::string &p)
{
    std::string err;

    sys_spawn_detached({ "xdg-open", p }, {}, "", err);
}

bool sys_can_pick() { return !sys_which("zenity").empty() || !sys_which("kdialog").empty(); }

bool sys_pick(bool folder, const char *title, std::string &out)
{
    ProcSpec s;
    std::string r;

    if (!sys_which("zenity").empty()) {
        s.argv = { "zenity", "--file-selection", std::string("--title=") + title };
        if (folder)
            s.argv.push_back("--directory");
    } else if (!sys_which("kdialog").empty()) {
        s.argv = { "kdialog", "--title", title, folder ? "--getexistingdirectory" : "--getopenfilename", "." };
    } else {
        return false;
    }
    if (run_capture(s, r) || r.empty())
        return false;
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r'))
        r.pop_back();
    out = r;
    return !out.empty();
}

void sys_sleep_ms(int ms) { usleep(ms * 1000); }

/* ---- processes ------------------------------------------------------------------------------ */

static void child_exec(const std::vector<std::string> &argv, const KV &env, const std::string &cwd)
{
    std::vector<char *> a;

    for (auto &kv : env)
        setenv(kv.first.c_str(), kv.second.c_str(), 1);
    if (!cwd.empty() && chdir(cwd.c_str())) {
        perror(cwd.c_str());
        _exit(127);
    }
    for (auto &s : argv)
        a.push_back((char *)s.c_str());
    a.push_back(nullptr);
    execvp(a[0], a.data());
    perror(a[0]);
    _exit(127);
}

bool sys_spawn(const ProcSpec &s, Spawn &h, std::string &err)
{
    int p[2];
    pid_t pid;

    if (pipe2(p, O_CLOEXEC)) {
        err = strerror(errno);
        return false;
    }
    if ((pid = fork()) < 0) {
        err = strerror(errno);
        close(p[0]);
        close(p[1]);
        return false;
    }
    if (!pid) {
        int nul = open("/dev/null", O_RDONLY);

        setsid();                       /* its own process group: Stop signals all of it */
        dup2(nul, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        signal(SIGPIPE, SIG_DFL);
        child_exec(s.argv, s.env, s.cwd);
    }
    close(p[1]);
    h.pid = pid;
    h.fd = p[0];
    return true;
}

bool sys_spawn_detached(const std::vector<std::string> &argv, const KV &env, const std::string &cwd, std::string &err)
{
    pid_t pid = fork();

    if (pid < 0) {
        err = strerror(errno);
        return false;
    }
    if (!pid) {
        /* double fork: no zombie, no tie to the GUI */
        setsid();
        if (fork())
            _exit(0);
        child_exec(argv, env, cwd);
    }
    waitpid(pid, nullptr, 0);
    return true;
}

int sys_read(Spawn &h, char *buf, int n)
{
    ssize_t r;

    if (h.fd < 0)
        return 0;
    while ((r = read(h.fd, buf, n)) < 0 && errno == EINTR)
        ;
    return (int)r;
}

int sys_wait(Spawn &h)
{
    int st = 0;

    if (h.pid <= 0 || waitpid(h.pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

void sys_signal(Spawn &h, int sig)
{
    if (h.pid > 0)
        kill(-h.pid, sig);
}

void sys_kill(Spawn &h) { sys_signal(h, SIGKILL); }

void sys_close(Spawn &h)
{
    if (h.fd >= 0)
        close(h.fd);
    h.fd = -1;
    h.pid = 0;
}
