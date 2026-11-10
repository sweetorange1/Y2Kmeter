// ==========================================================
// WasapiLoopbackCapture.cpp
//   使用 WASAPI 采集 "默认渲染端点（扬声器/耳机）" 的混音输出信号
//
//   关键 API：
//     - IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)
//     - IAudioClient::Initialize(AUDCLNT_SHAREMODE_SHARED,
//                                AUDCLNT_STREAMFLAGS_LOOPBACK
//                              | AUDCLNT_STREAMFLAGS_EVENTCALLBACK? (这里用轮询，更简单) ,
//                                hnsBufferDuration, 0, mixFormat, nullptr)
//     - IAudioCaptureClient::GetBuffer / ReleaseBuffer
//
//   混音格式一般是 WAVEFORMATEXTENSIBLE (PCM float32 / int16 / int24)；
//   为简化，这里只支持 IEEE float32 与 PCM int16/int24/int32 的 interleaved，
//   运行时转成两路平面 float32。
//
//   线程：采集在一个后台 std::thread；每次拿到 buffer → 转格式 → 调 onAudio
// ==========================================================

#include "WasapiLoopbackCapture.h"

#if JUCE_WINDOWS

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <thread>
#include <vector>
#include <chrono>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

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

    template <class T>
    void safeRelease (T*& p) noexcept
    {
        if (p != nullptr) { p->Release(); p = nullptr; }
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
} // namespace

WasapiLoopbackCapture::WasapiLoopbackCapture() = default;

WasapiLoopbackCapture::~WasapiLoopbackCapture()
{
    stop();
}

bool WasapiLoopbackCapture::start()
{
    if (running.load (std::memory_order_acquire))
        return true; // already running

    shouldStop.store (false, std::memory_order_release);
    lastError = {};

    captureThread = std::make_unique<std::thread> ([this] { runCaptureThread(); });

    // 等最多 500ms 让线程真正进入 running 状态（或因失败退出）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (500);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (running.load (std::memory_order_acquire)) return true;
        if (! captureThread->joinable())               return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
        // 线程可能已因失败退出但仍 joinable()；看 lastError 是否被写
        if (lastError.isNotEmpty()) { stop(); return false; }
    }

    // 超时仍未 running → 认为失败
    if (! running.load (std::memory_order_acquire))
    {
        stop();
        if (lastError.isEmpty())
            lastError = "Loopback capture did not start within 500ms.";
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

    ComApartment com;
    if (! com.ok)
    {
        lastError = "CoInitializeEx failed";
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
        return;
    }

    IMMDeviceEnumerator* enumerator   = nullptr;
    IMMDevice*           renderDevice = nullptr;
    IAudioClient*        audioClient  = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX*        mixFormat    = nullptr;

    auto cleanup = [&]()
    {
        safeRelease (captureClient);
        if (audioClient != nullptr)
        {
            audioClient->Stop();
            safeRelease (audioClient);
        }
        safeRelease (renderDevice);
        safeRelease (enumerator);
        if (mixFormat != nullptr) { CoTaskMemFree (mixFormat); mixFormat = nullptr; }
    };

    HRESULT hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof (IMMDeviceEnumerator), (void**) &enumerator);
    if (FAILED (hr))
    {
        lastError = "CoCreateInstance(MMDeviceEnumerator) failed";
        cleanup();
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
        return;
    }

    // 取"默认"渲染端点（就是系统当前外放设备）
    hr = enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &renderDevice);
    if (FAILED (hr) || renderDevice == nullptr)
    {
        lastError = "GetDefaultAudioEndpoint(eRender) failed; no active playback device?";
        cleanup();
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
        return;
    }

    hr = renderDevice->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &audioClient);
    if (FAILED (hr))
    {
        lastError = "IMMDevice::Activate(IAudioClient) failed";
        cleanup();
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
        return;
    }

    hr = audioClient->GetMixFormat (&mixFormat);
    if (FAILED (hr) || mixFormat == nullptr)
    {
        lastError = "IAudioClient::GetMixFormat failed";
        cleanup();
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
        return;
    }

    // 100ms buffer（REFERENCE_TIME 单位 = 100ns）
    REFERENCE_TIME bufferDuration = 10 * 1000 * 100;
    hr = audioClient->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  bufferDuration,
                                  0,
                                  mixFormat,
                                  nullptr);
    if (FAILED (hr))
    {
        lastError = "IAudioClient::Initialize(LOOPBACK) failed (hr=0x"
                     + juce::String::toHexString ((int) hr) + ")";
        cleanup();
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
        return;
    }

    hr = audioClient->GetService (__uuidof (IAudioCaptureClient), (void**) &captureClient);
    if (FAILED (hr))
    {
        lastError = "IAudioClient::GetService(IAudioCaptureClient) failed";
        cleanup();
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
        return;
    }

    hr = audioClient->Start();
    if (FAILED (hr))
    {
        lastError = "IAudioClient::Start failed";
        cleanup();
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
        return;
    }

    running.store (true, std::memory_order_release);

    const double sampleRate = (double) mixFormat->nSamplesPerSec;

    // 每次 sleep 一半的 buffer，避免忙轮询
    const DWORD pollMs = 5;

    std::vector<float> bufL, bufR;
    bufL.reserve (4096);
    bufR.reserve (4096);

    // 记录"上次有真实数据"的时间：系统静音时 WASAPI loopback 可能不再产生
    //   packet（GetNextPacketSize 返回 0），onAudio 不再触发 → 分析器冻结。
    //   这里在确认静音后主动补全 0 数据，让 LoudnessMeter 等持续归零。
    //
    //   ★ 补零的两个节奏细节都直接决定 Waveform 的观感：
    //     1) 补零必须是"墙钟小步"，不能一次灌一大段（旧实现每 100ms 灌 0.1s）：
    //        下游按样本数推进的模块会被脉冲带偏，画面变成"定格 → 横跳"；
    //     2) 判定窗口不能太长。旧实现要等 100ms 才开始补零 —— 这 100ms 里
    //        一个样本都不进，波形硬定格，就是"刚进入静音时顿一下"。
    //        现在窗口收到 20ms（loopback 正常 packet 间隔 ~10ms），并且窗口
    //        到期时**从最后一个真实 packet 起把整段空白一次补齐**，
    //        时间轴不丢，只剩 20ms（120px/s 下 ≈2px）的极小迟疑。
    constexpr double kSilenceGraceMs  = 20.0;   // 判定静音前的等待窗口
    constexpr int    kMaxSilenceChunk = 1024;   // 单次补零上限（< 示波器 2048 快照）

    auto   lastDataTime      = std::chrono::steady_clock::now();
    auto   lastSilenceTopUp  = lastDataTime;   // 已经补到的时刻
    bool   silenceFeeding    = false;          // 是否已越过判定窗口、进入补零
    double silenceFramesOwed = 0.0;            // 不足 1 帧的余量累积，避免长期漂移

    while (! shouldStop.load (std::memory_order_acquire))
    {
        UINT32 packetLength = 0;
        hr = captureClient->GetNextPacketSize (&packetLength);
        if (FAILED (hr)) break;

        if (packetLength == 0)
        {
            const auto now = std::chrono::steady_clock::now();

            if (! silenceFeeding)
            {
                // 判定窗口：正常 loopback packet 间隔约 10ms，只有明显超过它才认为
                //   引擎真的停了（窗口内不产出任何数据，避免把真实音频替换成 0）。
                const double gapMs = std::chrono::duration<double, std::milli> (now - lastDataTime).count();
                if (gapMs < kSilenceGraceMs)
                {
                    Sleep (pollMs);
                    continue;
                }

                // 确认静音：从"最后一个真实 packet"开始补，把窗口里的空白一起补上
                silenceFeeding   = true;
                lastSilenceTopUp = lastDataTime;
            }

            const double elapsedSec = std::chrono::duration<double> (now - lastSilenceTopUp).count();
            lastSilenceTopUp = now;

            // 单次上限 100ms：采集线程被调度延迟（睡眠 / 调试断点 / 系统卡顿）后
            //   不要一次性灌进大段数据 —— 那正是本函数要消除的脉冲。
            silenceFramesOwed += juce::jlimit (0.0, 0.1, elapsedSec) * sampleRate;

            int frames = (int) silenceFramesOwed;
            if (frames > 0)
            {
                // 单块上限 1024：始终小于示波器 2048 样本快照，任何采样率下都不会
                //   因为"一帧只能消费快照"而丢样本（丢样本 = 滚动变慢 + 跳格）。
                frames = juce::jmin (frames, kMaxSilenceChunk);
                silenceFramesOwed -= (double) frames;

                bufL.assign ((size_t) frames, 0.0f);
                bufR.assign ((size_t) frames, 0.0f);
                if (onAudio)
                    onAudio (bufL.data(), bufR.data(), frames, sampleRate);
            }

            Sleep (pollMs);
            continue;
        }

        silenceFeeding    = false;
        lastDataTime      = std::chrono::steady_clock::now();
        lastSilenceTopUp  = lastDataTime;
        silenceFramesOwed = 0.0;

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

                bufL.assign ((size_t) frames, 0.0f);
                bufR.assign ((size_t) frames, 0.0f);

                if (! silent && data != nullptr)
                    decodeToStereoFloat (data, (int) frames, mixFormat, bufL, bufR);

                if (onAudio)
                    onAudio (bufL.data(), bufR.data(), (int) frames, sampleRate);
            }

            captureClient->ReleaseBuffer (frames);

            hr = captureClient->GetNextPacketSize (&packetLength);
            if (FAILED (hr)) { packetLength = 0; break; }
        }
    }

    running.store (false, std::memory_order_release);
    cleanup();
    if (mmcss != nullptr) AvRevertMmThreadCharacteristics (mmcss);
}

} // namespace y2k

#else // !JUCE_WINDOWS

namespace y2k
{
    WasapiLoopbackCapture::WasapiLoopbackCapture() = default;
    WasapiLoopbackCapture::~WasapiLoopbackCapture() = default;

    bool WasapiLoopbackCapture::start()
    {
        lastError = "WASAPI loopback is only supported on Windows.";
        return false;
    }
    void WasapiLoopbackCapture::stop() {}
    void WasapiLoopbackCapture::runCaptureThread() {}
}

#endif
