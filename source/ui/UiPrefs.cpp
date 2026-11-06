#include "source/ui/UiPrefs.h"

namespace y2k::prefs
{
    namespace
    {
        constexpr const char* kRootTag = "Y2KUiPrefs";

        std::unique_ptr<juce::XmlElement> loadRoot()
        {
            const auto f = getPrefsFile();
            if (f.existsAsFile())
                if (auto xml = juce::parseXML (f))
                    if (xml->hasTagName (kRootTag))
                        return xml;
            return {};
        }

        void writeAttribute (const char* key, const juce::String& value)
        {
            auto xml = loadRoot();
            if (xml == nullptr)
                xml = std::make_unique<juce::XmlElement> (kRootTag);

            xml->setAttribute (key, value);

            const auto f = getPrefsFile();
            const auto parent = f.getParentDirectory();
            if (! parent.isDirectory())
                parent.createDirectory();
            xml->writeTo (f);
        }
    }

    juce::File getPrefsFile()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        dir = dir.getChildFile ("Application Support");
       #endif
        return dir.getChildFile ("Y2Kmeter.uiPrefs.xml");
    }

    juce::String getString (const char* key, const juce::String& defaultValue)
    {
        if (auto xml = loadRoot())
            if (xml->hasAttribute (key))
                return xml->getStringAttribute (key);
        return defaultValue;
    }

    void setString (const char* key, const juce::String& value)
    {
        writeAttribute (key, value);
    }

    bool getBool (const char* key, bool defaultValue)
    {
        if (auto xml = loadRoot())
            if (xml->hasAttribute (key))
                return xml->getBoolAttribute (key, defaultValue);
        return defaultValue;
    }

    void setBool (const char* key, bool value)
    {
        writeAttribute (key, value ? "1" : "0");
    }
}
