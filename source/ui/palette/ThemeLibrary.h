// ==========================================================
// ThemeLibrary.h —— 我的主题库（R11）+ 分享/导入导出（R12）
//
//   · 用户主题（最多 12 个）：名称 + ThemeSpec + 纹理 + 标题图标
//   · 持久化：
//       - Standalone：getUserSettings() 的 "ui.themeLibrary"（XML）
//       - VST3：随宿主工程保存 —— PluginProcessor 的 state 里存同一段 XML
//         （toXmlString / loadFromXmlString 由两边共用）
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

        static constexpr int kMaxUserThemes = 12;   // R11：至少支持 12 个

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

        // ---- 持久化 ----
        juce::String toXmlString() const;
        bool loadFromXmlString(const juce::String& xml);

        // Standalone：读/写 settings["ui.themeLibrary"]（R18）。
        //   注意：getUserSettings() 不是全局函数（是 Standalone 应用类的私有成员），
        //   因此由调用方传入 PropertiesFile 指针；VST3 走 PluginProcessor state 无需调用。
        bool loadFromSettings(juce::PropertiesFile* settings);
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

        void notifyListeners();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ThemeLibrary)
    };
}
