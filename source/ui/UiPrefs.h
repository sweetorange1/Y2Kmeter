#ifndef Y2K_UI_PREFS_H_INCLUDED
#define Y2K_UI_PREFS_H_INCLUDED

#include <JuceHeader.h>

// ==========================================================
// UiPrefs —— 进程级 UI 偏好（独立于布局 / 预设 / 宿主工程状态）
//
//   存档：用户目录下 Y2Kmeter.uiPrefs.xml
//     · Windows：%APPDATA%\Y2Kmeter.uiPrefs.xml
//     · macOS  ：~/Library/Application Support/Y2Kmeter.uiPrefs.xml
//   Standalone 与各插件实例共用同一份；只在用户切换时写入，频率极低。
//
//   当前条目：
//     · hoverTips  —— 悬停提示开关（HoverHelp）
//     · language   —— 界面语言代码（y2k::i18n）
//
//   只在消息线程调用。实现不持有任何 JUCE 静态对象（规避插件 DLL 卸载期的
//   静态析构顺序问题），每次读写都是一次性的小文件 IO。
// ==========================================================
namespace y2k::prefs
{
    juce::String getString (const char* key, const juce::String& defaultValue);
    void         setString (const char* key, const juce::String& value);

    bool getBool (const char* key, bool defaultValue);
    void setBool (const char* key, bool value);

    // 偏好文件路径（供其它需要"用户目录下 Y2Kmeter 子目录"的模块参考）
    juce::File getPrefsFile();
}

#endif // Y2K_UI_PREFS_H_INCLUDED
