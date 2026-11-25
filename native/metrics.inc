// System counters use the OS or the display driver's management API.
#include <map>
#include <pdh.h>
#include <sstream>
#include <wincodec.h>
#include <wincrypt.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#include "gpu.inc"
extern "C"
{
    int metrics(double *v)
    {
        MEMORYSTATUSEX m{sizeof(m)};
        GlobalMemoryStatusEx(&m);
        v[0] = double(m.ullTotalPhys - m.ullAvailPhys);
        v[1] = double(m.ullTotalPhys);
        FILETIME i, k, u;
        GetSystemTimes(&i, &k, &u);
        auto value = [](FILETIME t) {
            return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        static unsigned long long lastIdle = 0, lastTotal = 0;
        auto idle = value(i), total = value(k) + value(u);
        v[2] = lastTotal && total > lastTotal ? 100.0 * (1.0 - double(idle - lastIdle) / double(total - lastTotal)) : 0;
        lastTotal = total;
        lastIdle = idle;
        static GpuCounters counters;
        return counters.read(v);
    }
    int http_request(const wchar_t *url, const char *json, void *out, int (*append)(void *, const char *, size_t))
    {
        URL_COMPONENTS c{sizeof(c)};
        c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = (DWORD)-1;
        if (!WinHttpCrackUrl(url, 0, 0, &c))
            return 0;
        wstring host(c.lpszHostName, c.dwHostNameLength), path(c.lpszUrlPath, c.dwUrlPathLength);
        if (c.dwExtraInfoLength)
            path.append(c.lpszExtraInfo, c.dwExtraInfoLength);
        auto ses = WinHttpOpen(L"FavBar", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
        if (!ses)
            return 0;
        WinHttpSetTimeouts(ses, 1200, 1200, 1500, 1500);
        auto conn = WinHttpConnect(ses, host.c_str(), c.nPort, 0);
        auto req = conn ? WinHttpOpenRequest(conn, json ? L"POST" : L"GET", path.c_str(), nullptr, nullptr, nullptr,
                                             c.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
        int n = 0;
        DWORD code = 0, sz = sizeof(code);
        if (req &&
            WinHttpSendRequest(req, json ? L"Content-Type: application/json" : nullptr, (DWORD)-1, (void *)json,
                               json ? (DWORD)strlen(json) : 0, json ? (DWORD)strlen(json) : 0, 0) &&
            WinHttpReceiveResponse(req, nullptr) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz,
                                nullptr) &&
            code >= 200 && code < 300)
        {
            char buffer[8192];
            for (;;)
            {
                DWORD got = 0;
                if (!WinHttpReadData(req, buffer, sizeof(buffer), &got))
                    break;
                if (!got)
                {
                    n = 1;
                    break;
                }
                if (!append(out, buffer, got))
                    break;
            }
        }
        if (req)
            WinHttpCloseHandle(req);
        if (conn)
            WinHttpCloseHandle(conn);
        WinHttpCloseHandle(ses);
        return n;
    }
    const wchar_t *gpu_processes()
    {
        static thread_local wstring result;
        result.clear();
        PDH_HQUERY q{};
        PDH_HCOUNTER counter{};
        if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS)
            return L"GPU counters unavailable";
        if (PdhAddEnglishCounterW(q, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &counter) == ERROR_SUCCESS &&
            PdhCollectQueryData(q) == ERROR_SUCCESS)
        {
            DWORD bytes = 0, count = 0;
            PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, nullptr);
            std::vector<BYTE> raw(bytes);
            auto a = (PDH_FMT_COUNTERVALUE_ITEM_W *)raw.data();
            std::map<DWORD, LONGLONG> usage;
            if (bytes && PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, a) == ERROR_SUCCESS)
                for (DWORD i = 0; i < count; i++)
                {
                    DWORD pid = 0;
                    if (swscanf_s(a[i].szName, L"pid_%lu_", &pid) == 1 && a[i].FmtValue.CStatus <= 1)
                        usage[pid] += a[i].FmtValue.largeValue;
                }
            std::vector<std::pair<LONGLONG, DWORD>> sorted;
            for (auto item : usage)
                if (item.second >= 32 * 1024 * 1024)
                    sorted.emplace_back(item.second, item.first);
            std::sort(sorted.rbegin(), sorted.rend());
            for (size_t j = 0; j < std::min<size_t>(8, sorted.size()); j++)
            {
                auto [amount, pid] = sorted[j];
                wstring name = std::to_wstring(pid);
                auto p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (p)
                {
                    wchar_t file[32768];
                    DWORD size = 32768;
                    if (QueryFullProcessImageNameW(p, 0, file, &size))
                        name = PathFindFileNameW(file);
                    CloseHandle(p);
                }
                result += name + L"   " + std::to_wstring(amount / 1048576) + L" MiB\n";
            }
        }
        PdhCloseQuery(q);
        return result.empty() ? L"—" : result.c_str();
    }
    long long process_birth(unsigned long pid)
    {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc)
            return 0;
        FILETIME c, e, k, u;
        BOOL ok = GetProcessTimes(proc, &c, &e, &k, &u);
        CloseHandle(proc);
        if (!ok)
            return 0;
        ULARGE_INTEGER t;
        t.LowPart = c.dwLowDateTime;
        t.HighPart = c.dwHighDateTime;
        return (t.QuadPart - 116444736000000000ULL + 50000) / 100000;
    }
    int replace_file(const wchar_t *from, const wchar_t *to)
    {
        return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    const wchar_t *ui_date(unsigned long long seconds)
    {
        ULARGE_INTEGER n;
        n.QuadPart = seconds * 10000000ULL + 116444736000000000ULL;
        FILETIME utc{n.LowPart, n.HighPart}, local;
        SYSTEMTIME st;
        result.clear();
        if (FileTimeToLocalFileTime(&utc, &local) && FileTimeToSystemTime(&local, &st))
        {
            wchar_t s[32];
            swprintf_s(s, L"%02u.%02u.%02u", st.wDay, st.wMonth, st.wYear % 100);
            result = s;
        }
        return result.c_str();
    }
}
