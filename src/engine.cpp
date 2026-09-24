#include "engine.h"
#include <windows.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <sstream>
#include <chrono>
#include <cwchar>
#include <cstdio>

#pragma comment(lib, "dxgi.lib")

namespace kp {

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring quote(const std::wstring& a) {
    std::wstring out = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; out.push_back(c); }
        else if (c == L'"') { out.append(bs + 1, L'\\'); out.push_back(c); bs = 0; }
        else { bs = 0; out.push_back(c); }
    }
    out.append(bs, L'\\');
    out.push_back(L'"');
    return out;
}

static DWORD runCapture(const std::wstring& cmd, std::string& out) {
    out.clear();
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return (DWORD)-1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    BOOL ok = CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return (DWORD)-1; }

    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, n);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

static std::string primaryGpuName() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
        return "GPU";
    std::string best;
    SIZE_T bestVram = 0;
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(ad->GetDesc1(&d))) {
            std::wstring name = d.Description;
            bool basic = (name.find(L"Basic Render") != std::wstring::npos);
            if (!basic && d.DedicatedVideoMemory >= bestVram) {
                bestVram = d.DedicatedVideoMemory;
                best = toUtf8(name);
            }
        }
        ad->Release();
    }
    factory->Release();
    return best.empty() ? "GPU" : best;
}

static std::wstring selfDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; size_t i = s.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : s.substr(0, i);
}
static std::wstring g_toolsDir;
void setToolsDir(const std::wstring& dir) { g_toolsDir = dir; }

static std::wstring toolExe(const wchar_t* file, const wchar_t* fallback) {
    if (!g_toolsDir.empty()) {
        std::wstring p = g_toolsDir + L"\\" + file;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            return L"\"" + p + L"\"";
    }
    std::wstring local = selfDir() + L"\\bin\\" + file;
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES)
        return L"\"" + local + L"\"";
    return fallback;
}
static std::wstring ffmpegExe()  { return toolExe(L"ffmpeg.exe",  L"ffmpeg"); }
static std::wstring ffprobeExe() { return toolExe(L"ffprobe.exe", L"ffprobe"); }

static bool encoderWorks(const std::wstring& enc) {
    std::wstring cmd = ffmpegExe() +
        L" -hide_banner -loglevel error -f lavfi -i color=c=black:s=256x256:d=0.2:r=10"
        L" -vf format=yuv420p -c:v " + enc + L" -f null -";
    std::string out;
    return runCapture(cmd, out) == 0;
}

Engine::Engine() {}
Engine::~Engine() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

Caps Engine::detect() {
    Caps c;
    c.gpu = primaryGpuName();
    c.nvencHevc = encoderWorks(L"hevc_nvenc");
    c.nvencAv1  = encoderWorks(L"av1_nvenc");
    if (!c.nvencHevc) c.qsvHevc = encoderWorks(L"hevc_qsv");
    c.x265 = true;

    if (c.nvencHevc)      { c.primaryLabel = "HEVC NVENC"; c.primaryCodec = "hevc"; }
    else if (c.qsvHevc)   { c.primaryLabel = "HEVC Quick Sync"; c.primaryCodec = "hevc"; }
    else                  { c.primaryLabel = "HEVC CPU"; c.primaryCodec = "hevc"; }
    caps_ = c;
    return c;
}

static std::string field(const std::string& text, const std::string& key) {
    size_t pos = 0;
    std::string pat = key + "=";
    while ((pos = text.find(pat, pos)) != std::string::npos) {
        bool lineStart = (pos == 0 || text[pos - 1] == '\n' || text[pos - 1] == '\r');
        if (lineStart) {
            size_t s = pos + pat.size();
            size_t e = text.find_first_of("\r\n", s);
            return text.substr(s, e == std::string::npos ? std::string::npos : e - s);
        }
        pos += pat.size();
    }
    return {};
}

MediaInfo Engine::probe(const std::wstring& path) {
    MediaInfo m;
    m.path = path;

    size_t slash = path.find_last_of(L"\\/");
    m.name = toUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1));

    std::wstring cmd = ffprobeExe() +
        L" -v error -select_streams v:0"
        L" -show_entries format=duration,size,bit_rate"
        L" -show_entries stream=width,height,codec_name"
        L" -of default=noprint_wrappers=1 " + quote(path);
    std::string out;
    DWORD code = runCapture(cmd, out);
    if (code != 0 && out.empty()) {
        m.error = "Nao foi possivel ler o arquivo";
        return m;
    }
    m.durationSec = atof(field(out, "duration").c_str());
    m.sizeBytes   = _atoi64(field(out, "size").c_str());
    m.width       = atoi(field(out, "width").c_str());
    m.height      = atoi(field(out, "height").c_str());
    m.vcodec      = field(out, "codec_name");

    std::wstring ac = ffprobeExe() +
        L" -v error -select_streams a:0 -show_entries stream=codec_name"
        L" -of default=noprint_wrappers=1:nokey=1 " + quote(path);
    std::string aout;
    runCapture(ac, aout);

    for (size_t i = 0; i < aout.size(); ++i) {
        if (aout[i] != '\r' && aout[i] != '\n') {
            size_t e = aout.find_first_of("\r\n", i);
            m.acodec = aout.substr(i, e == std::string::npos ? std::string::npos : e - i);
            break;
        }
    }

    if (m.sizeBytes == 0) {

        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
            ULARGE_INTEGER u; u.HighPart = fa.nFileSizeHigh; u.LowPart = fa.nFileSizeLow;
            m.sizeBytes = (long long)u.QuadPart;
        }
    }
    m.ok = (m.durationSec > 0 && m.width > 0);
    if (!m.ok && m.error.empty()) m.error = "Arquivo de video invalido";
    return m;
}

void Engine::start(const Job& job, ProgressFn onProgress, DoneFn onDone, LogFn onLog) {
    if (running_.load()) return;
    cancel_.store(false);
    running_.store(true);
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&Engine::worker, this, job, onProgress, onDone, onLog);
}

void Engine::cancel() {
    cancel_.store(true);
    HANDLE h = childProcess_.load();
    if (h) TerminateProcess(h, 1);
}

void Engine::worker(Job job, ProgressFn onProgress, DoneFn onDone, LogFn onLog) {
    auto t0 = std::chrono::steady_clock::now();
    Result res;
    res.outPath = job.output;

    MediaInfo info = probe(job.input);
    if (!info.ok) {
        res.error = info.error.empty() ? "Falha ao analisar o video" : info.error;
        running_.store(false);
        onDone(res);
        return;
    }
    res.inBytes = info.sizeBytes;
    { size_t slash = job.output.find_last_of(L"\\/");
      res.outName = toUtf8(slash == std::wstring::npos ? job.output : job.output.substr(slash + 1)); }

    double dur = info.durationSec;
    double keep = (100.0 - job.reductionPercent) / 100.0;
    double targetBytes = (double)info.sizeBytes * keep;
    double audioBps = 128000.0;
    double audioBytes = audioBps / 8.0 * dur;
    double overhead = targetBytes * 0.03;
    double videoBytes = targetBytes - audioBytes - overhead;
    if (videoBytes < 1) videoBytes = targetBytes * 0.5;
    double vbps = videoBytes * 8.0 / dur;
    if (vbps < 150000.0) vbps = 150000.0;
    long long vb = (long long)vbps;
    long long mx = (long long)(vb * 1.5);
    long long bf = (long long)(vb * 2.0);

    std::wstring enc;
    bool nvenc = false, isHevc = true;
    if (job.codec == "av1" && caps_.nvencAv1) { enc = L"av1_nvenc"; nvenc = true; isHevc = false; }
    else if (caps_.nvencHevc)                 { enc = L"hevc_nvenc"; nvenc = true; }
    else if (caps_.qsvHevc)                   { enc = L"hevc_qsv"; }
    else                                      { enc = L"libx265"; }

    std::wstring preset;
    if (nvenc) preset = (job.speed == "turbo") ? L"p2" : (job.speed == "quality") ? L"p6" : L"p4";
    else if (enc == L"hevc_qsv") preset = (job.speed == "turbo") ? L"veryfast" : (job.speed == "quality") ? L"slow" : L"medium";
    else  preset = (job.speed == "turbo") ? L"veryfast" : (job.speed == "quality") ? L"slow" : L"medium";

    auto num = [](long long v) { return std::to_wstring(v); };

    std::wstring cmd = ffmpegExe() +
        L" -y -hide_banner -loglevel error -i " + quote(job.input) +
        L" -sn -dn -map_metadata 0 -c:v " + enc + L" -preset " + preset;

    if (nvenc) {
        cmd += L" -rc vbr -b:v " + num(vb) + L" -maxrate " + num(mx) +
               L" -bufsize " + num(bf) + L" -multipass qres";
    } else {
        cmd += L" -b:v " + num(vb) + L" -maxrate " + num(mx) + L" -bufsize " + num(bf);
    }
    cmd += L" -pix_fmt yuv420p";
    if (isHevc) cmd += L" -tag:v hvc1";
    cmd += L" -c:a aac -b:a 128k -movflags +faststart";
    cmd += L" -progress pipe:1 -nostats " + quote(job.output);

    onLog(toUtf8(L"encoder=" + enc + L" preset=" + preset + L" vbps=" + num(vb)));

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    BOOL ok = CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        res.error = "Nao foi possivel iniciar o ffmpeg";
        running_.store(false);
        onDone(res);
        return;
    }
    childProcess_.store(pi.hProcess);

    std::string acc;
    std::string errAcc;
    char buf[4096];
    DWORD n = 0;
    auto lastEmit = std::chrono::steady_clock::now();

    auto handleLine = [&](const std::string& line) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            std::string t = line;
            while (!t.empty() && (t.back() == '\r' || t.back() == '\n')) t.pop_back();
            if (!t.empty()) { errAcc += t + "\n"; onLog(t); }
            return;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();

        static double curUs = 0, curFps = 0, curSpeed = 0;
        static long long curBytes = 0;
        if (key == "out_time_us")      curUs = atof(val.c_str());
        else if (key == "fps")         curFps = atof(val.c_str());
        else if (key == "total_size")  curBytes = _atoi64(val.c_str());
        else if (key == "speed") {
            std::string s = val; if (!s.empty() && (s.back() == 'x' || s.back() == 'X')) s.pop_back();
            curSpeed = atof(s.c_str());
        }
        else if (key == "progress") {
            auto now = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - lastEmit).count();
            if (ms >= 150.0 || val == "end") {
                lastEmit = now;
                Progress p;
                double curSec = curUs / 1e6;
                p.percent = dur > 0 ? (curSec / dur) * 100.0 : 0;
                if (p.percent > 99.9) p.percent = 99.9;
                if (val == "end") p.percent = 100.0;
                p.fps = curFps;
                p.speed = curSpeed;
                p.outBytes = curBytes;
                p.etaSec = (curSpeed > 0.01 && dur > curSec) ? (dur - curSec) / curSpeed : 0;
                onProgress(p);
            }
        }
    };

    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
        acc.append(buf, n);
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            handleLine(acc.substr(0, nl));
            acc.erase(0, nl + 1);
        }
    }
    if (!acc.empty()) handleLine(acc);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    childProcess_.store(nullptr);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    auto t1 = std::chrono::steady_clock::now();
    res.elapsedSec = std::chrono::duration<double>(t1 - t0).count();

    if (cancel_.load()) {
        DeleteFileW(job.output.c_str());
        res.error = "Cancelado";
        running_.store(false);
        onDone(res);
        return;
    }
    if (code != 0) {
        DeleteFileW(job.output.c_str());
        res.error = errAcc.empty() ? "Falha na codificacao" : errAcc;
        running_.store(false);
        onDone(res);
        return;
    }

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(job.output.c_str(), GetFileExInfoStandard, &fa)) {
        ULARGE_INTEGER u; u.HighPart = fa.nFileSizeHigh; u.LowPart = fa.nFileSizeLow;
        res.outBytes = (long long)u.QuadPart;
    }
    res.ratioPercent = res.inBytes > 0 ? (1.0 - (double)res.outBytes / (double)res.inBytes) * 100.0 : 0;
    res.ok = true;
    running_.store(false);
    onDone(res);
}

}
