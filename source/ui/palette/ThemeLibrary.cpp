// ==========================================================
// ThemeLibrary.cpp —— 我的主题库实现
// ==========================================================
#include "source/ui/palette/ThemeLibrary.h"
#include "source/ui/PinkXPStyle.h"

namespace PinkXP
{
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

    bool ThemeLibrary::saveTheme(const juce::String& name, const y2kpal::ThemeSpec& spec)
    {
        if (!isValidName(name))
            return false;

        const int existing = indexOfName(name);
        if (existing >= 0)
        {
            themes_[(size_t) existing].spec = spec;
            notifyListeners();
            return true;
        }

        if ((int) themes_.size() >= kMaxUserThemes)
            return false;   // 满员：面板提示用户先删除

        themes_.push_back({ name.trim(), spec });
        notifyListeners();
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
        notifyListeners();
        return true;
    }

    bool ThemeLibrary::removeTheme(int index)
    {
        if (index < 0 || index >= (int) themes_.size())
            return false;
        themes_.erase(themes_.begin() + index);
        notifyListeners();
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
        notifyListeners();
    }

    void ThemeLibrary::applyThemeAt(int index)
    {
        if (index < 0 || index >= (int) themes_.size())
            return;
        const auto& t = themes_[(size_t) index];
        PinkXP::applyUserTheme(t.spec, t.name);
    }

    // ---- XML（toXmlString / loadFromXmlString，Standalone 与 VST3 共用）----
    juce::String ThemeLibrary::toXmlString() const
    {
        juce::XmlElement root("Y2KT");
        root.setAttribute("libVer", 1);

        for (const auto& t : themes_)
        {
            auto* e = new juce::XmlElement("THEME");
            e->setAttribute("name", t.name);
            e->setAttribute("spec", juce::String(t.spec.shareCode()));
            root.addChildElement(e);
        }
        return root.toString();
    }

    bool ThemeLibrary::loadFromXmlString(const juce::String& xml)
    {
        themes_.clear();

        auto root = juce::parseXML(xml);
        if (root == nullptr || !root->hasTagName("Y2KT"))
            return false;

        for (auto* e : root->getChildIterator())
        {
            if (e->hasTagName("THEME") && (int) themes_.size() < kMaxUserThemes)
            {
                y2kpal::ThemeSpec spec;
                const juce::String code = e->getStringAttribute("spec");
                if (!y2kpal::ThemeSpec::fromShareCode(code.toStdString(), spec))
                    continue;
                themes_.push_back({ e->getStringAttribute("name"), spec });
            }
        }
        notifyListeners();
        return true;
    }

    bool ThemeLibrary::loadFromSettings(juce::PropertiesFile* settings)
    {
        if (settings == nullptr)
            return false;
        const juce::String xml = settings->getValue("ui.themeLibrary", {});
        if (xml.isEmpty())
            return false;
        return loadFromXmlString(xml);
    }

    bool ThemeLibrary::persistToSettings(juce::PropertiesFile* settings)
    {
        if (settings == nullptr)
            return false;
        settings->setValue("ui.themeLibrary", toXmlString());
        return true;
    }

    // ---- .y2kpalette 文件 ----
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

    // ---- 订阅 ----
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
