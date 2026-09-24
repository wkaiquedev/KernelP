#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <windows.h>

namespace kp {

void setToolsDir(const std::wstring& dir);

struct MediaInfo {
    bool ok = false;
    std::wstring path;
    std::string  name;
    long long    sizeBytes = 0;
    double       durationSec = 0;
    int          width = 0;
    int          height = 0;
    std::string  vcodec;
    std::string  acodec;
    std::string  error;
};

struct Caps {
    bool nvencHevc = false;
    bool nvencAv1  = false;
    bool qsvHevc   = false;
    bool x265      = true;
    std::string gpu;
    std::string primaryLabel;
    std::string primaryCodec;
};

struct Job {
    std::wstring input;
    std::wstring output;
    int          reductionPercent = 90;
    std::string  codec = "hevc";
    std::string  speed = "balanced";
};

struct Progress {
    double    percent = 0;
    double    fps = 0;
    double    speed = 0;
    double    etaSec = 0;
    long long outBytes = 0;
};

struct Result {
    bool         ok = false;
    std::wstring outPath;
    std::string  outName;
    long long    inBytes = 0;
    long long    outBytes = 0;
    double       ratioPercent = 0;
    double       elapsedSec = 0;
    std::string  error;
};

class Engine {
public:
    using ProgressFn = std::function<void(const Progress&)>;
    using DoneFn     = std::function<void(const Result&)>;
    using LogFn      = std::function<void(const std::string&)>;

    Engine();
    ~Engine();

    Caps detect();

    MediaInfo probe(const std::wstring& path);

    void start(const Job& job, ProgressFn onProgress, DoneFn onDone, LogFn onLog);

    void cancel();

    bool running() const { return running_.load(); }

private:
    void worker(Job job, ProgressFn onProgress, DoneFn onDone, LogFn onLog);

    std::thread          thread_;
    std::atomic<bool>    running_{false};
    std::atomic<bool>    cancel_{false};
    std::atomic<HANDLE>  childProcess_{nullptr};
    Caps                 caps_;
};

}
