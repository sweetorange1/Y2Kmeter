// ==========================================================
// ThemeLibrary.h —— 我的主题库（R11）+ 分享/导入导出（R12）
//
//   · 用户主题（≤ kMaxUserThemes 个）：名称 + ThemeSpec
//
//   · 持久化（"颜色存档独立"）：
//       - 主题库有自己的**用户级文件**：<userAppData>/Y2Kmeter.themeLibrary.xml
//         任何增删改都会立即落盘 —— 与软件存档（Standalone 的 .settings /
//         VST3 的宿主工程状态）完全解耦。
//       - 存档（.settings / 宿主工程）里的 "themeLib" 仍然会写入（分享/迁移用），
//         但**读取时只合并、不清空**：Load 一份不带主题（或只带少量主题）的存档，
//         用户自己创建的颜色主题不会丢失；存档里额外的主题会被追加进来。
//       - 同名同色视为同一条，跳过；同名不同色保留双方（新的一份自动改名）。
//
//   · 分享码：`Y2KT1-` + Base64url（ThemeSpec 自带，这里做转发）
//   · 文件：.y2kpalette（XML）
// ==========================================================
#pragma once

#include <JuceHeader.h>
#include "source/ui/palette/ThemeSpec.h"
#include <functional>
#include <vector>

namespace PinkXP
{
    class ThemeLibrary
    {
    public:
        static ThemeLibrary& instance();

        struct UserTheme
        {
            juce::String      name;
            y2kpal::ThemeSpec spec;
        };

        // 容量上限（合并存档时也受此限制；满了会丢弃多出来的条目）
        static constexpr int kMaxUserThemes = 24;

        const std::vector<UserTheme>& themes() const noexcept { return themes_; }
        int size() const noexcept { return (int) themes_.size(); }

        // ---- 增删改查 ----
        // 保存（已存在同名则覆盖；满了返回 false）
        bool saveTheme(const juce::String& name, const y2kpal::ThemeSpec& spec);
        bool renameTheme(int index, const juce::String& newName);
        bool removeTheme(int index);
        void moveTheme(int from, int to);              // 排序（拖动/上移下移）
        int  indexOfName(const juce::String& name) const;
        bool isValidName(const juce::String& name) const;

        // 应用第 index 个我的主题（→ PinkXP::applyUserTheme）
        void applyThemeAt(int index);

        // ---- 序列化 ----
        juce::String toXmlString() const;
        // 替换整个库（旧语义；会同时落盘到独立文件）
        bool loadFromXmlString(const juce::String& xml);
        // 合并：保留内存中已有的主题，只追加 XML 里缺失的（同名同色跳过）
        bool mergeFromXmlString(const juce::String& xml);

        // ---- 独立主题存档文件（与 settings / 宿主工程解耦）----
        static juce::File getDefaultLibraryFile();
        juce::File getLibraryFile() const { return libraryFile_; }
        void setLibraryFile(const juce::File& f);        // 改变路径后下次 ensure 会重新读
        // 首次调用时把磁盘上的主题库读进内存（幂等；之后直接返回）
        bool ensureLoadedFromLibraryFile();
        bool saveToLibraryFile() const;

        // ---- 与旧接口兼容 ----
        // Standalone：读/写 settings["ui.themeLibrary"]（R18）。
        //   注意：getUserSettings() 不是全局函数（是 Standalone 应用类的私有成员），
        //   因此由调用方传入 PropertiesFile 指针。
        bool loadFromSettings(juce::PropertiesFile* settings);      // 替换语义（历史接口）
        bool mergeFromSettings(juce::PropertiesFile* settings);     // 合并语义（启动时用）
        bool persistToSettings(juce::PropertiesFile* settings);

        // ---- 分享码（R12）----
        static juce::String specToShareCode(const y2kpal::ThemeSpec& spec)
        {
            return juce::String(spec.shareCode());
        }
        static bool shareCodeToSpec(const juce::String& code, y2kpal::ThemeSpec& out)
        {
            return y2kpal::ThemeSpec::fromShareCode(code.toStdString(), out);
        }

        // ---- .y2kpalette 文件导入导出（R12）----
        bool exportToFile(int index, const juce::File& file);
        bool importFromFile(const juce::File& file,
                            juce::String& outName, y2kpal::ThemeSpec& outSpec);

        // ---- 变更订阅（色票条 / 面板刷新）----
        int  subscribe(std::function<void()> cb);
        void unsubscribe(int token);

    private:
        ThemeLibrary() = default;

        std::vector<UserTheme> themes_;
        struct Listener { int token; std::function<void()> cb; };
        std::vector<Listener> listeners_;
        int nextToken_ = 1;

        juce::File libraryFile_ = getDefaultLibraryFile();
        bool libraryLoaded_ = false;

        // 把 XML 里的主题并入内存（不落盘、不通知）。返回新增数量；-1 = XML 无效
        int  mergeXmlIntoThemes(const juce::String& xml);
        juce::String makeUniqueName(const juce::String& base) const;
        void persistAfterChange();     // 落盘 + 通知订阅者

        void notifyListeners();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ThemeLibrary)
    };
}
