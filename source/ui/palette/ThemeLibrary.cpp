// ==========================================================
// ThemeLibrary.cpp —— 我的主题库实现
//
//   "颜色存档独立"：
//     · 主题库有自己的用户级文件（Y2Kmeter.themeLibrary.xml），任何增删改
//       立即落盘；
//     · 软件存档（Standalone .settings / VST3 宿主工程）里的 themeLib 只在
//       启动/加载时**合并**进来，绝不清空用户已创建的主题。
// ==========================================================
#include "source/ui/palette/ThemeLibrary.h"
#include "source/ui/PinkXPStyle.h"

namespace PinkXP
{
    // ---- 独立主题存档文件路径 ----
    //   Windows：%APPDATA%\Y2Kmeter.themeLibrary.xml（与 Y2Kmeter.settings 同目录）
    //   macOS  ：~/Library/Application Support/Y2Kmeter.themeLibrary.xml
    juce::File ThemeLibrary::getDefaultLibraryFile()
    {
        auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);

       #if JUCE_MAC
        dir = dir.getChildFile("Application Support");
       #endif

        return dir.getChildFile("Y2Kmeter.themeLibrary.xml");
    }

    ThemeLibrary& ThemeLibrary::instance()
    {
        static ThemeLibrary lib;
        return lib;
    }

    bool ThemeLibrary::isValidName(const juce::String& name) const
    {
        const juce::String trimmed = name.trim();
        return trimmed.isNotEmpty() && trimmed.length() <= 32;
    }

    int ThemeLibrary::indexOfName(const juce::String& name) const
    {
        for (int i = 0; i < (int) themes_.size(); ++i)
            if (themes_[(size_t) i].name == name)
                return i;
        return -1;
    }

    // 同名冲突时给"外来"主题生成一个不冲突的名字（"Name (2)"）
    juce::String ThemeLibrary::makeUniqueName(const juce::String& base) const
    {
        const juce::String stem = base.trim().substring(0, 28);
        for (int i = 2; i < 1000; ++i)
        {
            const juce::String candidate = stem + " (" + juce::String(i) + ")";
            if (indexOfName(candidate) < 0)
                return candidate;
        }
        return stem;
    }

    // ==================================================
    // 增删改查
    // ==================================================
    bool ThemeLibrary::saveTheme(const juce::String& name, const y2kpal::ThemeSpec& spec)
    {
        if (!isValidName(name))
            return false;

        // 用户显式保存：先确保磁盘上的库已读入，避免覆盖掉已有主题。
        ensureLoadedFromLibraryFile();

        const int existing = indexOfName(name.trim());
        if (existing >= 0)
        {
            themes_[(size_t) existing].spec = spec;
            persistAfterChange();
            return true;
        }

        if ((int) themes_.size() >= kMaxUserThemes)
            return false;   // 满员：面板提示用户先删除

        themes_.push_back({ name.trim(), spec });
        persistAfterChange();
        return true;
    }

    bool ThemeLibrary::renameTheme(int index, const juce::String& newName)
    {
        if (index < 0 || index >= (int) themes_.size())
            return false;
        if (!isValidName(newName))
            return false;
        if (indexOfName(newName) != -1 && indexOfName(newName) != index)
            return false;

        themes_[(size_t) index].name = newName.trim();
        persistAfterChange();
        return true;
    }

    bool ThemeLibrary::removeTheme(int index)
    {
        if (index < 0 || index >= (int) themes_.size())
            return false;
        themes_.erase(themes_.begin() + index);
        persistAfterChange();
        return true;
    }

    void ThemeLibrary::moveTheme(int from, int to)
    {
        if (from < 0 || from >= (int) themes_.size() ||
            to   < 0 || to   >= (int) themes_.size() || from == to)
            return;
        auto item = themes_[(size_t) from];
        themes_.erase(themes_.begin() + from);
        themes_.insert(themes_.begin() + to, item);
        persistAfterChange();
    }

    void ThemeLibrary::applyThemeAt(int index)
    {
        if (index < 0 || index >= (int) themes_.size())
            return;
        const auto& t = themes_[(size_t) index];
        PinkXP::applyUserTheme(t.spec, t.name);
    }

    // ==================================================
    // 序列化
    // ==================================================
    // XML（toXmlString / loadFromXmlString，Standalone 与 VST3 共用）
    juce::String ThemeLibrary::toXmlString() const
    {
        juce::XmlElement root("Y2KT");
        root.setAttribute("libVer", 2);

        for (const auto& t : themes_)
        {
            auto* e = new juce::XmlElement("THEME");
            e->setAttribute("name", t.name);
            e->setAttribute("spec", juce::String(t.spec.shareCode()));
            root.addChildElement(e);
        }
        return root.toString();
    }

    int ThemeLibrary::mergeXmlIntoThemes(const juce::String& xml)
    {
        auto root = juce::parseXML(xml);
        if (root == nullptr || !root->hasTagName("Y2KT"))
            return -1;

        int added = 0;

        for (auto* e : root->getChildIterator())
        {
            if (!e->hasTagName("THEME"))
                continue;

            y2kpal::ThemeSpec spec;
            const juce::String code = e->getStringAttribute("spec");
            if (!y2kpal::ThemeSpec::fromShareCode(code.toStdString(), spec))
                continue;

            juce::String name = e->getStringAttribute("name").trim();
            if (name.isEmpty())
                name = "Imported Palette";

            const int existing = indexOfName(name);
            if (existing >= 0)
            {
                // 同名同色 → 同一条，跳过；同名不同色 → 两边都保留（改名后追加）
                if (themes_[(size_t) existing].spec.shareCode() == spec.shareCode())
                    continue;

                name = makeUniqueName(name);
            }

            if ((int) themes_.size() >= kMaxUserThemes)
                break;      // 满员：剩下的装不下，但绝不清空已有主题

            themes_.push_back({ name, spec });
            ++added;
        }

        return added;
    }

    bool ThemeLibrary::loadFromXmlString(const juce::String& xml)
    {
        auto root = juce::parseXML(xml);
        if (root == nullptr || !root->hasTagName("Y2KT"))
            return false;   // 无效内容：保留现有主题，不做破坏性清空

        themes_.clear();
        mergeXmlIntoThemes(xml);
        persistAfterChange();
        return true;
    }

    bool ThemeLibrary::mergeFromXmlString(const juce::String& xml)
    {
        ensureLoadedFromLibraryFile();

        const int added = mergeXmlIntoThemes(xml);
        if (added < 0)
            return false;   // 无效内容

        if (added > 0)
            persistAfterChange();

        return true;
    }

    // ==================================================
    // 独立主题存档文件
    // ==================================================
    void ThemeLibrary::setLibraryFile(const juce::File& f)
    {
        libraryFile_ = f;
        libraryLoaded_ = false;
    }

    bool ThemeLibrary::ensureLoadedFromLibraryFile()
    {
        if (libraryLoaded_)
            return true;

        libraryLoaded_ = true;      // 只尝试一次（文件缺失时不反复 io）

        if (libraryFile_ == juce::File{})
            libraryFile_ = getDefaultLibraryFile();

        if (!libraryFile_.existsAsFile())
            return false;

        const juce::String xml = libraryFile_.loadFileAsString();
        if (xml.isEmpty())
            return false;

        const int added = mergeXmlIntoThemes(xml);
        if (added > 0)
            notifyListeners();

        return added >= 0;
    }

    bool ThemeLibrary::saveToLibraryFile() const
    {
        if (libraryFile_ == juce::File{})
            return false;

        auto parent = libraryFile_.getParentDirectory();
        if (parent != juce::File{} && !parent.isDirectory())
            parent.createDirectory();

        return libraryFile_.replaceWithText(toXmlString());
    }

    void ThemeLibrary::persistAfterChange()
    {
        saveToLibraryFile();
        notifyListeners();
    }

    // ==================================================
    // settings 兼容层（Standalone）
    // ==================================================
    bool ThemeLibrary::loadFromSettings(juce::PropertiesFile* settings)
    {
        if (settings == nullptr)
            return false;
        const juce::String xml = settings->getValue("ui.themeLibrary", {});
        if (xml.isEmpty())
            return false;
        return loadFromXmlString(xml);
    }

    bool ThemeLibrary::mergeFromSettings(juce::PropertiesFile* settings)
    {
        if (settings == nullptr)
            return false;
        const juce::String xml = settings->getValue("ui.themeLibrary", {});
        if (xml.isEmpty())
            return false;
        return mergeFromXmlString(xml);
    }

    bool ThemeLibrary::persistToSettings(juce::PropertiesFile* settings)
    {
        if (settings == nullptr)
            return false;
        settings->setValue("ui.themeLibrary", toXmlString());
        return true;
    }

    // ==================================================
    // .y2kpalette 文件
    // ==================================================
    bool ThemeLibrary::exportToFile(int index, const juce::File& file)
    {
        if (index < 0 || index >= (int) themes_.size())
            return false;

        juce::XmlElement root("Y2KPALETTE");
        root.setAttribute("name", themes_[(size_t) index].name);
        root.setAttribute("spec", juce::String(themes_[(size_t) index].spec.shareCode()));

        return file.replaceWithText(root.toString());
    }

    bool ThemeLibrary::importFromFile(const juce::File& file,
                                      juce::String& outName, y2kpal::ThemeSpec& outSpec)
    {
        auto root = juce::parseXML(file);
        if (root == nullptr)
            return false;

        y2kpal::ThemeSpec spec;
        if (!root->hasTagName("Y2KPALETTE"))
        {
            // 兼容：允许直接导入"库 XML"里第一个主题
            if (root->hasTagName("Y2KT"))
            {
                for (auto* e : root->getChildIterator())
                {
                    if (e->hasTagName("THEME") &&
                        y2kpal::ThemeSpec::fromShareCode(
                            e->getStringAttribute("spec").toStdString(), spec))
                    {
                        outName = e->getStringAttribute("name");
                        outSpec = spec;
                        return true;
                    }
                }
            }
            return false;
        }

        if (!y2kpal::ThemeSpec::fromShareCode(
                root->getStringAttribute("spec").toStdString(), spec))
            return false;

        outName = root->getStringAttribute("name");
        outSpec = spec;
        return true;
    }

    // ==================================================
    // 订阅
    // ==================================================
    int ThemeLibrary::subscribe(std::function<void()> cb)
    {
        const int token = nextToken_++;
        listeners_.push_back({ token, std::move(cb) });
        return token;
    }

    void ThemeLibrary::unsubscribe(int token)
    {
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        {
            if (it->token == token) { listeners_.erase(it); return; }
        }
    }

    void ThemeLibrary::notifyListeners()
    {
        for (auto& l : listeners_)
            if (l.cb) l.cb();
    }
}
