#ifndef Y2K_LOCALISATION_H_INCLUDED
#define Y2K_LOCALISATION_H_INCLUDED

#include <JuceHeader.h>
#include <functional>

// 翻译入口（y2k::tr / y2k::trf）拆到轻量头里，只依赖 juce_core：
//   Objective-C++ 源文件不能安全地引入本文件（JuceHeader.h 末尾有全局
//   `using namespace juce;`，会与随后 import 的 Carbon/CoreServices 头撞名），
//   它们应当只 include I18nTranslate.h。详见该文件头部说明。
#include "I18nTranslate.h"

// ==========================================================
// y2k::i18n —— 界面国际化（运行时切换、无需重启）
//
//   设计要点（扩展新语言 = 只加数据，不改代码）：
//
//   1) 语言包 = 一个 UTF-8 文本文件 `<code>.lang`：
//        @code      = zh-CN          语言代码（唯一键）
//        @name      = 简体中文        语言自称（菜单显示）
//        @label     = 中             工具栏语言按钮上的短标签
//        @order     = 10             排序（小在前；源语言 en = 0）
//        @match     = zh             系统语言前缀（逗号分隔），首次启动自动匹配
//        @font      = ChillBitmap_16px_ttf   字体：内嵌资源名 或 .lang 同目录下的文件名；
//                                    省略 / default = 内置 Silkscreen
//        @fontScale = 0.85           字号缩放（不同字体 em 框 / 字面比例不同）
//        @minFontHeight = 10         最小字号（px，防止 CJK 小字糊成一团）
//        "English source" = "译文"     条目；支持 \n \t \" \\ 转义，# 开头为注释
//
//      来源（后者覆盖前者的同名条目，便于社区补译 / 热修文案）：
//        a. 构建期打包：仓库 i18n/*.lang（CMake 自动 glob 进 Y2KI18nData）
//        b. 运行期外挂：<用户应用数据>/Y2Kmeter/lang/*.lang（无需重新编译）
//
//   2) 键 = 英文源文案（gettext 风格）。查表顺序（结果带缓存）：
//        整串精确 → 去首尾空白 → 数字模板化（"SAVE LAST 10 s" → "SAVE LAST {0} s"）
//        → 多行逐行翻译。均未命中则原样返回（缺译自动回退英文，永不崩坏）。
//
//   3) 显示层"延迟翻译"：PinkXPLookAndFeel 在绘制按钮 / 标签 / 下拉框 / 菜单 /
//      提示 / 标题栏时调用 translate()，因此控件里存的始终是英文源文案 ——
//      运行时切换语言只需重绘，不必逐个控件 setText；比较逻辑、持久化不受影响。
//      自绘文本（g.drawText）与对话框文案在调用点用 y2k::tr() / y2k::trf() 包裹。
//
//   4) 切换广播：setLanguage() → 清缓存 → 通知订阅者（Editor 负责换字体、
//      失效各模块文字缓存、重排 + 重绘，见 PluginEditor::applyLanguageChange）。
//
//   线程：setLanguage / subscribe 只在消息线程调用；translate() 线程安全
//   （GL 线程上的 overlay 绘制也可能调用）。
// ==========================================================
namespace y2k::i18n
{
    struct LanguageInfo
    {
        juce::String code;            // "en" / "zh-CN"
        juce::String nativeName;      // "English" / "简体中文"
        juce::String buttonLabel;     // "EN" / "中"
        int          order = 100;
        float        fontScale = 1.0f;
        float        minFontHeight = 0.0f;
        bool         isSource = false;   // 源语言（英文）：不查表
    };

    // ---- 语言注册表 ----
    int                 getNumLanguages();
    const LanguageInfo& getLanguage (int index);
    int                 indexOfLanguage (const juce::String& code);   // -1 = 不存在

    // ---- 当前语言 ----
    const LanguageInfo& getCurrentLanguage();
    juce::String        getCurrentLanguageCode();
    bool                isSourceLanguage();

    // 切换语言（持久化 + 广播）。code 不存在时忽略。
    void setLanguage (const juce::String& code);
    // 切到注册表里的下一种语言（循环）
    void cycleToNextLanguage();

    // 每次切换 +1；可作为缓存键的一部分（如静态文字位图缓存）
    juce::uint32 getGeneration() noexcept;

    // ---- 翻译 ----
    //   translate / translateFormat 声明在 I18nTranslate.h（上方已引入），
    //   这样 ObjC++ 源文件可以只依赖轻量头拿到 y2k::tr / y2k::trf。

    // ---- 字体 ----
    //   当前语言的字体数据；返回 false 表示使用内置默认字体（Silkscreen）。
    //   fontId 用于字体缓存键（同一字体只创建一次 Typeface）。
    bool getCurrentFontData (const void*& data, size_t& size, juce::String& fontId);

    // ---- 切换通知 ----
    int  subscribe (std::function<void()> onLanguageChanged);
    void unsubscribe (int token);

    // 运行期外挂语言包目录（<用户应用数据>/Y2Kmeter/lang）；不存在时不会自动创建
    juce::File getUserLanguageDirectory();
}

// y2k::tr / y2k::trf 的便捷入口定义在 I18nTranslate.h（上方已引入）。

#endif // Y2K_LOCALISATION_H_INCLUDED
