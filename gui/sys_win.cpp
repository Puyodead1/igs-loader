/*
 * The Windows host: files, child processes (a pipe for stdout and stderr, a
 * job object so Stop ends the whole tree, no console windows), the file
 * dialog.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gui.h"

std::string sys_exe_dir()
{
    char p[MAX_PATH], *s;

    GetModuleFileNameA(NULL, p, sizeof(p));
    if ((s = strrchr(p, '\\')))
        *s = 0;
    return p;
}

bool sys_exists(const std::string &p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool sys_is_dir(const std::string &p)
{
    DWORD a = GetFileAttributesA(p.c_str());

    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool sys_mkdirs(const std::string &p)
{
    for (size_t i = 3; i <= p.size(); i++)
        if (i == p.size() || p[i] == '\\' || p[i] == '/') {
            std::string d = p.substr(0, i);

            if (!sys_is_dir(d) && !CreateDirectoryA(d.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
    return sys_is_dir(p);
}

long long sys_mtime(const std::string &p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;

    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a))
        return -1;
    return ((long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

std::vector<std::string> sys_list_dir(const std::string &dir)
{
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);

    if (h == INVALID_HANDLE_VALUE)
        return out;
    do
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
            out.push_back(fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

bool sys_replace(const std::string &from, const std::string &to)
{
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool sys_remove_tree(const std::string &p)
{
    DWORD a = GetFileAttributesA(p.c_str());

    if (a == INVALID_FILE_ATTRIBUTES)
        return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileA(p.c_str()) || RemoveDirectoryA(p.c_str());
    }
    for (auto &n : sys_list_dir(p))
        sys_remove_tree(p + "\\" + n);
    return RemoveDirectoryA(p.c_str()) != 0;
}

std::string sys_getenv(const char *name)
{
    char buf[32768];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));

    return n && n < sizeof(buf) ? std::string(buf, n) : "";
}

KV sys_environ()
{
    KV out;
    char *e = GetEnvironmentStringsA(), *p;

    for (p = e; *p; p += strlen(p) + 1) {
        const char *eq = strchr(p + 1, '=');      /* "=C:=C:\..." entries start with '=' */

        if (eq)
            out.push_back({ std::string(p, eq - p), eq + 1 });
    }
    FreeEnvironmentStringsA(e);
    return out;
}

std::string sys_which(const std::string &exe)
{
    char buf[MAX_PATH];

    if (exe.find_first_of("\\/") != std::string::npos)
        return sys_exists(exe) ? exe : "";
    return SearchPathA(NULL, exe.c_str(), ".exe", sizeof(buf), buf, NULL) ? buf : "";
}

void sys_open(const std::string &p)
{
    ShellExecuteA(NULL, "open", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

bool sys_can_pick() { return true; }

bool sys_pick(bool folder, const char *title, std::string &out)
{
    IFileOpenDialog *d = nullptr;
    IShellItem *it = nullptr;
    PWSTR w = nullptr;
    wchar_t wt[256];
    bool ok = false;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void **)&d)))
        return false;
    MultiByteToWideChar(CP_ACP, 0, title, -1, wt, 256);
    d->SetTitle(wt);
    if (folder) {
        FILEOPENDIALOGOPTIONS o;

        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS);
    }
    if (SUCCEEDED(d->Show(GetActiveWindow())) && SUCCEEDED(d->GetResult(&it))
        && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &w))) {
        char a[MAX_PATH * 2];

        WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof(a), NULL, NULL);
        out = a;
        ok = true;
        CoTaskMemFree(w);
    }
    if (it)
        it->Release();
    d->Release();
    return ok;
}

void sys_sleep_ms(int ms) { Sleep(ms); }

/* ---- processes ------------------------------------------------------------------------------ */

/* one argument for CommandLineToArgvW-style parsing */
static std::string quote(const std::string &a)
{
    std::string r = "\"";
    int bs = 0;

    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos)
        return a;
    for (char c : a) {
        if (c == '\\') {
            bs++;
            continue;
        }
        r.append(c == '"' ? bs * 2 + 1 : bs, '\\');
        r.push_back(c);
        bs = 0;
    }
    r.append(bs * 2, '\\');
    return r + "\"";
}

/* this process's environment with env added, as a sorted block */
static std::string env_block(const KV &env)
{
    KV all = sys_environ();
    std::string b;

    for (auto &kv : env) {
        bool done = false;

        for (auto &a : all)
            if (!_stricmp(a.first.c_str(), kv.first.c_str())) {
                a.second = kv.second;
                done = true;
            }
        if (!done)
            all.push_back(kv);
    }
    std::stable_sort(all.begin(), all.end(), [](const std::pair<std::string, std::string> &x,
                                                const std::pair<std::string, std::string> &y) {
        return _stricmp(x.first.c_str(), y.first.c_str()) < 0;
    });
    for (auto &kv : all) {
        b += kv.first + "=" + kv.second;
        b.push_back(0);
    }
    b.push_back(0);
    return b;
}

static std::string cmdline(const std::vector<std::string> &argv)
{
    std::string c;

    for (auto &a : argv)
        c += (c.empty() ? "" : " ") + quote(a);
    return c;
}

static std::string last_error(const char *what)
{
    char m[256] = "";

    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), 0, m, sizeof(m), NULL);
    return std::string(what) + ": " + m;
}

bool sys_spawn(const ProcSpec &s, Spawn &h, std::string &err)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd, wr, nul, inherit[2];
    STARTUPINFOEXA si;
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    std::string cmd = cmdline(s.argv), env = env_block(s.env);
    SIZE_T alen = 0;
    std::vector<char> attrs;
    BOOL ok;

    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        err = last_error("pipe");
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    if (s.minimized) {
        si.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        si.StartupInfo.wShowWindow = SW_SHOWMINNOACTIVE;
    }
    /* the child inherits these two handles only, never the GUI's others (log files, other children's pipes) */
    inherit[0] = nul;
    inherit[1] = wr;
    InitializeProcThreadAttributeList(NULL, 1, 0, &alen);
    attrs.resize(alen);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrs.data();
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &alen);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), NULL, NULL);
    ok = CreateProcessA(NULL, &cmd[0], NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                        &env[0], s.cwd.empty() ? NULL : s.cwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) {
        err = last_error(s.argv.empty() ? "?" : s.argv[0].c_str());
        CloseHandle(rd);
        return false;
    }
    /* the job: Stop ends the whole tree, and so does the GUI's exit */
    h.job = CreateJobObjectA(NULL, NULL);
    memset(&lim, 0, sizeof(lim));
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(h.job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    AssignProcessToJobObject(h.job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    h.process = pi.hProcess;
    h.out = rd;
    h.pid = (int)pi.dwProcessId;
    return true;
}

bool sys_spawn_detached(const std::vector<std::string> &argv, const KV &env, const std::string &cwd, std::string &err)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    std::string cmd = cmdline(argv), e = env_block(env);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, 0, &e[0], cwd.empty() ? NULL : cwd.c_str(), &si, &pi)) {
        err = last_error(argv[0].c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int sys_read(Spawn &h, char *buf, int n)
{
    DWORD got = 0;

    if (!h.out || !ReadFile(h.out, buf, n, &got, NULL))
        return 0;
    return (int)got;
}

int sys_wait(Spawn &h)
{
    DWORD code = 0;

    if (!h.process)
        return -1;
    WaitForSingleObject(h.process, INFINITE);
    GetExitCodeProcess(h.process, &code);
    return (int)code;
}

void sys_signal(Spawn &, int) {}

void sys_kill(Spawn &h)
{
    if (h.job)
        TerminateJobObject(h.job, 1);
    else if (h.process)
        TerminateProcess(h.process, 1);
}

void sys_close(Spawn &h)
{
    HANDLE job = h.job;

    if (h.out)
        CloseHandle(h.out);
    if (h.process)
        CloseHandle(h.process);
    h.out = h.process = h.job = nullptr;
    if (job)
        CloseHandle(job);       /* KILL_ON_JOB_CLOSE: whatever the child left behind ends too */
}
