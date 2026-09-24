#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "WebView2.h"
#include "engine.h"
#include "resource.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Microsoft::WRL;

static HWND                       g_hwnd = nullptr;
static ComPtr<ICoreWebView2>            g_webview;
static ComPtr<ICoreWebView2Controller>  g_controller;
static ComPtr<ICoreWebView2Environment> g_env;
static kp::Engine                 g_engine;
static kp::Caps                   g_caps;
static std::atomic<bool>          g_capsReady{false};
static kp::MediaInfo              g_current;

static bool loadRes(int id, const BYTE*& data, DWORD& size) {
    HRSRC h = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!h) return false;
    HGLOBAL g = LoadResource(nullptr, h);
    if (!g) return false;
    data = (const BYTE*)LockResource(g);
    size = SizeofResource(nullptr, h);
    return data && size;
}

struct UiRes { const wchar_t* path; int id; const wchar_t* mime; };
static const UiRes kUi[] = {
    { L"/index.html",         IDR_UI_INDEX, L"text/html; charset=utf-8" },
    { L"/styles.css",         IDR_UI_CSS,   L"text/css; charset=utf-8" },
    { L"/app.js",             IDR_UI_APP,   L"text/javascript; charset=utf-8" },
    { L"/vendor/three.min.js",IDR_UI_THREE, L"text/javascript; charset=utf-8" },
    { L"/vendor/gsap.min.js", IDR_UI_GSAP,  L"text/javascript; charset=utf-8" },
};

struct ToolRes { int id; const wchar_t* name; };
static const ToolRes kTools[] = {
    { IDR_FFMPEG,        L"ffmpeg.exe" },
    { IDR_FFPROBE,       L"ffprobe.exe" },
    { IDR_DLL_AVCODEC,   L"avcodec-63.dll" },
    { IDR_DLL_AVDEVICE,  L"avdevice-63.dll" },
    { IDR_DLL_AVFILTER,  L"avfilter-12.dll" },
    { IDR_DLL_AVFORMAT,  L"avformat-63.dll" },
    { IDR_DLL_AVUTIL,    L"avutil-61.dll" },
    { IDR_DLL_SWRESAMPLE,L"swresample-7.dll" },
    { IDR_DLL_SWSCALE,   L"swscale-10.dll" },
};
static const wchar_t* TOOLS_VERSION = L"ffmpeg-gpl-shared-2026-09";

static const UINT WM_KP_POST = WM_APP + 1;

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
static std::string jesc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

static void postJson(const std::string& json) {
    if (!g_hwnd) return;
    PostMessageW(g_hwnd, WM_KP_POST, 0, (LPARAM)new std::string(json));
}
static void sendCaps() {
    if (!g_capsReady.load()) return;
    std::string j = "{\"type\":\"caps\",\"gpu\":\"" + jesc(g_caps.gpu) +
        "\",\"encoder\":\"" + jesc(g_caps.primaryLabel) +
        "\",\"hevc\":" + (g_caps.nvencHevc || g_caps.qsvHevc || g_caps.x265 ? "true" : "false") +
        ",\"av1\":" + (g_caps.nvencAv1 ? "true" : "false") +
        ",\"hw\":" + ((g_caps.nvencHevc || g_caps.nvencAv1 || g_caps.qsvHevc) ? "true" : "false") + "}";
    postJson(j);
}
static std::string humanSize(long long b) {
    const char* u[] = { "B","KB","MB","GB","TB" };
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    char buf[64]; sprintf_s(buf, v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, u[i]);
    return buf;
}
static void sendFile(const kp::MediaInfo& m) {
    if (!m.ok) {
        postJson("{\"type\":\"error\",\"message\":\"" + jesc(m.error) + "\"}");
        return;
    }
    char buf[256];
    sprintf_s(buf, "%d", (int)(m.durationSec + 0.5));
    std::string j = "{\"type\":\"file\",\"name\":\"" + jesc(m.name) +
        "\",\"sizeBytes\":" + std::to_string(m.sizeBytes) +
        ",\"sizeHuman\":\"" + jesc(humanSize(m.sizeBytes)) +
        "\",\"durationSec\":" + buf +
        ",\"width\":" + std::to_string(m.width) +
        ",\"height\":" + std::to_string(m.height) +
        ",\"vcodec\":\"" + jesc(m.vcodec) + "\"}";
    postJson(j);
}

static std::string getStr(const std::string& j, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat); if (p == std::string::npos) return {};
    p = j.find(':', p + pat.size()); if (p == std::string::npos) return {};
    p++;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
    if (p < j.size() && j[p] == '"') {
        p++; std::string o;
        while (p < j.size() && j[p] != '"') {
            if (j[p] == '\\' && p + 1 < j.size()) { p++; o += j[p]; }
            else o += j[p];
            p++;
        }
        return o;
    }

    std::string o;
    while (p < j.size() && j[p] != ',' && j[p] != '}') { o += j[p]; p++; }
    return o;
}
static int getInt(const std::string& j, const std::string& key, int def) {
    std::string s = getStr(j, key);
    return s.empty() ? def : atoi(s.c_str());
}

static std::wstring openFileDialog() {
    std::wstring result;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return result;
    COMDLG_FILTERSPEC filt[] = {
        { L"Videos", L"*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v;*.wmv;*.flv;*.mpg;*.mpeg;*.ts;*.m2ts;*.3gp" },
        { L"Todos os arquivos", L"*.*" },
    };
    dlg->SetFileTypes(2, filt);
    dlg->SetTitle(L"Escolher video");
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
        }
    }
    return result;
}

static std::wstring outputFor(const std::wstring& input) {
    size_t dot = input.find_last_of(L'.');
    size_t slash = input.find_last_of(L"\\/");
    std::wstring stem = (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        ? input.substr(0, dot) : input;
    return stem + L"_KernelP.mp4";
}

static void doPick(const std::wstring& pathIn) {
    std::wstring path = pathIn.empty() ? openFileDialog() : pathIn;
    if (path.empty()) return;
    g_current = g_engine.probe(path);
    sendFile(g_current);
}

static void doStart(const std::string& msg) {
    if (!g_current.ok) {
        postJson("{\"type\":\"error\",\"message\":\"Escolha um video primeiro\"}");
        return;
    }
    if (g_engine.running()) return;

    kp::Job job;
    job.input = g_current.path;
    job.output = outputFor(g_current.path);
    job.reductionPercent = getInt(msg, "reduction", 90);
    job.codec = getStr(msg, "codec"); if (job.codec.empty()) job.codec = "hevc";
    job.speed = getStr(msg, "speed"); if (job.speed.empty()) job.speed = "balanced";

    auto onProgress = [](const kp::Progress& p) {
        char b[512];
        sprintf_s(b, "{\"type\":\"progress\",\"percent\":%.2f,\"fps\":%.0f,\"speed\":%.2f,\"etaSec\":%.0f,\"outBytes\":%lld}",
                  p.percent, p.fps, p.speed, p.etaSec, p.outBytes);
        postJson(b);
    };
    auto onDone = [](const kp::Result& r) {
        if (!r.ok) {
            postJson("{\"type\":\"error\",\"message\":\"" + jesc(r.error) + "\"}");
            return;
        }
        char b[1024];
        std::string outName = r.outName;
        std::string outHuman = humanSize(r.outBytes);
        std::string inHuman = humanSize(r.inBytes);
        sprintf_s(b, "{\"type\":\"done\",\"outName\":\"%s\",\"outBytes\":%lld,\"outHuman\":\"%s\",\"inHuman\":\"%s\",\"ratio\":%.1f,\"elapsedSec\":%.1f}",
                  jesc(outName).c_str(), r.outBytes, jesc(outHuman).c_str(), jesc(inHuman).c_str(), r.ratioPercent, r.elapsedSec);
        postJson(b);
    };
    auto onLog = [](const std::string& s) {
        postJson("{\"type\":\"log\",\"line\":\"" + jesc(s) + "\"}");
    };
    postJson("{\"type\":\"started\"}");
    g_engine.start(job, onProgress, onDone, onLog);
}

static void doReveal() {
    if (!g_current.ok) return;
    std::wstring out = outputFor(g_current.path);
    std::wstring arg = L"/select,\"" + out + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}
static void doOpenOutput() {
    if (!g_current.ok) return;
    std::wstring out = outputFor(g_current.path);
    ShellExecuteW(nullptr, L"open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void onWebMessage(const std::wstring& wjson) {
    std::string msg = toUtf8(wjson);
    std::string type = getStr(msg, "type");
    if (type == "ready")        sendCaps();
    else if (type == "pick")    doPick(L"");
    else if (type == "start")   doStart(msg);
    else if (type == "cancel")  g_engine.cancel();
    else if (type == "reveal")  doReveal();
    else if (type == "open")    doOpenOutput();
    else if (type == "minimize") ShowWindow(g_hwnd, SW_MINIMIZE);
    else if (type == "close")   DestroyWindow(g_hwnd);
}

static void createWebView() {
    std::wstring udf;
    wchar_t* la = nullptr; size_t len = 0;
    if (_wdupenv_s(&la, &len, L"LOCALAPPDATA") == 0 && la) { udf = la; free(la); }
    udf += L"\\KernelP\\WebView2";

    CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                g_env = env;
                env->CreateCoreWebView2Controller(g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (!ctrl) return S_OK;
                            g_controller = ctrl;
                            g_controller->get_CoreWebView2(&g_webview);

                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(g_controller.As(&c2))) {
                                COREWEBVIEW2_COLOR black{ 255, 0, 0, 0 };
                                c2->put_DefaultBackgroundColor(black);
                            }

                            ComPtr<ICoreWebView2Controller4> c4;
                            if (SUCCEEDED(g_controller.As(&c4)))
                                c4->put_AllowExternalDrop(FALSE);

                            ComPtr<ICoreWebView2Settings> st;
                            g_webview->get_Settings(&st);
                            if (st) {
                                st->put_IsStatusBarEnabled(FALSE);
                                st->put_AreDefaultContextMenusEnabled(FALSE);
                                st->put_IsZoomControlEnabled(FALSE);
                            }

                            EventRegistrationToken tok;
                            g_webview->AddWebResourceRequestedFilter(
                                L"https://appassets.kernelp/*",
                                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                            g_webview->add_WebResourceRequested(
                                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                        ComPtr<ICoreWebView2WebResourceRequest> req;
                                        if (FAILED(args->get_Request(&req)) || !req) return S_OK;
                                        LPWSTR uri = nullptr; req->get_Uri(&uri);
                                        std::wstring u = uri ? uri : L""; if (uri) CoTaskMemFree(uri);
                                        const wchar_t* host = L"appassets.kernelp";
                                        size_t hp = u.find(host);
                                        if (hp == std::wstring::npos) return S_OK;
                                        std::wstring path = u.substr(hp + wcslen(host));
                                        size_t q = path.find_first_of(L"?#");
                                        if (q != std::wstring::npos) path = path.substr(0, q);
                                        if (path.empty() || path == L"/") path = L"/index.html";
                                        for (const auto& r : kUi) {
                                            if (path == r.path) {
                                                const BYTE* d; DWORD n;
                                                if (loadRes(r.id, d, n) && g_env) {
                                                    IStream* s = SHCreateMemStream(d, n);
                                                    std::wstring hdr = std::wstring(L"Content-Type: ") + r.mime;
                                                    ComPtr<ICoreWebView2WebResourceResponse> resp;
                                                    g_env->CreateWebResourceResponse(s, 200, L"OK", hdr.c_str(), &resp);
                                                    if (resp) args->put_Response(resp.Get());
                                                    if (s) s->Release();
                                                }
                                                break;
                                            }
                                        }
                                        return S_OK;
                                    }).Get(), &tok);

                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                            onWebMessage(raw);
                                            CoTaskMemFree(raw);
                                        }
                                        return S_OK;
                                    }).Get(), &tok);

                            g_webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        sendCaps();
                                        return S_OK;
                                    }).Get(), &tok);

                            RECT rc; GetClientRect(g_hwnd, &rc);
                            g_controller->put_Bounds(rc);
                            g_controller->put_IsVisible(TRUE);
                            g_webview->Navigate(L"https://appassets.kernelp/index.html");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_controller) {
            RECT rc; GetClientRect(hwnd, &rc);
            g_controller->put_Bounds(rc);
        }
        return 0;
    case WM_KP_POST: {
        std::string* s = (std::string*)lp;
        if (s) {
            if (g_webview) g_webview->PostWebMessageAsJson(toWide(*s).c_str());
            delete s;
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(hd, 0, path, MAX_PATH)) doPick(path);
        DragFinish(hd);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 900;
        mmi->ptMinTrackSize.y = 620;
        return 0;
    }
    case WM_DESTROY:
        g_engine.cancel();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static std::wstring localAppData() {
    std::wstring r; wchar_t* p = nullptr; size_t len = 0;
    if (_wdupenv_s(&p, &len, L"LOCALAPPDATA") == 0 && p) { r = p; free(p); }
    return r;
}
static bool fileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static std::wstring readTextFile(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    char buf[256]; DWORD n = 0; std::string s;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) s.append(buf, n);
    CloseHandle(h);
    return toWide(s);
}
static bool writeFileBytes(const std::wstring& p, const BYTE* d, DWORD n) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = true; const BYTE* cur = d; DWORD left = n;
    while (left > 0) { if (!WriteFile(h, cur, left, &w, nullptr)) { ok = false; break; } cur += w; left -= w; }
    CloseHandle(h);
    return ok;
}

static std::wstring extractTools() {
    std::wstring base = localAppData(); if (base.empty()) return L"";
    std::wstring kp = base + L"\\KernelP"; CreateDirectoryW(kp.c_str(), nullptr);
    std::wstring dir = kp + L"\\runtime"; CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring marker = dir + L"\\.version";
    if (fileExists(dir + L"\\ffmpeg.exe") && readTextFile(marker) == TOOLS_VERSION)
        return dir;
    for (const auto& t : kTools) {
        const BYTE* d; DWORD n;
        if (loadRes(t.id, d, n)) writeFileBytes(dir + L"\\" + t.name, d, n);
    }
    std::string ver = toUtf8(TOOLS_VERSION);
    writeFileBytes(marker, (const BYTE*)ver.data(), (DWORD)ver.size());
    return dir;
}

static void initToolsAndDetect() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L"\\/"));
    if (fileExists(dir + L"\\bin\\ffmpeg.exe"))
        kp::setToolsDir(dir + L"\\bin");
    else {
        std::wstring t = extractTools();
        if (!t.empty()) kp::setToolsDir(t);
    }
    g_caps = g_engine.detect();
    g_capsReady.store(true);
    sendCaps();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"KernelPWindow";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_KERNELP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    RegisterClassW(&wc);

    int W = 1180, H = 760;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(0, L"KernelPWindow", L"KernelP",
        WS_OVERLAPPEDWINDOW, (sx - W) / 2, (sy - H) / 2, W, H,
        nullptr, nullptr, hInst, nullptr);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 , &dark, sizeof(dark));
    COLORREF blk = 0x00000000;
    DwmSetWindowAttribute(g_hwnd, 35 , &blk, sizeof(blk));
    DwmSetWindowAttribute(g_hwnd, 34 , &blk, sizeof(blk));

    HICON icoBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_KERNELP), IMAGE_ICON, 32, 32, 0);
    HICON icoSm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_KERNELP), IMAGE_ICON, 16, 16, 0);
    if (icoBig) SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)icoBig);
    if (icoSm)  SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icoSm);

    DragAcceptFiles(g_hwnd, TRUE);
    ShowWindow(g_hwnd, nCmd);
    UpdateWindow(g_hwnd);

    createWebView();

    std::thread(initToolsAndDetect).detach();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
