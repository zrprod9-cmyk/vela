// Tiny file logger. No spdlog, no ImGui -- the interceptor has no menu. Writes to the path in the
// INTERCEPT_LOG env var, or <exe dir>\sgsr3intercept.log by default. Thread-safe enough for our use.
#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

class Log
{
  public:
    static Log& I()
    {
        static Log inst;
        return inst;
    }
    void line(const char* fmt, ...)
    {
        std::lock_guard<std::mutex> g(_m);
        if (!_f) open();
        if (!_f) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(_f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list a;
        va_start(a, fmt);
        vfprintf(_f, fmt, a);
        va_end(a);
        fputc('\n', _f);
        fflush(_f);
    }

  private:
    void open()
    {
        char path[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("INTERCEPT_LOG", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
        {
            char exe[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::string p(exe);
            auto slash = p.find_last_of("\\/");
            p = (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "sgsr3intercept.log";
            _f = fopen(p.c_str(), "w");
        }
        else
        {
            _f = fopen(path, "w");
        }
    }
    std::mutex _m;
    FILE* _f = nullptr;
};

#define LOGI(...) Log::I().line(__VA_ARGS__)

// {0}-style logging for code ported from the fork (SGSR3Student_Vk): LOG_INFO("x {0} y {1}", a, b).
#include <cctype>
#include <sstream>
#include <vector>
template <typename... A> std::string fmtBraces(const char* f, const A&... a)
{
    std::vector<std::string> v;
    (v.push_back([&] { std::ostringstream o; o << a; return o.str(); }()), ...);
    std::string out;
    for (const char* p = f; *p; ++p)
    {
        if (*p == '{' && isdigit((unsigned char) p[1]))
        {
            const char* q = p + 1;
            int n = 0;
            while (isdigit((unsigned char) *q)) n = n * 10 + (*q++ - '0');
            if (*q == '}')
            {
                if (n < (int) v.size()) out += v[n];
                p = q;
                continue;
            }
        }
        out += *p;
    }
    return out;
}
#define LOG_INFO(...) Log::I().line("%s", fmtBraces(__VA_ARGS__).c_str())
#define LOG_ERROR(...) Log::I().line("ERROR %s", fmtBraces(__VA_ARGS__).c_str())
