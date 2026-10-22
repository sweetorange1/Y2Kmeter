// ==========================================================
// LinuxMonitorCapture.cpp
//   通过 PipeWire / PulseAudio 命令行工具采集默认输出设备的 monitor 源。
//
//   思路：
//     · 目标设备：默认 sink 的 "<sink>.monitor" 源。解析顺序见
//       resolvePipeWireMonitor() / resolvePulseMonitor()。
//     · 采集进程：pw-record（优先）或 parec，fork/exec 后从 stdout 管道
//       读取裸 f32le / 48000 / 2ch 交错数据。
//     · 采集线程：按 480 帧（10ms，3840 字节）一块读满，拆成 L/R 平面
//       float 后回调；半包会跨多次 read() 累积，不足一块不会回调。
//     · 退出：子进程退出/管道 EOF/read 错误 → 记录 lastError（带 stderr
//       尾部文本），回收子进程并关闭所有 fd。stop() 发 SIGTERM，超时
//       再 SIGKILL，最后 join 线程。
//
//   注意：pw-record 写 stdout 时若不指定容器，某些版本会按 AU 写一个
//   24 字节头，破坏 f32 对齐。因此显式加 "--container raw" 保证纯裸流。
// ==========================================================

#include "LinuxMonitorCapture.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <set>
#include <utility>
#include <vector>

namespace y2k
{

namespace
{
    constexpr int    kSampleRate  = 48000;
    constexpr int    kChannels    = 2;
    constexpr int    kBlockFrames = 480;                                  // 10 ms
    constexpr size_t kBytesPerFrame = sizeof (float) * (size_t) kChannels; // 8
    constexpr size_t kBlockBytes = (size_t) kBlockFrames * kBytesPerFrame; // 3840
    constexpr int    kStartTimeoutMs = 1500;   // start() 等待首个音频块的上限
    constexpr int    kStopGraceMs    = 500;    // SIGTERM → SIGKILL 的宽限
    constexpr int    kCommandTimeoutMs = 2000; // 设备解析命令的超时
    constexpr size_t kStderrTailBytes = 4096;
    constexpr size_t kMaxCapturedBytes = 8u * 1024u * 1024u; // pw-dump 输出上限
} // namespace

namespace detail
{
    using Clock = std::chrono::steady_clock;

    // ---------------- 通用小工具 ----------------

    std::string trimCopy (std::string s)
    {
        auto notSpace = [] (char c) { return ! std::isspace ((unsigned char) c); };
        s.erase (s.begin(), std::find_if (s.begin(), s.end(), notSpace));
        s.erase (std::find_if (s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    bool isRegularExecutable (const std::string& path)
    {
        struct stat st {};
        return ::stat (path.c_str(), &st) == 0
            && S_ISREG (st.st_mode)
            && ::access (path.c_str(), X_OK) == 0;
    }

    // 在 PATH（以及几个常见目录）里找可执行文件，返回绝对路径；找不到返回空串。
    // 返回绝对路径后，后续 fork/exec 用 execv，避免依赖子进程的 PATH。
    std::string findExecutable (const std::string& name)
    {
        if (name.empty())
            return {};

        if (name.find ('/') != std::string::npos)
            return isRegularExecutable (name) ? name : std::string {};

        std::vector<std::string> dirs;

        if (const char* pathEnv = std::getenv ("PATH"))
        {
            const std::string pathStr (pathEnv);
            size_t begin = 0;
            while (begin <= pathStr.size())
            {
                const size_t end = pathStr.find (':', begin);
                const std::string dir = pathStr.substr (begin,
                                                        end == std::string::npos ? std::string::npos
                                                                                 : end - begin);
                if (! dir.empty())
                    dirs.push_back (dir);
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
        }

        // GUI 进程的 PATH 可能很精简，补几个标准目录兜底。
        for (const char* d : { "/usr/local/bin", "/usr/bin", "/bin",
                               "/usr/local/sbin", "/usr/sbin", "/sbin" })
            dirs.emplace_back (d);

        for (const auto& dir : dirs)
        {
            const std::string candidate = (dir.back() == '/' ? dir : dir + "/") + name;
            if (isRegularExecutable (candidate))
                return candidate;
        }
        return {};
    }

    // 简单作用域清理器（用于让采集线程所有退出路径都复位 running/threadExited）
    struct Finally
    {
        std::function<void()> fn;
        explicit Finally (std::function<void()> f) : fn (std::move (f)) {}
        ~Finally() { if (fn) fn(); }
        Finally (const Finally&) = delete;
        Finally& operator= (const Finally&) = delete;
    };

    int pollTimeoutMs (Clock::time_point deadline, int capMs = 100)
    {
        const auto remain = std::chrono::duration_cast<std::chrono::milliseconds> (deadline - Clock::now()).count();
        if (remain <= 0)
            return 0;
        return (int) std::min<long long> (remain, capMs);
    }

    // ---------------- 执行查询命令并捕获 stdout ----------------

    bool runCommand (const std::vector<std::string>& argv, std::string& output)
    {
        output.clear();
        if (argv.empty() || argv[0].empty())
            return false;

        int pipeFds[2] = { -1, -1 };
        if (::pipe (pipeFds) != 0)
            return false;

        ::fcntl (pipeFds[0], F_SETFD, ::fcntl (pipeFds[0], F_GETFD) | FD_CLOEXEC);
        ::fcntl (pipeFds[1], F_SETFD, ::fcntl (pipeFds[1], F_GETFD) | FD_CLOEXEC);

        const pid_t pid = ::fork();
        if (pid < 0)
        {
            ::close (pipeFds[0]);
            ::close (pipeFds[1]);
            return false;
        }

        if (pid == 0)
        {
            // 子进程：stdin/stderr → /dev/null，stdout → 管道
            const int devNull = ::open ("/dev/null", O_RDWR);
            if (devNull >= 0)
            {
                ::dup2 (devNull, STDIN_FILENO);
                ::dup2 (devNull, STDERR_FILENO);
            }
            ::dup2 (pipeFds[1], STDOUT_FILENO);
            ::close (pipeFds[0]);
            ::close (pipeFds[1]);
            if (devNull > STDERR_FILENO)
                ::close (devNull);

            std::vector<char*> cargv;
            cargv.reserve (argv.size() + 1);
            for (const auto& a : argv)
                cargv.push_back (const_cast<char*> (a.c_str()));
            cargv.push_back (nullptr);

            ::execv (argv[0].c_str(), cargv.data());
            _exit (127); // exec 失败
        }

        ::close (pipeFds[1]);
        ::fcntl (pipeFds[0], F_SETFL, ::fcntl (pipeFds[0], F_GETFL) | O_NONBLOCK);

        const auto deadline = Clock::now() + std::chrono::milliseconds (kCommandTimeoutMs);
        bool timedOut = false;
        char buf[4096];

        for (;;)
        {
            struct pollfd pfd {};
            pfd.fd = pipeFds[0];
            pfd.events = POLLIN;

            const int pr = ::poll (&pfd, 1, pollTimeoutMs (deadline));
            if (pr < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (pr == 0)
            {
                if (Clock::now() >= deadline)
                {
                    timedOut = true;
                    break;
                }
                continue;
            }

            const ssize_t n = ::read (pipeFds[0], buf, sizeof buf);
            if (n > 0)
            {
                // pw-dump 可能很大：只保留前 8 MB，但继续把管道读空，
                // 防止子进程因管道写满而卡住（超时仍由 deadline 兜底）。
                if (output.size() < kMaxCapturedBytes)
                {
                    const size_t take = std::min ((size_t) n, kMaxCapturedBytes - output.size());
                    output.append (buf, take);
                }
            }
            else if (n == 0)
            {
                break; // EOF
            }
            else if (errno == EINTR)
            {
                continue;
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (Clock::now() >= deadline)
                {
                    timedOut = true;
                    break;
                }
                continue;
            }
            else
            {
                break;
            }
        }

        ::close (pipeFds[0]);

        if (timedOut)
            ::kill (pid, SIGKILL);

        int status = 0;
        pid_t waited = -1;
        do
        {
            waited = ::waitpid (pid, &status, 0);
        } while (waited < 0 && errno == EINTR);

        if (timedOut || waited != pid)
            return false;

        return WIFEXITED (status) && WEXITSTATUS (status) == 0;
    }

    // ---------------- 设备名解析 ----------------

    // 从 JSON 片段里取 "field": "value" 的 value。
    std::string extractJsonStringField (const std::string& text, size_t from, const char* field)
    {
        const std::string key = std::string ("\"") + field + "\"";
        const size_t k = text.find (key, from);
        if (k == std::string::npos)
            return {};

        const size_t colon = text.find (':', k + key.size());
        if (colon == std::string::npos)
            return {};

        const size_t quote = text.find ('"', colon + 1);
        if (quote == std::string::npos)
            return {};

        // 冒号与左引号之间只允许空白
        for (size_t i = colon + 1; i < quote; ++i)
            if (! std::isspace ((unsigned char) text[i]))
                return {};

        const size_t endQuote = text.find ('"', quote + 1);
        if (endQuote == std::string::npos)
            return {};

        return text.substr (quote + 1, endQuote - quote - 1);
    }

    // wpctl inspect @DEFAULT_AUDIO_SINK@ 输出里取 node.name（跳过 node.description）
    std::string parseNodeNameFromWpctl (const std::string& text)
    {
        const std::string key = "node.name";
        size_t pos = 0;
        while ((pos = text.find (key, pos)) != std::string::npos)
        {
            const size_t after = pos + key.size();
            // 确保是完整 token，不是 node.name.xxx 之类
            if (after < text.size())
            {
                const unsigned char c = (unsigned char) text[after];
                if (std::isalnum (c) || c == '_' || c == '.' || c == '-')
                {
                    pos = after;
                    continue;
                }
            }

            const size_t eq = text.find ('=', after);
            if (eq == std::string::npos)
                return {};

            const size_t q1 = text.find ('"', eq + 1);
            if (q1 == std::string::npos)
                return {};
            const size_t q2 = text.find ('"', q1 + 1);
            if (q2 == std::string::npos)
                return {};

            return text.substr (q1 + 1, q2 - q1 - 1);
        }
        return {};
    }

    // pw-metadata -n default 0 或 pw-dump 里解析 default.audio.sink 的 name。
    // 注意：不能匹配 default.configured.audio.sink（子串不同）。
    std::string parseDefaultAudioSinkName (const std::string& text)
    {
        const size_t keyPos = text.find ("default.audio.sink");
        if (keyPos == std::string::npos)
            return {};
        return extractJsonStringField (text, keyPos, "name");
    }

    std::string sanitizeNodeName (const std::string& raw)
    {
        const std::string name = trimCopy (raw);
        if (name.empty() || name.size() > 512)
            return {};
        if (name.find ('\n') != std::string::npos || name.find ('\r') != std::string::npos
            || name.find ('\t') != std::string::npos)
            return {};
        return name;
    }

    // 解析默认 sink 的 **node name**（不是 <sink>.monitor）。
    //   order: pactl get-default-sink → wpctl inspect → pw-metadata → pw-dump
    //   shouldStop 用于在逐级兜底时尽快响应 stop()，避免长时间卡在查询命令上。
    bool resolvePipeWireSink (std::string& sinkName, std::string& error,
                              const std::atomic<bool>& shouldStop)
    {
        sinkName.clear();
        std::string out;

        const auto cancelled = [&shouldStop]
        {
            return shouldStop.load (std::memory_order_acquire);
        };

        if (cancelled())
            return false;

        const std::string pactl = findExecutable ("pactl");
        if (! pactl.empty() && runCommand ({ pactl, "get-default-sink" }, out))
        {
            const std::string sink = sanitizeNodeName (out);
            if (! sink.empty())
            {
                sinkName = sink;
                return true;
            }
        }

        if (cancelled())
            return false;

        const std::string wpctl = findExecutable ("wpctl");
        if (! wpctl.empty() && runCommand ({ wpctl, "inspect", "@DEFAULT_AUDIO_SINK@" }, out))
        {
            const std::string name = sanitizeNodeName (parseNodeNameFromWpctl (out));
            if (! name.empty())
            {
                sinkName = name;
                return true;
            }
        }

        if (cancelled())
            return false;

        const std::string pwMetadata = findExecutable ("pw-metadata");
        if (! pwMetadata.empty() && runCommand ({ pwMetadata, "-n", "default", "0" }, out))
        {
            const std::string name = sanitizeNodeName (parseDefaultAudioSinkName (out));
            if (! name.empty())
            {
                sinkName = name;
                return true;
            }
        }

        if (cancelled())
            return false;

        const std::string pwDump = findExecutable ("pw-dump");
        if (! pwDump.empty() && runCommand ({ pwDump }, out))
        {
            const std::string name = sanitizeNodeName (parseDefaultAudioSinkName (out));
            if (! name.empty())
            {
                sinkName = name;
                return true;
            }
        }

        error = "Could not determine the default PipeWire sink "
                "(pactl get-default-sink, wpctl inspect @DEFAULT_AUDIO_SINK@, "
                "pw-metadata and pw-dump all failed).";
        return false;
    }

    // PulseAudio 路径：pactl 取默认 sink，失败则交给 parec 的 @DEFAULT_MONITOR@。
    bool resolvePulseMonitor (std::string& monitor, const std::atomic<bool>& shouldStop)
    {
        monitor.clear();
        std::string out;

        if (! shouldStop.load (std::memory_order_acquire))
        {
            const std::string pactl = findExecutable ("pactl");
            if (! pactl.empty() && runCommand ({ pactl, "get-default-sink" }, out))
            {
                const std::string sink = sanitizeNodeName (out);
                if (! sink.empty())
                {
                    monitor = sink + ".monitor";
                    return true;
                }
            }
        }

        if (shouldStop.load (std::memory_order_acquire))
            return false;

        monitor = "@DEFAULT_MONITOR@";
        return true;
    }

    // ---------------- 采集目标校验 ----------------
    // 为什么要校验：
    //   `pw-record --target <name>` 在目标名字解析失败时会**静默回退到默认
    //   source（麦克风）**，不报任何错。旧实现把 "<sink>.monitor" 当 target
    //   传进去，但 monitor 不是原生 PipeWire node（只是 pulse 兼容层虚拟源），
    //   于是一直在偷偷录麦克风。现在改为 target=<sink node name> +
    //   `-P stream.capture.sink=true`（PipeWire 正确抓取 sink monitor 的方式），
    //   并在首个音频块到达时用 pactl 交叉验证实际绑定的 source。
    bool listPactlShort (const std::string& object,
                         std::vector<std::vector<std::string>>& rows)
    {
        rows.clear();
        const std::string pactl = findExecutable ("pactl");
        if (pactl.empty())
            return false;

        std::string out;
        if (! runCommand ({ pactl, "list", "short", object }, out))
            return false;

        auto pushLine = [&rows] (const std::string& line)
        {
            if (line.empty())
                return;
            std::vector<std::string> fields;
            std::string cur;
            for (char c : line)
            {
                if (c == '\t') { fields.push_back (cur); cur.clear(); }
                else            { cur.push_back (c); }
            }
            fields.push_back (cur);
            rows.push_back (std::move (fields));
        };

        std::string line;
        for (char c : out)
        {
            if (c == '\n') { pushLine (line); line.clear(); }
            else            { line.push_back (c); }
        }
        pushLine (line);
        return true;
    }

    int parseIntField (const std::string& s)
    {
        if (s.empty() || s.size() > 9)
            return -1;
        for (char c : s)
            if (c < '0' || c > '9')
                return -1;
        return std::atoi (s.c_str());
    }

    // pulse 源名字 → source index；找不到返回 -1（-1 时后续校验会跳过）。
    int lookupMonitorSourceIndex (const std::string& monitorName)
    {
        std::vector<std::vector<std::string>> rows;
        if (! listPactlShort ("sources", rows))
            return -1;

        for (const auto& f : rows)
            if (f.size() >= 2 && f[1] == monitorName)
                return parseIntField (f[0]);

        return -1;
    }

    std::set<int> snapshotSourceOutputIndices()
    {
        std::set<int> ids;
        std::vector<std::vector<std::string>> rows;
        if (! listPactlShort ("source-outputs", rows))
            return ids;

        for (const auto& f : rows)
        {
            const int id = parseIntField (f.empty() ? std::string() : f[0]);
            if (id >= 0)
                ids.insert (id);
        }
        return ids;
    }

    // 返回值：1 = 绑定到 monitor；0 = 绑定到了别的源（写 error）；-1 = 无法校验。
    int verifyCaptureBoundToMonitor (const std::set<int>& before,
                                     int monitorSourceIndex,
                                     std::string& error)
    {
        if (monitorSourceIndex < 0)
            return -1;

        std::vector<std::vector<std::string>> rows;
        if (! listPactlShort ("source-outputs", rows))
            return -1;

        for (const auto& f : rows)
        {
            if (f.size() < 2)
                continue;

            const int id = parseIntField (f[0]);
            if (id < 0 || before.count (id) != 0)
                continue;

            const int sourceIndex = parseIntField (f[1]);
            if (sourceIndex == monitorSourceIndex)
                return 1;

            error = "capture bound to source index " + std::to_string (sourceIndex)
                  + " instead of the sink monitor (index "
                  + std::to_string (monitorSourceIndex) + ")";
            return 0;
        }

        return -1;
    }

    // ---------------- 子进程启动/回收 ----------------

    bool spawnCaptureProcess (const std::vector<std::string>& argv,
                              int& audioFdOut,
                              int& stderrFdOut,
                              pid_t& pidOut,
                              std::string& error)
    {
        audioFdOut = -1;
        stderrFdOut = -1;
        pidOut = -1;

        if (argv.empty() || argv[0].empty())
        {
            error = "Internal error: empty capture command.";
            return false;
        }

        int audioPipe[2] = { -1, -1 };
        int errPipe[2]   = { -1, -1 };

        if (::pipe (audioPipe) != 0)
        {
            error = std::string ("pipe() failed: ") + std::strerror (errno);
            return false;
        }
        if (::pipe (errPipe) != 0)
        {
            error = std::string ("pipe() failed: ") + std::strerror (errno);
            ::close (audioPipe[0]);
            ::close (audioPipe[1]);
            return false;
        }

        ::fcntl (audioPipe[0], F_SETFD, ::fcntl (audioPipe[0], F_GETFD) | FD_CLOEXEC);
        ::fcntl (errPipe[0],   F_SETFD, ::fcntl (errPipe[0],   F_GETFD) | FD_CLOEXEC);

        const pid_t pid = ::fork();
        if (pid < 0)
        {
            error = std::string ("fork() failed: ") + std::strerror (errno);
            ::close (audioPipe[0]);
            ::close (audioPipe[1]);
            ::close (errPipe[0]);
            ::close (errPipe[1]);
            return false;
        }

        if (pid == 0)
        {
            // ---- 子进程 ----
            const int devNull = ::open ("/dev/null", O_RDWR);
            if (devNull >= 0)
                ::dup2 (devNull, STDIN_FILENO);

            ::dup2 (audioPipe[1], STDOUT_FILENO);
            ::dup2 (errPipe[1],   STDERR_FILENO);

            ::close (audioPipe[0]);
            ::close (audioPipe[1]);
            ::close (errPipe[0]);
            ::close (errPipe[1]);
            if (devNull > STDERR_FILENO)
                ::close (devNull);

            // 父进程若提前关闭管道，写端得到 EPIPE 而不是被 SIGPIPE 杀掉。
            ::signal (SIGPIPE, SIG_IGN);

            std::vector<char*> cargv;
            cargv.reserve (argv.size() + 1);
            for (const auto& a : argv)
                cargv.push_back (const_cast<char*> (a.c_str()));
            cargv.push_back (nullptr);

            ::execv (argv[0].c_str(), cargv.data());

            char msg[512];
            const int len = std::snprintf (msg, sizeof msg,
                                           "failed to exec %s: %s\n",
                                           argv[0].c_str(), std::strerror (errno));
            if (len > 0)
            {
                const ssize_t ignored = ::write (STDERR_FILENO, msg,
                                                 std::min ((size_t) len, sizeof msg - 1));
                (void) ignored;
            }
            _exit (127);
        }

        // ---- 父进程 ----
        ::close (audioPipe[1]);
        ::close (errPipe[1]);

        ::fcntl (audioPipe[0], F_SETFL, ::fcntl (audioPipe[0], F_GETFL) | O_NONBLOCK);
        ::fcntl (errPipe[0],   F_SETFL, ::fcntl (errPipe[0],   F_GETFL) | O_NONBLOCK);

        audioFdOut  = audioPipe[0];
        stderrFdOut = errPipe[0];
        pidOut      = pid;
        return true;
    }

    // 把 stderr 管道内容读空（只保留尾部若干字节）。EOF 时关闭 fd 并置 -1。
    void drainStderr (int& fd, std::string& tail)
    {
        if (fd < 0)
            return;

        char buf[1024];
        for (;;)
        {
            const ssize_t n = ::read (fd, buf, sizeof buf);
            if (n > 0)
            {
                tail.append (buf, (size_t) n);
                if (tail.size() > kStderrTailBytes)
                    tail.erase (0, tail.size() - kStderrTailBytes);
            }
            else if (n == 0)
            {
                ::close (fd);
                fd = -1;
                return;
            }
            else if (errno == EINTR)
            {
                continue;
            }
            else
            {
                return; // EAGAIN/EWOULDBLOCK，等下次 poll
            }
        }
    }

    bool reapChild (pid_t pid, int& statusOut, int graceMs)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds (graceMs);

        for (;;)
        {
            const pid_t r = ::waitpid (pid, &statusOut, WNOHANG);
            if (r == pid)
                return true;
            if (r < 0 && errno != EINTR)
                return false; // ECHILD 等
            if (Clock::now() >= deadline)
                break;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }

        ::kill (pid, SIGKILL);
        for (;;)
        {
            const pid_t r = ::waitpid (pid, &statusOut, 0);
            if (r == pid)
                return true;
            if (r < 0 && errno == EINTR)
                continue;
            return false;
        }
    }

    std::string lastNonEmptyLine (const std::string& text)
    {
        size_t end = text.size();
        while (end > 0)
        {
            size_t begin = text.rfind ('\n', end - 1);
            if (begin == std::string::npos)
                begin = 0;
            else
                ++begin;

            const std::string line = trimCopy (text.substr (begin, end - begin));
            if (! line.empty())
                return line;

            if (begin == 0)
                break;
            end = begin - 1;
        }
        return {};
    }

    std::string formatChildExitReason (const std::string& program, int status,
                                       const std::string& stderrText)
    {
        std::string reason;

        if (WIFEXITED (status))
            reason = program + " exited (code " + std::to_string (WEXITSTATUS (status)) + ")";
        else if (WIFSIGNALED (status))
            reason = program + " terminated by signal " + std::to_string (WTERMSIG (status));
        else
            reason = program + " stopped unexpectedly";

        const std::string tail = lastNonEmptyLine (stderrText);
        if (! tail.empty())
            reason += ": " + tail;

        return reason;
    }

    inline float readFloatLE (const unsigned char* p) noexcept
    {
        // pw-record/parec 输出的 f32 为本机字节序；x86/ARM 均为小端。
        float v = 0.0f;
        std::memcpy (&v, p, sizeof v);
        return v;
    }
} // namespace detail

// ==========================================================
// LinuxMonitorCapture
// ==========================================================

LinuxMonitorCapture::LinuxMonitorCapture() = default;

LinuxMonitorCapture::~LinuxMonitorCapture()
{
    stop();
}

bool LinuxMonitorCapture::isSupported()
{
    return ! detail::findExecutable ("pw-record").empty()
        || ! detail::findExecutable ("parec").empty();
}

bool LinuxMonitorCapture::isRunning() const noexcept
{
    return running.load (std::memory_order_acquire);
}

const std::string& LinuxMonitorCapture::getLastError() const noexcept
{
    return lastError;
}

bool LinuxMonitorCapture::start()
{
    // 已在运行（或上次线程对象还在）→ 先干净地停掉
    if (captureThread != nullptr || running.load (std::memory_order_acquire))
        stop();

    lastError.clear();
    shouldStop.store (false, std::memory_order_release);
    threadExited.store (false, std::memory_order_release);
    childPid.store (-1, std::memory_order_release);

    if (! isSupported())
    {
        lastError = "Neither pw-record nor parec was found in PATH; "
                    "a PipeWire or PulseAudio installation is required.";
        threadExited.store (true, std::memory_order_release);
        return false;
    }

    captureThread = std::make_unique<std::thread> ([this] { runCaptureThread(); });

    // 等采集线程拿到第一个音频块（此时 running=true），或确认它失败退出。
    const auto deadline = detail::Clock::now()
                        + std::chrono::milliseconds (kStartTimeoutMs);

    for (;;)
    {
        if (running.load (std::memory_order_acquire))
            return true;

        if (threadExited.load (std::memory_order_acquire))
        {
            stop(); // join + 回收
            return false;
        }

        if (detail::Clock::now() >= deadline)
            break;

        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    // 超时：线程可能还卡在设备解析或等数据，stop() 负责发 SIGTERM/join。
    stop();

    if (lastError.empty())
        lastError = "Linux monitor capture did not produce any audio within "
                    + std::to_string (kStartTimeoutMs)
                    + " ms (pw-record/parec could not open the default sink monitor).";

    return false;
}

void LinuxMonitorCapture::stop()
{
    shouldStop.store (true, std::memory_order_release);

    // 先温和地请子进程退出；采集线程随后会读到 EOF 并完成清理。
    const int pid = childPid.load (std::memory_order_acquire);
    if (pid > 0)
        ::kill ((pid_t) pid, SIGTERM);

    if (captureThread == nullptr)
    {
        running.store (false, std::memory_order_release);
        return;
    }

    // 从 onAudio 回调里调用 stop()：不能 join 自己，交给线程自己退出。
    if (captureThread->get_id() == std::this_thread::get_id())
        return;

    const auto deadline = detail::Clock::now()
                        + std::chrono::milliseconds (kStopGraceMs);

    while (! threadExited.load (std::memory_order_acquire)
           && detail::Clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));

    if (! threadExited.load (std::memory_order_acquire))
    {
        const int pidToKill = childPid.load (std::memory_order_acquire);
        if (pidToKill > 0)
            ::kill ((pid_t) pidToKill, SIGKILL);
    }

    if (captureThread->joinable())
        captureThread->join();

    captureThread.reset();
    running.store (false, std::memory_order_release);
    childPid.store (-1, std::memory_order_release);
}

void LinuxMonitorCapture::runCaptureThread()
{
    detail::Finally finally ([this]()
    {
        running.store (false, std::memory_order_release);
        threadExited.store (true, std::memory_order_release);
    });

    if (shouldStop.load (std::memory_order_acquire))
        return;

    // ---- 1. 找可执行文件 + 解析目标 monitor 源 ----
    const std::string pwRecordPath = detail::findExecutable ("pw-record");
    const std::string parecPath    = detail::findExecutable ("parec");

    std::string program;                 // 仅用于错误信息
    std::vector<std::string> argv;

    std::string verifyMonitorName;   // 用于启动后校验实际绑定的 monitor 源

    if (! pwRecordPath.empty())
    {
        std::string sinkName;
        std::string resolveError;
        if (! detail::resolvePipeWireSink (sinkName, resolveError,
                                           shouldStop))
        {
            if (! shouldStop.load (std::memory_order_acquire))
                lastError = resolveError.empty()
                              ? "Could not determine the default PipeWire sink."
                              : resolveError;
            return;
        }

        // ★ 关键：target 必须是 sink 的 node name（原生节点），并显式要求
        //   capture sink。不能传 "<sink>.monitor" —— 那只是 pulse 兼容层的
        //   虚拟源名字，pw-record 解析不到会静默回退到默认麦克风。
        verifyMonitorName = sinkName + ".monitor";

        program = "pw-record";
        argv = { pwRecordPath,
                 "--target", sinkName,
                 "-P", "stream.capture.sink=true",
                 "--format", "f32",
                 "--rate", "48000",
                 "--channels", "2",
                 "--container", "raw",
                 "-" };
    }
    else if (! parecPath.empty())
    {
        std::string monitor;
        if (! detail::resolvePulseMonitor (monitor, shouldStop))
        {
            if (! shouldStop.load (std::memory_order_acquire))
                lastError = "Could not determine the default PulseAudio monitor.";
            return;
        }

        program = "parec";
        argv = { parecPath,
                 "--device=" + monitor,
                 "--format=float32le",
                 "--rate=48000",
                 "--channels=2",
                 "--raw" };
    }
    else
    {
        lastError = "Neither pw-record nor parec was found in PATH; "
                    "a PipeWire or PulseAudio installation is required.";
        return;
    }

    if (shouldStop.load (std::memory_order_acquire))
        return;

    // ---- 2. 启动子进程 ----
    //   启动前记录已有 source-output 快照 + monitor 的 pulse 源 index，
    //   首个音频块到达后用 pactl 交叉校验实际绑定目标（防静默回退麦克风）。
    const int monitorSourceIndex = verifyMonitorName.empty()
                                     ? -1
                                     : detail::lookupMonitorSourceIndex (verifyMonitorName);
    const auto sourceOutputsBefore = detail::snapshotSourceOutputIndices();

    int audioFd = -1;
    int stderrFd = -1;
    pid_t child = -1;
    std::string spawnError;

    if (! detail::spawnCaptureProcess (argv, audioFd, stderrFd, child, spawnError))
    {
        lastError = spawnError;
        return;
    }

    childPid.store ((int) child, std::memory_order_release);

    // ---- 3. 读循环：10ms 一块，半包累积 ----
    std::vector<unsigned char> rawBlock (kBlockBytes);
    std::vector<float> left (kBlockFrames);
    std::vector<float> right (kBlockFrames);
    size_t filled = 0;

    std::string stderrTail;
    bool sawEof = false;
    bool readFailed = false;
    std::string readError;
    bool firstBlockDelivered = false;

    while (! shouldStop.load (std::memory_order_acquire))
    {
        struct pollfd fds[2] {};
        fds[0].fd = audioFd;
        fds[0].events = POLLIN;
        fds[1].fd = stderrFd;
        fds[1].events = POLLIN;

        const int pr = ::poll (fds, 2, 100);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            readFailed = true;
            readError = std::string ("poll() failed: ") + std::strerror (errno);
            break;
        }
        if (pr == 0)
            continue;

        if (fds[1].revents != 0)
            detail::drainStderr (stderrFd, stderrTail);

        if (fds[0].revents != 0)
        {
            const ssize_t n = ::read (audioFd, rawBlock.data() + filled, kBlockBytes - filled);

            if (n > 0)
            {
                filled += (size_t) n;
                if (filled == kBlockBytes)
                {
                    for (int f = 0; f < kBlockFrames; ++f)
                    {
                        const unsigned char* p = rawBlock.data() + (size_t) f * kBytesPerFrame;
                        left[(size_t) f]  = detail::readFloatLE (p);
                        right[(size_t) f] = detail::readFloatLE (p + sizeof (float));
                    }

                    if (! firstBlockDelivered)
                    {
                        // 校验采集目标：如果 pw-record 回退到了麦克风，宁可报错
                        // 也不能把麦克风静默当成"系统输出"喂给分析链路。
                        std::string verifyError;
                        const int ok = detail::verifyCaptureBoundToMonitor (
                                            sourceOutputsBefore, monitorSourceIndex,
                                            verifyError);

                        if (ok == 0)
                        {
                            lastError = "System output capture mis-targeted: "
                                      + verifyError
                                      + ". Check PipeWire >= 0.3.x and retry.";
                            break;
                        }

                        firstBlockDelivered = true;
                        running.store (true, std::memory_order_release);
                    }

                    if (onAudio)
                        onAudio (left.data(), right.data(), kBlockFrames, (double) kSampleRate);

                    filled = 0;
                }
            }
            else if (n == 0)
            {
                sawEof = true;
                break;
            }
            else if (errno == EINTR)
            {
                continue;
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            else
            {
                readFailed = true;
                readError = std::string ("read() failed: ") + std::strerror (errno);
                break;
            }
        }
    }

    // ---- 4. 收尾：结束子进程、回收、关 fd ----
    if (! sawEof && child > 0)
        ::kill (child, SIGTERM);

    int status = 0;
    const bool reaped = (child > 0) && detail::reapChild (child, status, kStopGraceMs);

    // 子进程退出后把 stderr 里剩余内容读干净
    detail::drainStderr (stderrFd, stderrTail);

    if (stderrFd >= 0)
    {
        ::close (stderrFd);
        stderrFd = -1;
    }
    if (audioFd >= 0)
    {
        ::close (audioFd);
        audioFd = -1;
    }

    childPid.store (-1, std::memory_order_release);

    // ---- 5. 记录退出原因（主动 stop() 不算错误）----
    if (! shouldStop.load (std::memory_order_acquire))
    {
        if (readFailed)
            lastError = readError;
        else if (! reaped)
            lastError = program + " could not be reaped (waitpid failed).";
        else
            lastError = detail::formatChildExitReason (program, status, stderrTail);
    }
}

} // namespace y2k
