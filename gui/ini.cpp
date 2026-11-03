/*
 * igs-loader.ini as lines: key=value lines are changed in place, new keys go
 * after the last key of their section (a global after its commented-out
 * "#key=" line if there is one), and everything else (comments, blank and
 * unknown lines, the line ends) is written back as it was read.  The
 * loaders read the globals before the first [section] and the running
 * id's [<id>] section (romset/romset.c); the GUI's own settings are [gui].
 */
#include <cstdio>

#include "gui.h"

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");

    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

/* a [section] line: its name; else false */
static bool header(const std::string &line, std::string &name)
{
    std::string t = trim(line);
    size_t e;

    if (t.empty() || t[0] != '[' || (e = t.find(']')) == std::string::npos)
        return false;
    name = trim(t.substr(1, e - 1));
    return true;
}

/* a key=value line: key and value; comments are not */
static bool keyval(const std::string &line, std::string &k, std::string &v)
{
    std::string t = trim(line);
    size_t eq = t.find('=');

    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[' || eq == std::string::npos || !eq)
        return false;
    k = trim(t.substr(0, eq));
    v = trim(t.substr(eq + 1));
    return true;
}

bool Ini::load(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "rb");
    std::string all, cur;
    char buf[4096];
    size_t n;

    path = p;
    lines.clear();
    crlf = false;
    if (!f)
        return false;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        all.append(buf, n);
    fclose(f);
    crlf = all.find("\r\n") != std::string::npos;
    for (char c : all) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        lines.push_back(cur);
    return true;
}

bool Ini::save()
{
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");

    if (!f)
        return false;
    for (auto &l : lines) {
        fputs(l.c_str(), f);
        fputs(crlf ? "\r\n" : "\n", f);
    }
    if (fclose(f))
        return false;
    return sys_replace(tmp, path);
}

std::string Ini::get(const std::string &sec, const std::string &key, bool *found) const
{
    std::string cur, k, v, h;

    if (found)
        *found = false;
    for (auto &l : lines) {
        if (header(l, h)) {
            cur = "[" + h;
            continue;
        }
        if ((sec.empty() ? cur.empty() : cur == "[" + sec) && keyval(l, k, v) && k == key) {
            if (found)
                *found = true;
            return v;
        }
    }
    return "";
}

KV Ini::section(const std::string &sec) const
{
    std::string cur, k, v, h;
    KV out;

    for (auto &l : lines) {
        if (header(l, h))
            cur = "[" + h;
        else if ((sec.empty() ? cur.empty() : cur == "[" + sec) && keyval(l, k, v))
            out.push_back({ k, v });
    }
    return out;
}

void Ini::set(const std::string &sec, const std::string &key, const std::string &value)
{
    std::string cur, k, v, h;
    int head = sec.empty() ? -1 : -2, last = -1, commented = -1, first_sec = -1;
    bool in;

    for (int i = 0; i < (int)lines.size(); i++) {
        const std::string &l = lines[i];

        if (header(l, h)) {
            cur = "[" + h;
            if (first_sec < 0)
                first_sec = i;
            if (!sec.empty() && h == sec)
                head = i;
            continue;
        }
        in = sec.empty() ? cur.empty() : cur == "[" + sec;
        if (!in)
            continue;
        if (keyval(l, k, v)) {
            if (k == key) {
                if (value.empty()) {
                    lines.erase(lines.begin() + i);
                    /* a section left empty goes too (with its blank lines) */
                    if (!sec.empty() && section(sec).empty()) {
                        int e = head + 1;

                        while (e < (int)lines.size() && trim(lines[e]).empty())
                            e++;
                        if (e >= (int)lines.size() || header(lines[e], h)) {
                            lines.erase(lines.begin() + head, lines.begin() + e);
                            while (head > 0 && head <= (int)lines.size() && trim(lines[head - 1]).empty()
                                   && (head == (int)lines.size() || header(lines[head], h)))
                                lines.erase(lines.begin() + --head);
                        }
                    }
                } else {
                    size_t eq = l.find('=');

                    lines[i] = l.substr(0, eq + 1) + value;
                }
                return;
            }
            last = i;
        } else {
            std::string t = trim(l);

            if (t.size() > 1 && (t[0] == '#' || t[0] == ';') && keyval(trim(t.substr(1)), k, v) && k == key)
                commented = i;
        }
    }
    if (value.empty())
        return;
    std::string line = key + "=" + value;
    if (sec.empty()) {
        int at = commented >= 0 ? commented + 1 : last >= 0 ? last + 1 : first_sec >= 0 ? first_sec : (int)lines.size();

        lines.insert(lines.begin() + at, line);
        if (at == first_sec)
            lines.insert(lines.begin() + at + 1, "");
        return;
    }
    if (head < 0) {
        if (!lines.empty() && !trim(lines.back()).empty())
            lines.push_back("");
        lines.push_back("[" + sec + "]");
        lines.push_back(line);
        return;
    }
    lines.insert(lines.begin() + (last >= 0 ? last + 1 : head + 1), line);
}
