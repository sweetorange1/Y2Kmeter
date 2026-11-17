// ==========================================================
// WasapiLoopbackCapture.cpp
//   使用 WASAPI 做回环采集，两种目标（见头文件）：
//
//   A) 整机输出 —— "默认渲染端点（扬声器/耳机）" 的混音输出信号
//     - IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)
//     - IAudioClient::Initialize(AUDCLNT_SHAREMODE_SHARED,
//                                AUDCLNT_STREAMFLAGS_LOOPBACK
//                              | AUDCLNT_STREAMFLAGS_EVENTCALLBACK? (这里用轮询，更简单) ,
//                                hnsBufferDuration, 0, mixFormat, nullptr)
//     - IAudioCaptureClient::GetBuffer / ReleaseBuffer
//
//   B) 定向程序 —— 进程级回环（OBS "Application Audio Capture" 用的同一套 API）
//     - ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, IAudioClient,
//                                   AUDIOCLIENT_ACTIVATION_PARAMS{ PROCESS_LOOPBACK,
//                                       TargetProcessId,
//                                       PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE })
//     - 这个虚拟设备**不支持 GetMixFormat**（E_NOTIMPL），格式由我们指定，
//       系统负责转换：固定要 float32 立体声，采样率跟随默认渲染端点
//     - IAudioClient::Initialize(SHARED, LOOPBACK | EVENTCALLBACK, ...)
//       事件驱动是微软示例与 OBS 都在用的路径；我们等事件时带超时，兼作轮询兜底
//
//   混音格式一般是 WAVEFORMATEXTENSIBLE (PCM float32 / int16 / int24)；
//   为简化，这里只支持 IEEE float32 与 PCM int16/int24/int32 的 interleaved，
//   运行时转成两路平面 float32。
//
//   线程：采集在一个后台 std::thread；每次拿到 buffer → 转格式 → 调 onAudio
// ==========================================================

#include "WasapiLoopbackCapture.h"
#include "AppAudioTargets.h"

#if JUCE_WINDOWS

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mmdevapi.lib")   // ActivateAudioInterfaceAsync

namespace y2k
{

namespace
{
    // RAII 小工具：COM 初始化（STA/MTA 均可，loopback 用 MTA 更合适）
    struct ComApartment
    {
        bool ok = false;
        ComApartment()
        {
            auto hr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            ok = SUCCEEDED (hr) || hr == RPC_E_CHANGED_MODE; // 已初始化过也行
        }
        ~ComApartment() { if (ok) CoUninitialize(); }
    };

    using Microsoft::WRL::ComPtr;

    // GetMixFormat 返回的 WAVEFORMATEX（CoTaskMemAlloc 分配）
    struct CoTaskMemDeleter { void operator() (void* p) const noexcept { CoTaskMemFree (p); } };
    using MixFormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;

    juce::String hrText (HRESULT hr)
    {
        return "hr=0x" + juce::String::toHexString ((int) hr);
    }

    // 把 interleaved 字节流按设备 mix format 解码为两路平面 float32（取前 2 ch）
    //   · IEEE float32   直拷贝 / 下混
    //   · PCM  int16     /32768.f
    //   · PCM  int24/32  按 subformat 分别转换
    // 返回实际写入的帧数
    int decodeToStereoFloat (const BYTE*       src,
                             int               framesAvailable,
                             const WAVEFORMATEX* wf,
                             std::vector<float>& L,
                             std::vector<float>& R)
    {
        L.resize ((size_t) framesAvailable);
        R.resize ((size_t) framesAvailable);

        const int    channels   = wf->nChannels;
        const int    bytesPerFr = wf->nBlockAlign;
        const int    bps        = wf->wBitsPerSample;

        // 判断样本格式：WAVE_FORMAT_EXTENSIBLE 时看 SubFormat
        bool isFloat = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (wf);
            isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }

        auto readSample = [&] (const BYTE* p) -> float
        {
            if (isFloat)
            {
                // float32
                float v;
                std::memcpy (&v, p, sizeof (float));
                return v;
            }
            // PCM
            if (bps == 16)
            {
                int16_t v;
                std::memcpy (&v, p, 2);
                return (float) v / 32768.0f;
            }
            if (bps == 24)
            {
                int32_t v = ((int32_t) (int8_t) p[2] << 16)
                          | ((int32_t) p[1] << 8)
                          |  (int32_t) p[0];
                return (float) v / 8388608.0f;
            }
            if (bps == 32)
            {
                int32_t v;
                std::memcpy (&v, p, 4);
                return (float) v / 2147483648.0f;
            }
            return 0.0f;
        };

        // 每帧 channels 个样本，取 ch0 / ch1（若 mono 则 L=R）
        for (int f = 0; f < framesAvailable; ++f)
        {
            const BYTE* frame = src + (size_t) f * (size_t) bytesPerFr;

            if (channels >= 2)
            {
                L[(size_t) f] = readSample (frame);
                R[(size_t) f] = readSample (frame + (bps / 8));
            }
            else
            {
                const float s = readSample (frame);
                L[(size_t) f] = s;
                R[(size_t) f] = s;
            }
        }

        return framesAvailable;
    }

    // ------------------------------------------------------
    // 静音补零节拍器
    //
    //   系统（或目标程序）静音时 WASAPI loopback 可能不再产生 packet
    //   （GetNextPacketSize 返回 0），onAudio 不再触发 → 分析器冻结。
    //   这里在确认静音后主动补全 0 数据，让 LoudnessMeter 等持续归零。
    //   定向程序模式下"目标程序没在运行"的等待期也用它持续输出静音。
    //
    //   ★ 补零的两个节奏细节都直接决定 Waveform 的观感：
    //     1) 补零必须是"墙钟小步"，不能一次灌一大段（旧实现每 100ms 灌 0.1s）：
    //        下游按样本数推进的模块会被脉冲带偏，画面变成"定格 → 横跳"；
    //     2) 判定窗口不能太长。旧实现要等 100ms 才开始补零 —— 这 100ms 里
    //        一个样本都不进，波形硬定格，就是"刚进入静音时顿一下"。
    //        现在窗口收到 20ms（loopback 正常 packet 间隔 ~10ms），并且窗口
    //        到期时**从最后一个真实 packet 起把整段空白一次补齐**，
    //        时间轴不丢，只剩 20ms（120px/s 下 ≈2px）的极小迟疑。
    // ------------------------------------------------------
    struct SilencePacer
    {
        using Clock = std::chrono::steady_clock;

        static constexpr double kSilenceGraceMs  = 20.0;   // 判定静音前的等待窗口
        static constexpr int    kMaxSilenceChunk = 1024;   // 单次补零上限（< 示波器 2048 快照）

        Clock::time_point lastDataTime = Clock::now();
        Clock::time_point lastTopUp    = lastDataTime;   // 已经补到的时刻
        bool              feeding      = false;          // 是否已越过判定窗口、进入补零
        double            framesOwed   = 0.0;            // 不足 1 帧的余量累积，避免长期漂移

        // 收到真实 packet 时调用
        void noteRealData()
        {
            feeding      = false;
            lastDataTime = Clock::now();
            lastTopUp    = lastDataTime;
            framesOwed   = 0.0;
        }

        // 没有 packet 时调用：返回此刻应补的静音帧数（0 = 还不用补）
        int framesDue (double sampleRate)
        {
            const auto now = Clock::now();

            if (! feeding)
            {
                // 判定窗口：正常 loopback packet 间隔约 10ms，只有明显超过它才认为
                //   引擎真的停了（窗口内不产出任何数据，避免把真实音频替换成 0）。
                const double gapMs = std::chrono::duration<double, std::milli> (now - lastDataTime).count();
                if (gapMs < kSilenceGraceMs)
                    return 0;

                // 确认静音：从"最后一个真实 packet"开始补，把窗口里的空白一起补上
                feeding   = true;
                lastTopUp = lastDataTime;
            }

            const double elapsedSec = std::chrono::duration<double> (now - lastTopUp).count();
            lastTopUp = now;

            // 单次上限 100ms：采集线程被调度延迟（睡眠 / 调试断点 / 系统卡顿）后
            //   不要一次性灌进大段数据 —— 那正是本结构要消除的脉冲。
            framesOwed += juce::jlimit (0.0, 0.1, elapsedSec) * sampleRate;

            int frames = (int) framesOwed;
            if (frames <= 0)
                return 0;

            // 单块上限 1024：始终小于示波器 2048 样本快照，任何采样率下都不会
            //   因为"一帧只能消费快照"而丢样本（丢样本 = 滚动变慢 + 跳格）。
            frames = juce::jmin (frames, kMaxSilenceChunk);
            framesOwed -= (double) frames;
            return frames;
        }
    };

    // 采集线程的工作缓冲 + 输出口（两种目标共用）
    struct CaptureSink
    {
        const WasapiLoopbackCapture::AudioCallback& onAudio;
        std::vector<float> bufL, bufR;
        SilencePacer       pacer;

        explicit CaptureSink (const WasapiLoopbackCapture::AudioCallback& cb) : onAudio (cb)
        {
            bufL.reserve (4096);
            bufR.reserve (4096);
        }

        void emitSilenceIfDue (double sampleRate)
        {
            // 一次醒来把欠下的静音全部补完（仍切成 <= 1024 帧的小块）。
            //   只补一块的话，高采样率设备（96k / 192k）下"每次醒来 1024 帧"追不上墙钟：
            //   192kHz 每 10ms 就欠 1920 帧 → 静音期间时间轴变慢、波形滚动拖沓。
            //   上限 32 块（约 170ms@192k）防止下游处理偏慢时在这里空转。
            for (int chunk = 0; chunk < 32; ++chunk)
            {
                const int frames = pacer.framesDue (sampleRate);
                if (frames <= 0)
                    return;

                bufL.assign ((size_t) frames, 0.0f);
                bufR.assign ((size_t) frames, 0.0f);
                if (onAudio)
                    onAudio (bufL.data(), bufR.data(), frames, sampleRate);
            }
        }
    };

    // 定向程序模式：盯着目标进程是否还活着
    class TargetWatch
    {
    public:
        TargetWatch (DWORD targetPid, const juce::String& targetExe)
            : pid (targetPid),
              exeName (targetExe),
              // 只为感知"目标退出"；受保护进程（反作弊 / DRM）拿不到句柄，不影响采集本身
              process (OpenProcess (SYNCHRONIZE, FALSE, targetPid))
        {
        }

        ~TargetWatch()
        {
            if (process != nullptr)
                CloseHandle (process);
        }

        bool exited()
        {
            if (process != nullptr)
                return WaitForSingleObject (process, 0) == WAIT_OBJECT_0;

            // 没有句柄：退而每 2 秒问一次系统
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPoll < std::chrono::seconds (2))
                return false;

            lastPoll = now;
            return ! appaudio::isProcessRunning ((std::uint32_t) pid, exeName);
        }

    private:
        const DWORD        pid;
        const juce::String exeName;
        const HANDLE       process;
        std::chrono::steady_clock::time_point lastPoll = std::chrono::steady_clock::now();

        JUCE_DECLARE_NON_COPYABLE (TargetWatch)
    };

    // 采集主循环：两种目标共用。返回 = stop() / 采集流失效 / 目标程序退出。
    //   dataEvent / watch 为空 = 整机输出模式（纯轮询、不关心进程死活）。
    void pumpCapture (IAudioCaptureClient*      captureClient,
                      const WAVEFORMATEX*       format,
                      HANDLE                    dataEvent,
                      TargetWatch*              watch,
                      const std::atomic<bool>&  shouldStop,
                      CaptureSink&              sink)
    {
        const double sampleRate = (double) format->nSamplesPerSec;

        // 没数据时最多等这么久，再回来补静音 / 看 stop() / 看目标是否退出
        const DWORD pollMs = 5;

        while (! shouldStop.load (std::memory_order_acquire))
        {
            UINT32 packetLength = 0;
            HRESULT hr = captureClient->GetNextPacketSize (&packetLength);
            if (FAILED (hr))
                return;   // 流失效（设备被拔 / 格式变化 / 引擎重启 ...）

            if (packetLength == 0)
            {
                if (watch != nullptr && watch->exited())
                    return;

                sink.emitSilenceIfDue (sampleRate);

                // 有事件句柄就等事件（数据一到立刻醒），否则纯 sleep 轮询
                if (dataEvent != nullptr)
                    WaitForSingleObject (dataEvent, pollMs);
                else
                    Sleep (pollMs);
                continue;
            }

            sink.pacer.noteRealData();

            while (packetLength != 0)
            {
                BYTE*   data     = nullptr;
                UINT32  frames   = 0;
                DWORD   flags    = 0;

                hr = captureClient->GetBuffer (&data, &frames, &flags, nullptr, nullptr);
                if (FAILED (hr)) { packetLength = 0; break; }

                if (frames > 0)
                {
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

                    sink.bufL.assign ((size_t) frames, 0.0f);
                    sink.bufR.assign ((size_t) frames, 0.0f);

                    if (! silent && data != nullptr)
                        decodeToStereoFloat (data, (int) frames, format, sink.bufL, sink.bufR);

                    if (sink.onAudio)
                        sink.onAudio (sink.bufL.data(), sink.bufR.data(), (int) frames, sampleRate);
                }

                captureClient->ReleaseBuffer (frames);

                hr = captureClient->GetNextPacketSize (&packetLength);
                if (FAILED (hr)) { packetLength = 0; break; }
            }
        }
    }

    // 打开"默认"渲染端点（就是系统当前外放设备）的 IAudioClient 并取它的 mix format。
    //   成功返回空串，失败返回错误描述。
    juce::String openDefaultRenderClient (ComPtr<IAudioClient>& client, MixFormatPtr& mixFormat)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS (enumerator.GetAddressOf()))))
            return "CoCreateInstance(MMDeviceEnumerator) failed";

        ComPtr<IMMDevice> device;
        if (FAILED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device)) || device == nullptr)
            return "GetDefaultAudioEndpoint(eRender) failed; no active playback device?";

        if (FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                      (void**) client.ReleaseAndGetAddressOf())))
            return "IMMDevice::Activate(IAudioClient) failed";

        WAVEFORMATEX* format = nullptr;
        if (FAILED (client->GetMixFormat (&format)) || format == nullptr)
            return "IAudioClient::GetMixFormat failed";

        mixFormat.reset (format);
        return {};
    }

    // ------------------------------------------------------
    // ActivateAudioInterfaceAsync 的完成回调（与微软 ApplicationLoopback 示例同一写法）。
    //   · FtmBase：回调在系统的工作线程上触发，对象必须是自由线程的（IAgileObject），
    //     否则 COM 会尝试跨套间封送而失败。
    //   · 激活参数（PROPVARIANT 指向的 blob）放在本对象里而不是调用方的栈上：
    //     等待超时 / 中途 stop() 时调用方会先返回，对象靠异步操作持有的引用活到回调结束。
    // ------------------------------------------------------
    class ProcessLoopbackActivation final
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                              Microsoft::WRL::FtmBase,
                                              IActivateAudioInterfaceCompletionHandler>
    {
    public:
        explicit ProcessLoopbackActivation (DWORD targetPid)
        {
            params.ActivationType                            = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            params.ProcessLoopbackParams.TargetProcessId     = targetPid;
            // 连同子进程一起采：Chrome / Electron 的声音来自子进程
            params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

            activateParams.vt             = VT_BLOB;
            activateParams.blob.cbSize    = sizeof (params);
            activateParams.blob.pBlobData = reinterpret_cast<BYTE*> (&params);
        }

        // 发起激活并等待结果。
        HRESULT activate (const std::atomic<bool>& shouldStop, ComPtr<IAudioClient>& outClient)
        {
            ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
            const HRESULT hr = ActivateAudioInterfaceAsync (VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                            __uuidof (IAudioClient),
                                                            &activateParams,
                                                            this,
                                                            &operation);
            if (FAILED (hr))
                return hr;

            // 正常几十毫秒内完成；分片等待以便 stop() 能及时打断。
            //   总上限 2.5 秒，略短于 start() 给定向程序模式的 3 秒，保证超时能报出具体原因。
            for (int slice = 0; slice < 50; ++slice)
            {
                if (completed.wait (50))
                {
                    if (SUCCEEDED (result))
                        outClient = std::move (client);
                    return result;
                }

                if (shouldStop.load (std::memory_order_acquire))
                    return E_ABORT;
            }

            return HRESULT_FROM_WIN32 (ERROR_TIMEOUT);
        }

        STDMETHODIMP ActivateCompleted (IActivateAudioInterfaceAsyncOperation* operation) override
        {
            HRESULT           activateResult = E_FAIL;
            ComPtr<IUnknown>  activated;

            HRESULT hr = operation->GetActivateResult (&activateResult, &activated);
            if (SUCCEEDED (hr))
                hr = activateResult;
            if (SUCCEEDED (hr))
                hr = (activated != nullptr) ? activated.As (&client) : E_NOINTERFACE;

            result = hr;
            completed.signal();
            return S_OK;
        }

    private:
        AUDIOCLIENT_ACTIVATION_PARAMS params {};
        PROPVARIANT                   activateParams {};
        juce::WaitableEvent           completed { true };   // 手动复位：置位后保持
        HRESULT                       result = E_FAIL;       // completed 置位前写、置位后读
        ComPtr<IAudioClient>          client;                // 同上
    };

    // 一条进程级回环采集流。open() 失败后不要复用本对象（已拿到的资源由析构回收）。
    struct ProcessStream
    {
        ComPtr<IAudioClient>        client;
        ComPtr<IAudioCaptureClient> capture;
        HANDLE                      dataEvent = nullptr;
        WAVEFORMATEXTENSIBLE        format {};

        ~ProcessStream()
        {
            capture.Reset();
            if (client != nullptr)
            {
                client->Stop();
                client.Reset();
            }
            if (dataEvent != nullptr)
                CloseHandle (dataEvent);
        }

        // 成功返回空串，失败返回错误描述
        juce::String open (DWORD pid, DWORD sampleRate, const std::atomic<bool>& shouldStop)
        {
            const auto activation = Microsoft::WRL::Make<ProcessLoopbackActivation> (pid);
            const HRESULT hrActivate = (activation != nullptr) ? activation->activate (shouldStop, client)
                                                               : E_OUTOFMEMORY;
            if (FAILED (hrActivate) || client == nullptr)
                return "ActivateAudioInterfaceAsync(process loopback) failed (" + hrText (hrActivate) + ")";

            // 进程级回环的虚拟设备没有 mix format，格式由我们定：float32 立体声
            format.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
            format.Format.nChannels            = 2;
            format.Format.nSamplesPerSec       = sampleRate;
            format.Format.wBitsPerSample       = 32;
            format.Format.nBlockAlign          = (WORD) (format.Format.nChannels * sizeof (float));
            format.Format.nAvgBytesPerSec      = sampleRate * format.Format.nBlockAlign;
            format.Format.cbSize               = (WORD) (sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX));
            format.Samples.wValidBitsPerSample = 32;
            format.dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            format.SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

            // 200ms buffer（REFERENCE_TIME 单位 = 100ns）
            const REFERENCE_TIME bufferDuration = 20 * 1000 * 100;
            HRESULT hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                             AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             bufferDuration,
                                             0,
                                             &format.Format,
                                             nullptr);
            if (FAILED (hr))
                return "IAudioClient::Initialize(process loopback) failed (" + hrText (hr) + ")";

            dataEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
            if (dataEvent == nullptr)
                return "CreateEvent failed";

            hr = client->SetEventHandle (dataEvent);
            if (FAILED (hr))
                return "IAudioClient::SetEventHandle failed (" + hrText (hr) + ")";

            hr = client->GetService (__uuidof (IAudioCaptureClient), (void**) capture.ReleaseAndGetAddressOf());
            if (FAILED (hr) || capture == nullptr)
                return "IAudioClient::GetService(IAudioCaptureClient) failed (" + hrText (hr) + ")";

            hr = client->Start();
            if (FAILED (hr))
                return "IAudioClient::Start failed (" + hrText (hr) + ")";

            return {};
        }
    };
} // namespace

WasapiLoopbackCapture::WasapiLoopbackCapture() = default;

WasapiLoopbackCapture::~WasapiLoopbackCapture()
{
    stop();
}

void WasapiLoopbackCapture::setTargetProcess (const juce::String& exeName)
{
    if (captureThread != nullptr)
        return;   // 运行中不允许改目标：采集线程正在读 targetExeName

    targetExeName = exeName.trim();
}

void WasapiLoopbackCapture::fail (const juce::String& message)
{
    lastError = message;
    failed.store (true, std::memory_order_release);
}

bool WasapiLoopbackCapture::start()
{
    if (running.load (std::memory_order_acquire))
        return true; // already running

    // 上一次的线程已自行退出（失败 / 流失效）但还没 join：先收掉
    stop();

    shouldStop.store (false, std::memory_order_release);
    failed.store (false, std::memory_order_release);
    lastError = {};

    captureThread = std::make_unique<std::thread> ([this] { runCaptureThread(); });

    // 等线程真正进入 running 状态（或因失败退出）。
    //   整机输出：最多 500ms；
    //   定向程序：异步激活进程级回环偶尔要几百毫秒，放宽到 3s
    //            （目标没在运行时线程会立刻进入"等待"并置 running，不会真等这么久）。
    const int timeoutMs = targetExeName.isEmpty() ? 500 : 3000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (running.load (std::memory_order_acquire)) return true;
        if (failed.load (std::memory_order_acquire))  { stop(); return false; }
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    // 超时仍未 running → 认为失败
    if (! running.load (std::memory_order_acquire))
    {
        stop();
        if (lastError.isEmpty())
            lastError = "Loopback capture did not start within "
                          + juce::String (timeoutMs) + "ms.";
        return false;
    }
    return true;
}

void WasapiLoopbackCapture::stop()
{
    shouldStop.store (true, std::memory_order_release);
    if (captureThread != nullptr)
    {
        if (captureThread->joinable())
            captureThread->join();
        captureThread.reset();
    }
    running.store (false, std::memory_order_release);
}

void WasapiLoopbackCapture::runCaptureThread()
{
    // 提升线程优先级（WASAPI 推荐的"Pro Audio" MMCSS 类）
    DWORD mmcssTaskIdx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &mmcssTaskIdx);

    {
        ComApartment com;
        if (! com.ok)
            fail ("CoInitializeEx failed");
        else if (targetExeName.isEmpty())
            runEndpointCapture();
        else
            runProcessCapture();
    }

    running.store (false, std::memory_order_release);
    if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
}

// ----------------------------------------------------------
// A) 整机输出：默认渲染端点回环
// ----------------------------------------------------------
void WasapiLoopbackCapture::runEndpointCapture()
{
    ComPtr<IAudioClient> audioClient;
    MixFormatPtr         mixFormat;

    const juce::String openError = openDefaultRenderClient (audioClient, mixFormat);
    if (openError.isNotEmpty())
        return fail (openError);

    // 100ms buffer（REFERENCE_TIME 单位 = 100ns）
    const REFERENCE_TIME bufferDuration = 10 * 1000 * 100;
    HRESULT hr = audioClient->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_LOOPBACK,
                                          bufferDuration,
                                          0,
                                          mixFormat.get(),
                                          nullptr);
    if (FAILED (hr))
        return fail ("IAudioClient::Initialize(LOOPBACK) failed (" + hrText (hr) + ")");

    ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService (__uuidof (IAudioCaptureClient), (void**) captureClient.GetAddressOf());
    if (FAILED (hr))
        return fail ("IAudioClient::GetService(IAudioCaptureClient) failed");

    hr = audioClient->Start();
    if (FAILED (hr))
        return fail ("IAudioClient::Start failed");

    running.store (true, std::memory_order_release);

    CaptureSink sink (onAudio);
    pumpCapture (captureClient.Get(), mixFormat.get(), nullptr, nullptr, shouldStop, sink);

    audioClient->Stop();
}

// ----------------------------------------------------------
// B) 定向程序：进程级回环
//
//   状态机（循环直到 stop()）：
//     等待 ──(目标程序出现)──▶ 采集 ──(目标退出 / 流失效)──▶ 等待
//   等待期间按墙钟节奏输出静音，表头照常归零、波形照常滚动。
// ----------------------------------------------------------
void WasapiLoopbackCapture::runProcessCapture()
{
    if (! appaudio::isProcessLoopbackSupported())
        return fail ("Application audio capture requires Windows 10 version 2004 or later.");

    // 采样率跟随默认播放端点：与整机输出模式一致，来回切换不触发下游重新 prepare。
    //   整个线程生命周期内固定（等待期的静音与真实数据用同一个采样率）。
    double sampleRate = 48000.0;
    {
        ComPtr<IAudioClient> client;
        MixFormatPtr         mixFormat;
        if (openDefaultRenderClient (client, mixFormat).isEmpty() && mixFormat->nSamplesPerSec >= 8000)
            sampleRate = (double) mixFormat->nSamplesPerSec;
    }

    CaptureSink sink (onAudio);

    while (! shouldStop.load (std::memory_order_acquire))
    {
        const DWORD pid = (DWORD) appaudio::resolveTargetProcessId (targetExeName);

        if (pid != 0)
        {
            ProcessStream stream;
            const juce::String error = stream.open (pid, (DWORD) sampleRate, shouldStop);

            if (error.isEmpty())
            {
                running.store (true, std::memory_order_release);

                // 返回 = stop() / 目标退出 / 流失效；后两种落到下面的等待，稍后重新解析目标
                TargetWatch watch (pid, targetExeName);
                pumpCapture (stream.capture.Get(), &stream.format.Format,
                             stream.dataEvent, &watch, shouldStop, sink);
            }
            else if (! running.load (std::memory_order_acquire))
            {
                // 还没运行起来、第一次尝试就接不上 → 视为 start() 失败（上层弹窗 + 回退）。
                //   运行起来之后的失败（程序重启瞬间、设备切换 ...）只是回到等待，稍后重试。
                if (! shouldStop.load (std::memory_order_acquire))
                    fail (error);
                return;
            }
        }

        // 目标没在运行（或刚断开）：进入等待。即使从没接上过也算"在运行"。
        running.store (true, std::memory_order_release);

        // 等约 2 秒再重新找目标（找一次要对整张进程表拍快照，几十毫秒，不宜更密）；
        //   期间 5ms 一步补静音（与采集循环的轮询节奏一致），stop() 最多迟滞一步
        for (int step = 0; step < 400 && ! shouldStop.load (std::memory_order_acquire); ++step)
        {
            sink.emitSilenceIfDue (sampleRate);
            Sleep (5);
        }
    }
}

} // namespace y2k

#else // !JUCE_WINDOWS

namespace y2k
{
    WasapiLoopbackCapture::WasapiLoopbackCapture() = default;
    WasapiLoopbackCapture::~WasapiLoopbackCapture() = default;

    void WasapiLoopbackCapture::setTargetProcess (const juce::String& exeName) { targetExeName = exeName; }

    bool WasapiLoopbackCapture::start()
    {
        lastError = "WASAPI loopback is only supported on Windows.";
        return false;
    }
    void WasapiLoopbackCapture::stop() {}
    void WasapiLoopbackCapture::runCaptureThread() {}
}

#endif
