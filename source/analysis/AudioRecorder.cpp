#include "source/analysis/AudioRecorder.h"

#include <algorithm>
#include <cmath>

#if JUCE_WINDOWS
 // 枚举"已打开的资源管理器窗口"需要 IShellWindows / IWebBrowserApp：
 //   · CoCreateInstance（ole32）+ CLSID/IID（__uuidof 直接进目标文件）
 //   · SysFreeString（oleaut32）—— 工程只显式链接了 ole32/uuid/avrt，这里补一条
 //   · 打开目录本身走 juce::Process::openDocument（JUCE 内部动态加载 shell32）
 #include <windows.h>
 #include <oleauto.h>
 #include <shlobj.h>
 #include <exdisp.h>
 #pragma comment (lib, "oleaut32.lib")
#endif

namespace
{
constexpr double kFallbackSampleRate = 44100.0;

#if JUCE_WINDOWS
// 调用 COM 前确保当前线程已初始化（已初始化过也不报错）
struct ScopedComForCaller
{
    ScopedComForCaller()
    {
        const auto hr = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        ok = SUCCEEDED (hr) || hr == RPC_E_CHANGED_MODE;
        shouldUninit = SUCCEEDED (hr);
    }

    ~ScopedComForCaller()
    {
        if (shouldUninit)
            CoUninitialize();
    }

    bool ok = false;
    bool shouldUninit = false;
};

// 极简 COM 智能指针（避免依赖 juce::ComSmartPtr 的模块级宏开关）
template <typename Type>
struct ScopedComPtr
{
    ScopedComPtr() = default;

    ~ScopedComPtr() { if (pointer != nullptr) pointer->Release(); }

    Type** address() noexcept { return &pointer; }
    Type*  operator->() const noexcept { return pointer; }
    explicit operator bool() const noexcept { return pointer != nullptr; }

    Type* pointer = nullptr;

    JUCE_DECLARE_NON_COPYABLE (ScopedComPtr)
};

juce::String normalisePath (const juce::String& path)
{
    return path.replaceCharacter ('/', '\\').trim();
}

// 是否已有资源管理器窗口停在该目录；有则通过 outWindow 返回窗口句柄（可能为 nullptr）
bool isFolderOpenInExplorer (const juce::File& folder, HWND* outWindow)
{
    if (outWindow != nullptr)
        *outWindow = nullptr;

    ScopedComForCaller com;
    if (! com.ok)
        return false;

    ScopedComPtr<IShellWindows> shellWindows;
    if (FAILED (CoCreateInstance (__uuidof (ShellWindows), nullptr, CLSCTX_ALL,
                                  __uuidof (IShellWindows),
                                  (void**) shellWindows.address()))
        || ! shellWindows)
        return false;

    long count = 0;
    if (FAILED (shellWindows->get_Count (&count)) || count <= 0)
        return false;

    const auto target = normalisePath (folder.getFullPathName());

    for (long i = 0; i < count; ++i)
    {
        VARIANT index;
        VariantInit (&index);
        index.vt   = VT_I4;
        index.lVal = i;

        ScopedComPtr<IDispatch> dispatch;
        if (FAILED (shellWindows->Item (index, dispatch.address())) || ! dispatch)
            continue;

        ScopedComPtr<IWebBrowserApp> browser;
        if (FAILED (dispatch->QueryInterface (__uuidof (IWebBrowserApp),
                                              (void**) browser.address()))
            || ! browser)
            continue;

        BSTR url = nullptr;
        if (FAILED (browser->get_LocationURL (&url)) || url == nullptr)
            continue;

        const juce::String urlString (url);
        SysFreeString (url);

        if (! urlString.startsWithIgnoreCase ("file:"))
            continue;

        const auto location = juce::URL (urlString).getLocalFile();
        if (location == juce::File()
            || ! normalisePath (location.getFullPathName()).equalsIgnoreCase (target))
            continue;

        if (outWindow != nullptr)
        {
            SHANDLE_PTR hwnd = 0;
            if (SUCCEEDED (browser->get_HWND (&hwnd)) && hwnd != 0)
                *outWindow = (HWND) hwnd;
        }

        return true;
    }

    return false;
}

void bringWindowToFront (HWND window)
{
    if (window == nullptr)
        return;

    if (IsIconic (window))
        ShowWindow (window, SW_RESTORE);

    SetForegroundWindow (window);
}
#else
// 非 Windows：无法可靠查询文件管理器状态，退化为"本次会话同一个目录只打开一次"
bool shouldOpenOnlyOnce (const juce::File& folder)
{
    static juce::CriticalSection lock;
    static juce::String lastOpened;

    const juce::ScopedLock sl (lock);
    const auto path = folder.getFullPathName();
    if (lastOpened == path)
        return false;

    lastOpened = path;
    return true;
}
#endif
} // namespace

// ==========================================================
// 构造 / 析构
// ==========================================================
AudioRecorder::AudioRecorder()
    : juce::Thread ("Y2Kmeter-AudioRecorder")
{
    fifoData.resize ((size_t) fifoCapacityPerChannel * 2u);
    std::fill (fifoData.begin(), fifoData.end(), 0.0f);

    // 软件一启动（插件实例一创建）就开始滚动采集，不等 Recorder 模块被加载：
    //   · 采集线程只负责把 FIFO 搬进滚动历史，滚动缓冲仍是懒分配；
    //   · Recorder 模块只负责展示 / 手动录制 / 导出，不再控制采集开关。
    startCapture();
}

AudioRecorder::~AudioRecorder()
{
    shuttingDown.store (true, std::memory_order_release);
    stopCapture();

    // 兜底：采集线程若被强杀，尽量把手动录制文件收尾（不再打开目录）
    if (takeWriter != nullptr)
    {
        takeWriter->flush();
        takeWriter.reset();
    }
}

// ==========================================================
// 保存目录 / 打开目录
// ==========================================================
juce::File AudioRecorder::getRecordingsDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
               .getChildFile (folderName);
}

void AudioRecorder::openDirectory (const juce::File& folder)
{
    if (folder == juce::File())
        return;

    if (! folder.isDirectory())
        folder.createDirectory();

#if JUCE_WINDOWS
    HWND existingWindow = nullptr;
    if (isFolderOpenInExplorer (folder, &existingWindow))
    {
        // 需求：已经打开过这个文件夹 → 不重复开新窗口，只把原窗口抬到前面
        bringWindowToFront (existingWindow);
        return;
    }

    juce::Process::openDocument (folder.getFullPathName(), juce::String());
#else
    if (shouldOpenOnlyOnce (folder))
        folder.revealToUser();
#endif
}

// ==========================================================
// 采集启停（构造即开始、析构才停止）
// ==========================================================
void AudioRecorder::startCapture()
{
    if (captureEnabled.exchange (true, std::memory_order_acq_rel))
        return;

    // 清空搬运缓冲 + 历史（新一轮采集从 0 开始）
    fifo.reset();
    {
        const juce::ScopedLock sl (historyLock);
        historyWritePos = 0;
        historyValid   = 0;
    }
    historyReady.store (0, std::memory_order_relaxed);
    takeRequested.store (false, std::memory_order_release);
    takeActive.store (false, std::memory_order_release);
    takeHitLimit.store (false, std::memory_order_release);
    takeSamples.store (0, std::memory_order_relaxed);
    setStatus ("CAPTURING");

    capturing.store (true, std::memory_order_release);

    if (! startThread (juce::Thread::Priority::high))
    {
        capturing.store (false, std::memory_order_release);
        captureEnabled.store (false, std::memory_order_release);
        setStatus ("ERROR: cannot start capture thread");
    }
}

void AudioRecorder::stopCapture()
{
    if (! captureEnabled.exchange (false, std::memory_order_acq_rel))
    {
        capturing.store (false, std::memory_order_release);
        return;
    }

    notify();

    if (isThreadRunning())
        stopThread (2000);

    capturing.store (false, std::memory_order_release);

    takeActive.store (false, std::memory_order_release);
    takeRequested.store (false, std::memory_order_release);

    // 释放滚动缓冲（下次启用时按当时的采样率重新分配）
    {
        const juce::ScopedLock sl (historyLock);
        historyL.clear();
        historyR.clear();
        historyL.shrink_to_fit();
        historyR.shrink_to_fit();
        historyCapacity = 0;
        historyWritePos = 0;
        historyValid    = 0;
    }
    historyReady.store (0, std::memory_order_relaxed);
}

// ==========================================================
// 音频线程
// ==========================================================
void AudioRecorder::pushRaw (const float* left, const float* right,
                             int numSamples, double sampleRate) noexcept
{
    if (! captureEnabled.load (std::memory_order_acquire)
        || left == nullptr || right == nullptr || numSamples <= 0)
        return;

    if (sampleRate > 0.0)
        inputSampleRate.store (sampleRate, std::memory_order_relaxed);

    const int wanted = juce::jmin (numSamples, fifoCapacityPerChannel);

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (wanted, start1, size1, start2, size2);

    const int total = size1 + size2;
    auto* base = fifoData.data();

    if (size1 > 0)
    {
        juce::FloatVectorOperations::copy (base + start1, left, size1);
        juce::FloatVectorOperations::copy (base + fifoCapacityPerChannel + start1, right, size1);
    }

    if (size2 > 0)
    {
        juce::FloatVectorOperations::copy (base + start2, left + size1, size2);
        juce::FloatVectorOperations::copy (base + fifoCapacityPerChannel + start2,
                                           right + size1, size2);
    }

    if (total > 0)
        fifo.finishedWrite (total);
}

// ==========================================================
// 采集线程：FIFO → 60 s 滚动历史
// ==========================================================
void AudioRecorder::run()
{
    for (;;)
    {
        // 手动录制开始请求：先把 WAV 打开，随后排空的数据才会进文件
        if (takeRequested.exchange (false, std::memory_order_acq_rel))
        {
            if (takeActive.load (std::memory_order_acquire))
                openTakeWriter (inputSampleRate.load (std::memory_order_relaxed));
        }

        if (fifo.getNumReady() > 0)
        {
            // 只在真正有数据时才按当前采样率分配滚动缓冲
            //   （hover 预览等临时模块会 retain/release 一次，没有数据时不留 20+ MB）
            ensureHistory (inputSampleRate.load (std::memory_order_relaxed));
            appendFromFifo();
        }

        // 停止请求 + 数据已排空 → 关闭手动录制文件（最后一块也写进去）
        if (! takeActive.load (std::memory_order_acquire)
            && takeWriter != nullptr
            && fifo.getNumReady() == 0)
            closeTakeWriter();

        if (! captureEnabled.load (std::memory_order_acquire) || threadShouldExit())
            break;

        if (fifo.getNumReady() == 0)
            wait (10);
    }

    // 停采前把 FIFO 残余写完，并确保手动录制文件被正确关闭
    while (fifo.getNumReady() > 0)
    {
        ensureHistory (inputSampleRate.load (std::memory_order_relaxed));
        appendFromFifo();
    }

    closeTakeWriter();

    capturing.store (false, std::memory_order_release);
}

void AudioRecorder::ensureHistory (double sampleRate)
{
    if (sampleRate <= 0.0)
        sampleRate = kFallbackSampleRate;

    const int desired = (int) std::ceil (maxHistorySeconds * sampleRate) + 1;

    if (desired == historyCapacity && std::abs (sampleRate - historySampleRate) < 0.5)
        return;

    const juce::ScopedLock sl (historyLock);

    historyL.assign ((size_t) desired, 0.0f);
    historyR.assign ((size_t) desired, 0.0f);
    historyCapacity = desired;
    historyWritePos = 0;
    historyValid    = 0;
    historySampleRate = sampleRate;

    historyRateSnapshot.store (sampleRate, std::memory_order_relaxed);
    historyReady.store (0, std::memory_order_relaxed);
}

void AudioRecorder::appendFromFifo()
{
    const int ready = fifo.getNumReady();
    if (ready <= 0)
        return;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (juce::jmin (ready, moveChunkSize), start1, size1, start2, size2);

    if (size1 > 0)
    {
        writeTake (fifoData.data() + start1,
                   fifoData.data() + fifoCapacityPerChannel + start1,
                   size1);

        appendHistory (fifoData.data() + start1,
                       fifoData.data() + fifoCapacityPerChannel + start1,
                       size1);
    }

    if (size2 > 0)
    {
        writeTake (fifoData.data() + start2,
                   fifoData.data() + fifoCapacityPerChannel + start2,
                   size2);

        appendHistory (fifoData.data() + start2,
                       fifoData.data() + fifoCapacityPerChannel + start2,
                       size2);
    }

    fifo.finishedRead (size1 + size2);
}

void AudioRecorder::appendHistory (const float* left, const float* right, int numSamples)
{
    if (numSamples <= 0)
        return;

    const juce::ScopedLock sl (historyLock);

    if (historyCapacity <= 0 || (int) historyL.size() < historyCapacity)
        return;

    int remaining = numSamples;
    int src = 0;

    while (remaining > 0)
    {
        const int chunk = juce::jmin (remaining, historyCapacity - historyWritePos);

        juce::FloatVectorOperations::copy (historyL.data() + historyWritePos, left + src, chunk);
        juce::FloatVectorOperations::copy (historyR.data() + historyWritePos, right + src, chunk);

        historyWritePos += chunk;
        if (historyWritePos >= historyCapacity)
            historyWritePos = 0;

        historyValid = juce::jmin ((juce::int64) historyCapacity, historyValid + chunk);

        src += chunk;
        remaining -= chunk;
    }

    historyReady.store (historyValid, std::memory_order_relaxed);
}

// ==========================================================
// 手动录制（红点按钮）：消息线程发请求，采集线程流式写盘
// ==========================================================
bool AudioRecorder::startTake()
{
    if (! capturing.load (std::memory_order_acquire))
        return false;

    if (takeActive.exchange (true, std::memory_order_acq_rel))
        return false;

    const auto dir = getRecordingsDirectory();
    if (! dir.isDirectory() && ! dir.createDirectory().wasOk())
    {
        setStatus ("ERROR: cannot create folder");
        takeActive.store (false, std::memory_order_release);
        return false;
    }

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    auto file = dir.getChildFile (juce::String (takePrefix) + stamp + ".wav");
    if (file.existsAsFile())
        file = file.getNonexistentSibling (false);

    {
        const juce::ScopedLock sl (takeLock);
        pendingTakeFile = file;
    }

    takeSamples.store (0, std::memory_order_relaxed);
    takeHitLimit.store (false, std::memory_order_release);
    takeRequested.store (true, std::memory_order_release);
    setStatus ("RECORDING");
    notify();

    return true;
}

void AudioRecorder::stopTake()
{
    if (! takeActive.exchange (false, std::memory_order_acq_rel))
        return;

    setStatus ("SAVING");
    notify();   // 采集线程排空 FIFO → 关闭文件 → 打开目录
}

double AudioRecorder::getTakeSeconds() const noexcept
{
    const double sr = takeSampleRate.load (std::memory_order_relaxed);
    if (sr <= 0.0)
        return 0.0;

    return (double) takeSamples.load (std::memory_order_relaxed) / sr;
}

juce::File AudioRecorder::getTakeFile() const
{
    const juce::ScopedLock sl (takeLock);
    return pendingTakeFile;
}

void AudioRecorder::openTakeWriter (double sampleRate)
{
    juce::File file;
    {
        const juce::ScopedLock sl (takeLock);
        file = pendingTakeFile;
    }

    if (file == juce::File())
    {
        takeActive.store (false, std::memory_order_release);
        return;
    }

    if (sampleRate <= 0.0)
        sampleRate = kFallbackSampleRate;

    file.getParentDirectory().createDirectory();

    auto fileStream = file.createOutputStream();
    if (fileStream == nullptr)
    {
        setStatus ("ERROR: cannot write file");
        takeActive.store (false, std::memory_order_release);
        return;
    }

    juce::WavAudioFormat wavFormat;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (2)
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    std::unique_ptr<juce::OutputStream> stream (fileStream.release());
    auto writer = wavFormat.createWriterFor (stream, options);
    if (writer == nullptr)
    {
        setStatus ("ERROR: cannot create WAV writer");
        takeActive.store (false, std::memory_order_release);
        return;
    }

    takeWriter = std::move (writer);
    takeSampleRate.store (sampleRate, std::memory_order_relaxed);
    takeSamples.store (0, std::memory_order_relaxed);
}

void AudioRecorder::closeTakeWriter()
{
    if (takeWriter == nullptr)
        return;

    takeWriter->flush();
    takeWriter.reset();

    juce::File saved;
    {
        const juce::ScopedLock sl (takeLock);
        saved = pendingTakeFile;
    }

    {
        const juce::SpinLock::ScopedLockType sl (metaLock);
        const bool hitLimit = takeHitLimit.load (std::memory_order_acquire);

        if (saved == juce::File())
            statusText = {};
        else
            statusText = hitLimit ? juce::String ("SAVED (1 H LIMIT)") : juce::String ("SAVED");

        if (saved != juce::File())
            lastSavedFile = saved;
    }

    if (saved != juce::File() && ! shuttingDown.load (std::memory_order_acquire))
    {
        const auto folder = saved.getParentDirectory();
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
            juce::MessageManager::callAsync ([folder] { AudioRecorder::openDirectory (folder); });
    }
}

void AudioRecorder::writeTake (const float* left, const float* right, int numSamples)
{
    if (takeWriter == nullptr || numSamples <= 0)
        return;

    // ---- 单段最长 1 小时 ----
    //   只写到上限为止；到达上限时把 takeActive 置 false ——
    //   run() 会在 FIFO 排空后调用 closeTakeWriter() 收尾（flush + 关闭 + 打开目录），
    //   与用户手动点停止走完全同一条收尾路径，不会留下半截 WAV 头。
    const double sr = takeSampleRate.load (std::memory_order_relaxed);
    const auto limit = (juce::int64) std::llround (maxTakeSeconds
                                                   * (sr > 0.0 ? sr : kFallbackSampleRate));
    const auto written = takeSamples.load (std::memory_order_relaxed);
    const auto remain  = limit - written;

    int allowed = numSamples;
    if (remain <= 0)
        allowed = 0;
    else if ((juce::int64) numSamples > remain)
        allowed = (int) remain;

    if (allowed > 0)
    {
        const float* chans[2] = { left, right };
        takeWriter->writeFromFloatArrays (chans, 2, allowed);

        takeSamples.fetch_add (allowed, std::memory_order_relaxed);
    }

    if (allowed < numSamples)
    {
        takeHitLimit.store (true, std::memory_order_release);
        takeActive.store (false, std::memory_order_release);
        setStatus ("SAVING (1 H LIMIT)");
    }
}

// ==========================================================
// 导出最近 N 秒（消息线程，同步落盘）
// ==========================================================
bool AudioRecorder::exportLastSeconds (double seconds)
{
    if (seconds <= 0.0)
        return false;

    juce::AudioBuffer<float> snapshot;
    double sampleRate = kFallbackSampleRate;

    // 1) 加锁快照（只拷内存，几 ms～几十 ms；采集线程期间写不进去，
    //    但音频线程仍在往 FIFO 里写，1.36 s 容量足够吸收这点停顿）
    {
        const juce::ScopedLock sl (historyLock);

        if (historyCapacity <= 0 || historyValid <= 0)
        {
            setStatus ("NO AUDIO CAPTURED");
            return false;
        }

        sampleRate = historySampleRate;

        const auto wanted = (juce::int64) (seconds * sampleRate);
        const int  n      = (int) juce::jlimit ((juce::int64) 1, historyValid, wanted);

        snapshot.setSize (2, n, /*keepExistingContent*/ false,
                          /*clearExtraSpace*/ false,
                          /*avoidReallocating*/ false);

        int start = historyWritePos - n;
        while (start < 0)
            start += historyCapacity;

        int copied = 0;
        while (copied < n)
        {
            const int chunk = juce::jmin (n - copied, historyCapacity - start);

            snapshot.copyFrom (0, copied, historyL.data() + start, chunk);
            snapshot.copyFrom (1, copied, historyR.data() + start, chunk);

            start += chunk;
            if (start >= historyCapacity)
                start = 0;

            copied += chunk;
        }
    }

    // 2) 目标文件
    const auto dir = getRecordingsDirectory();
    if (! dir.isDirectory() && ! dir.createDirectory().wasOk())
    {
        setStatus ("ERROR: cannot create folder");
        return false;
    }

    const int clipSeconds = juce::jmax (1, (int) std::lround (seconds));
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");

    // Y2Kmeter_clip_<N秒>_<时间>.wav
    auto file = dir.getChildFile (juce::String (clipPrefix)
                                  + juce::String (clipSeconds) + "_"
                                  + stamp + ".wav");
    if (file.existsAsFile())
        file = file.getNonexistentSibling (false);

    auto fileStream = file.createOutputStream();
    if (fileStream == nullptr)
    {
        setStatus ("ERROR: cannot write file");
        return false;
    }

    juce::WavAudioFormat wavFormat;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (2)
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    std::unique_ptr<juce::OutputStream> stream (fileStream.release());
    auto writer = wavFormat.createWriterFor (stream, options);
    if (writer == nullptr)
    {
        setStatus ("ERROR: cannot create WAV writer");
        return false;
    }

    writer->writeFromAudioSampleBuffer (snapshot, 0, snapshot.getNumSamples());
    writer->flush();
    writer.reset();

    {
        const juce::SpinLock::ScopedLockType sl (metaLock);
        lastSavedFile = file;
        statusText    = "SAVED";
    }

    // 3) 打开保存目录（已打开则不重复开窗）
    openDirectory (dir);
    return true;
}

// ==========================================================
// 状态查询
// ==========================================================
double AudioRecorder::getBufferedSeconds() const noexcept
{
    const double sr = historyRateSnapshot.load (std::memory_order_relaxed);
    if (sr <= 0.0)
        return 0.0;

    const auto n = historyReady.load (std::memory_order_relaxed);
    return juce::jlimit (0.0, maxHistorySeconds, (double) n / sr);
}

juce::File AudioRecorder::getLastSavedFile() const
{
    const juce::SpinLock::ScopedLockType sl (metaLock);
    return lastSavedFile;
}

juce::String AudioRecorder::getStatusText() const
{
    const juce::SpinLock::ScopedLockType sl (metaLock);
    return statusText;
}

void AudioRecorder::setStatus (const juce::String& text)
{
    const juce::SpinLock::ScopedLockType sl (metaLock);
    statusText = text;
}
