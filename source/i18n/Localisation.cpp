#include "source/i18n/Localisation.h"
#include "source/ui/UiPrefs.h"
#include "Y2KI18nData.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

// ==========================================================
// Localisation.cpp —— 语言包注册表 / 解析 / 查表翻译 / 切换广播
//   详见头文件说明。
// ==========================================================
namespace y2k::i18n
{
    namespace
    {
        constexpr const char* kPrefLanguageKey = "language";
        constexpr const char* kSourceCode      = "en";
        constexpr size_t      kMaxCacheEntries = 8192;

        struct Pack
        {
            LanguageInfo      info;
            juce::StringArray matches;   // 系统语言前缀
            juce::String      fontSpec;  // 内嵌资源名 / 文件名 / "default"
            juce::File        baseDir;   // 外挂包所在目录（解析相对字体路径）
            std::unordered_map<juce::String, juce::String> entries;
        };

        struct Registry
        {
            std::vector<Pack>          packs;
            std::atomic<int>           current { 0 };
            std::atomic<juce::uint32>  generation { 1 };

            juce::SpinLock cacheLock;
            std::unordered_map<juce::String, juce::String> cache;

            struct Sub { int token; std::function<void()> cb; };
            std::vector<Sub> subs;
            int nextToken = 1;

            // 外挂字体文件数据（按完整路径缓存，只读一次）
            std::unordered_map<juce::String, std::unique_ptr<juce::MemoryBlock>> fileFonts;
        };

        Registry& reg()
        {
            static Registry r;
            return r;
        }

        // ------------------------------------------------------
        // .lang 解析
        // ------------------------------------------------------
        bool parseQuoted (juce::String::CharPointerType& p, juce::String& out)
        {
            if (*p != '"')
                return false;
            ++p;

            juce::String s;
            while (! p.isEmpty())
            {
                const juce_wchar c = p.getAndAdvance();
                if (c == '"')
                {
                    out = s;
                    return true;
                }

                if (c == '\\' && ! p.isEmpty())
                {
                    const juce_wchar e = p.getAndAdvance();
                    switch (e)
                    {
                        case 'n':  s += '\n'; break;
                        case 't':  s += '\t'; break;
                        case '"':  s += '"';  break;
                        case '\\': s += '\\'; break;
                        default:   s += '\\'; s += e; break;
                    }
                    continue;
                }

                s += c;
            }
            return false;   // 未闭合
        }

        void skipSpaces (juce::String::CharPointerType& p)
        {
            while (! p.isEmpty() && (*p == ' ' || *p == '\t'))
                ++p;
        }

        // "key" = "value" ["value 续"...]
        bool parseEntryLine (const juce::String& line, juce::String& key, juce::String& value)
        {
            auto p = line.getCharPointer();
            if (! parseQuoted (p, key))
                return false;

            skipSpaces (p);
            if (*p != '=')
                return false;
            ++p;
            skipSpaces (p);

            juce::String piece;
            if (! parseQuoted (p, piece))
                return false;
            value = piece;

            // 相邻引号段拼接（长文案可分段书写）
            for (;;)
            {
                skipSpaces (p);
                if (*p != '"')
                    break;
                if (! parseQuoted (p, piece))
                    break;
                value += piece;
            }
            return key.isNotEmpty();
        }

        Pack parsePack (const juce::String& text, const juce::File& baseDir)
        {
            Pack pk;
            pk.baseDir = baseDir;
            pk.info.fontScale = -1.0f;        // -1 = 未声明（合并时不覆盖）
            pk.info.minFontHeight = -1.0f;
            pk.info.order = -100000;

            juce::StringArray lines;
            lines.addLines (text);

            for (auto& raw : lines)
            {
                const auto line = raw.trim();
                if (line.isEmpty() || line.startsWithChar ('#') || line.startsWith ("//"))
                    continue;

                if (line.startsWithChar ('@'))
                {
                    const auto k = line.substring (1).upToFirstOccurrenceOf ("=", false, false).trim().toLowerCase();
                    const auto v = line.fromFirstOccurrenceOf ("=", false, false).trim().unquoted();

                    if      (k == "code")          pk.info.code = v;
                    else if (k == "name")          pk.info.nativeName = v;
                    else if (k == "label")         pk.info.buttonLabel = v;
                    else if (k == "order")         pk.info.order = v.getIntValue();
                    else if (k == "font")          pk.fontSpec = v;
                    else if (k == "fontscale")     pk.info.fontScale = juce::jlimit (0.3f, 3.0f, v.getFloatValue());
                    else if (k == "minfontheight") pk.info.minFontHeight = juce::jlimit (0.0f, 64.0f, v.getFloatValue());
                    else if (k == "match")
                    {
                        pk.matches.addTokens (v, ",;", "\"");
                        pk.matches.trim();
                        pk.matches.removeEmptyStrings();
                    }
                    continue;
                }

                juce::String key, value;
                if (parseEntryLine (line, key, value))
                    pk.entries.insert_or_assign (key, value);
            }

            return pk;
        }

        int findPack (const std::vector<Pack>& packs, const juce::String& code)
        {
            for (size_t i = 0; i < packs.size(); ++i)
                if (packs[i].info.code.equalsIgnoreCase (code))
                    return (int) i;
            return -1;
        }

        void mergePack (Registry& r, Pack&& pk)
        {
            if (pk.info.code.isEmpty())
                return;

            const int existing = findPack (r.packs, pk.info.code);
            if (existing < 0)
            {
                if (pk.info.nativeName.isEmpty())  pk.info.nativeName  = pk.info.code;
                if (pk.info.buttonLabel.isEmpty()) pk.info.buttonLabel = pk.info.code.toUpperCase().substring (0, 2);
                if (pk.info.fontScale < 0.0f)      pk.info.fontScale = 1.0f;
                if (pk.info.minFontHeight < 0.0f)  pk.info.minFontHeight = 0.0f;
                if (pk.info.order == -100000)      pk.info.order = 100;
                r.packs.push_back (std::move (pk));
                return;
            }

            // 覆盖合并：只覆盖声明过的元数据；条目逐条覆盖
            auto& dst = r.packs[(size_t) existing];
            if (pk.info.nativeName.isNotEmpty())  dst.info.nativeName  = pk.info.nativeName;
            if (pk.info.buttonLabel.isNotEmpty()) dst.info.buttonLabel = pk.info.buttonLabel;
            if (pk.info.order != -100000)         dst.info.order = pk.info.order;
            if (pk.info.fontScale >= 0.0f)        dst.info.fontScale = pk.info.fontScale;
            if (pk.info.minFontHeight >= 0.0f)    dst.info.minFontHeight = pk.info.minFontHeight;
            if (! pk.matches.isEmpty())           dst.matches = pk.matches;
            if (pk.fontSpec.isNotEmpty())
            {
                dst.fontSpec = pk.fontSpec;
                dst.baseDir  = pk.baseDir;
            }

            if (! dst.info.isSource)
                for (auto& kv : pk.entries)
                    dst.entries.insert_or_assign (kv.first, kv.second);
        }

        const char* findEmbeddedResource (const juce::String& spec, int& size)
        {
            size = 0;
            if (spec.isEmpty())
                return nullptr;

            if (auto* d = Y2KI18nData::getNamedResource (spec.toRawUTF8(), size))
                return d;

            // 也接受原始文件名（"ChillBitmap_16px.ttf"）
            for (int i = 0; i < Y2KI18nData::namedResourceListSize; ++i)
            {
                const char* name = Y2KI18nData::namedResourceList[i];
                if (const char* orig = Y2KI18nData::getNamedResourceOriginalFilename (name))
                    if (spec.equalsIgnoreCase (juce::String::fromUTF8 (orig)))
                        return Y2KI18nData::getNamedResource (name, size);
            }
            return nullptr;
        }

        int detectSystemLanguage (const std::vector<Pack>& packs)
        {
            const auto userLang    = juce::SystemStats::getUserLanguage();     // "zh"
            const auto displayLang = juce::SystemStats::getDisplayLanguage();  // "zh-CN" / "zh-Hans-CN"

            for (size_t i = 0; i < packs.size(); ++i)
            {
                if (packs[i].info.isSource)
                    continue;
                for (auto& m : packs[i].matches)
                    if (displayLang.startsWithIgnoreCase (m) || userLang.startsWithIgnoreCase (m))
                        return (int) i;
            }
            return -1;
        }

        void initialise (Registry& r)
        {
            // 1) 源语言（英文）：恒存在，不查表
            {
                Pack en;
                en.info.code        = kSourceCode;
                en.info.nativeName  = "English";
                en.info.buttonLabel = "EN";
                en.info.order       = 0;
                en.info.isSource    = true;
                en.fontSpec         = "default";
                r.packs.push_back (std::move (en));
            }

            // 2) 构建期内嵌的语言包
            for (int i = 0; i < Y2KI18nData::namedResourceListSize; ++i)
            {
                const char* name = Y2KI18nData::namedResourceList[i];
                const char* orig = Y2KI18nData::getNamedResourceOriginalFilename (name);
                if (orig == nullptr || ! juce::String::fromUTF8 (orig).endsWithIgnoreCase (".lang"))
                    continue;

                int size = 0;
                if (const char* data = Y2KI18nData::getNamedResource (name, size))
                    mergePack (r, parsePack (juce::String::createStringFromData (data, size), {}));
            }

            // 3) 运行期外挂语言包（覆盖 / 新增）
            const auto dir = getUserLanguageDirectory();
            if (dir.isDirectory())
            {
                for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.lang"))
                {
                    juce::MemoryBlock mb;
                    if (f.loadFileAsData (mb))
                        mergePack (r, parsePack (juce::String::createStringFromData (mb.getData(), (int) mb.getSize()),
                                                 f.getParentDirectory()));
                }
            }

            std::stable_sort (r.packs.begin(), r.packs.end(), [] (const Pack& a, const Pack& b)
            {
                if (a.info.isSource != b.info.isSource) return a.info.isSource;
                return a.info.order < b.info.order;
            });

            // 4) 当前语言：用户存档 > 系统语言自动匹配 > 英文
            int idx = -1;
            const auto saved = y2k::prefs::getString (kPrefLanguageKey, {});
            if (saved.isNotEmpty())
                idx = findPack (r.packs, saved);
            if (idx < 0 && saved.isEmpty())
                idx = detectSystemLanguage (r.packs);
            if (idx < 0)
                idx = findPack (r.packs, kSourceCode);

            r.current.store (juce::jmax (0, idx));
        }

        Registry& ready()
        {
            static std::once_flag once;
            auto& r = reg();
            std::call_once (once, [&r] { initialise (r); });
            return r;
        }

        const Pack& currentPack (Registry& r)
        {
            const int i = juce::jlimit (0, (int) r.packs.size() - 1, r.current.load());
            return r.packs[(size_t) i];
        }

        // ------------------------------------------------------
        // 查表
        // ------------------------------------------------------
        bool hasLatinLetter (const juce::String& s) noexcept
        {
            for (auto p = s.getCharPointer(); ! p.isEmpty();)
            {
                const juce_wchar c = p.getAndAdvance();
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                    return true;
            }
            return false;
        }

        // 把数字串替换为 {0} {1} ...（符号仅在前一个字符不是字母数字时并入）
        juce::String makeNumberTemplate (const juce::String& s, juce::StringArray& numbers)
        {
            juce::Array<juce_wchar> cs;
            for (auto p = s.getCharPointer(); ! p.isEmpty();)
                cs.add (p.getAndAdvance());

            auto isDigit = [] (juce_wchar c) { return c >= '0' && c <= '9'; };
            auto isAlnum = [] (juce_wchar c) { return juce::CharacterFunctions::isLetterOrDigit (c); };

            juce::String out;
            const int n = cs.size();
            int i = 0;
            while (i < n)
            {
                const juce_wchar c = cs[i];
                const bool signedNum = (c == '-' || c == '+') && i + 1 < n && isDigit (cs[i + 1])
                                       && (i == 0 || ! isAlnum (cs[i - 1]));

                if (isDigit (c) || signedNum)
                {
                    juce::String num;
                    if (signedNum) { num += c; ++i; }
                    while (i < n && isDigit (cs[i])) num += cs[i++];
                    if (i + 1 < n && cs[i] == '.' && isDigit (cs[i + 1]))
                    {
                        num += cs[i++];
                        while (i < n && isDigit (cs[i])) num += cs[i++];
                    }
                    out << "{" << numbers.size() << "}";
                    numbers.add (num);
                    continue;
                }

                out += c;
                ++i;
            }
            return out;
        }

        juce::String substitute (juce::String templ, const juce::StringArray& args)
        {
            for (int i = args.size(); --i >= 0;)
                templ = templ.replace ("{" + juce::String (i) + "}", args[i]);
            return templ;
        }

        bool lookup (const Pack& pk, const juce::String& key, juce::String& out)
        {
            const auto it = pk.entries.find (key);
            if (it == pk.entries.end())
                return false;
            out = it->second;
            return true;
        }

        // 单行：精确 → 去空白 → 数字模板
        bool translateLine (const Pack& pk, const juce::String& s, juce::String& out)
        {
            if (lookup (pk, s, out))
                return true;

            const auto t = s.trim();
            if (t.isEmpty() || ! hasLatinLetter (t))
                return false;

            const auto lead  = s.substring (0, s.length() - s.trimStart().length());
            const auto trail = s.substring (s.trimEnd().length());

            juce::String v;
            if (t != s && lookup (pk, t, v))
            {
                out = lead + v + trail;
                return true;
            }

            juce::StringArray nums;
            const auto templ = makeNumberTemplate (t, nums);
            if (! nums.isEmpty() && lookup (pk, templ, v))
            {
                out = lead + substitute (v, nums) + trail;
                return true;
            }
            return false;
        }

        juce::String translateUncached (const Pack& pk, const juce::String& s)
        {
            juce::String out;
            if (translateLine (pk, s, out))
                return out;

            // 多行：逐行翻译（空行 / 未命中行保持原样）
            if (s.containsChar ('\n'))
            {
                juce::String result;
                bool any = false;
                int start = 0;
                for (;;)
                {
                    const int nl = s.indexOfChar (start, '\n');
                    const auto line = nl < 0 ? s.substring (start) : s.substring (start, nl);

                    juce::String tl;
                    if (translateLine (pk, line, tl)) { result += tl; any = true; }
                    else                                result += line;

                    if (nl < 0) break;
                    result += '\n';
                    start = nl + 1;
                }
                if (any)
                    return result;
            }
            return s;
        }
    }

    // ==========================================================
    // 公共接口
    // ==========================================================
    int getNumLanguages()
    {
        return (int) ready().packs.size();
    }

    const LanguageInfo& getLanguage (int index)
    {
        auto& r = ready();
        return r.packs[(size_t) juce::jlimit (0, (int) r.packs.size() - 1, index)].info;
    }

    int indexOfLanguage (const juce::String& code)
    {
        return findPack (ready().packs, code);
    }

    const LanguageInfo& getCurrentLanguage()
    {
        auto& r = ready();
        return currentPack (r).info;
    }

    juce::String getCurrentLanguageCode()
    {
        return getCurrentLanguage().code;
    }

    bool isSourceLanguage()
    {
        return getCurrentLanguage().isSource;
    }

    juce::uint32 getGeneration() noexcept
    {
        return reg().generation.load();
    }

    void setLanguage (const juce::String& code)
    {
        auto& r = ready();
        const int idx = findPack (r.packs, code);
        if (idx < 0)
            return;

        // 即便与当前相同也写一次存档：用户的显式选择应覆盖"系统语言自动匹配"
        y2k::prefs::setString (kPrefLanguageKey, r.packs[(size_t) idx].info.code);

        if (idx == r.current.load())
            return;

        r.current.store (idx);
        ++r.generation;
        {
            const juce::SpinLock::ScopedLockType sl (r.cacheLock);
            r.cache.clear();
        }

        // 拷贝一份再回调：回调里允许订阅 / 退订
        const auto subs = r.subs;
        for (auto& s : subs)
        {
            if (! s.cb)
                continue;
            try { s.cb(); }
            catch (...) {}
        }
    }

    void cycleToNextLanguage()
    {
        auto& r = ready();
        if (r.packs.size() < 2)
            return;
        const int next = (r.current.load() + 1) % (int) r.packs.size();
        setLanguage (r.packs[(size_t) next].info.code);
    }

    juce::String translate (const juce::String& source)
    {
        if (source.isEmpty())
            return source;

        auto& r = ready();
        const auto& pk = currentPack (r);
        if (pk.info.isSource || pk.entries.empty() || ! hasLatinLetter (source))
            return source;

        const auto gen = r.generation.load();
        {
            const juce::SpinLock::ScopedLockType sl (r.cacheLock);
            const auto it = r.cache.find (source);
            if (it != r.cache.end())
                return it->second;
        }

        const auto result = translateUncached (pk, source);

        {
            const juce::SpinLock::ScopedLockType sl (r.cacheLock);
            if (gen == r.generation.load())
            {
                if (r.cache.size() >= kMaxCacheEntries)
                    r.cache.clear();
                r.cache.emplace (source, result);
            }
        }
        return result;
    }

    juce::String translateFormat (const juce::String& templ, const juce::StringArray& args)
    {
        auto& r = ready();
        const auto& pk = currentPack (r);

        juce::String t;
        if (pk.info.isSource || ! lookup (pk, templ, t))
            t = templ;
        return substitute (t, args);
    }

    bool getCurrentFontData (const void*& data, size_t& size, juce::String& fontId)
    {
        auto& r = ready();
        const auto& pk = currentPack (r);
        const auto spec = pk.fontSpec.trim();
        if (spec.isEmpty() || spec.equalsIgnoreCase ("default"))
            return false;

        int sz = 0;
        if (const char* d = findEmbeddedResource (spec, sz))
        {
            data   = d;
            size   = (size_t) sz;
            fontId = "res:" + spec;
            return sz > 0;
        }

        // 外挂字体文件（相对 .lang 所在目录，或绝对路径）
        const auto f = juce::File::isAbsolutePath (spec) ? juce::File (spec)
                                                         : pk.baseDir.getChildFile (spec);
        if (! f.existsAsFile())
            return false;

        const auto path = f.getFullPathName();
        auto it = r.fileFonts.find (path);
        if (it == r.fileFonts.end())
        {
            auto mb = std::make_unique<juce::MemoryBlock>();
            if (! f.loadFileAsData (*mb) || mb->getSize() == 0)
                return false;
            it = r.fileFonts.emplace (path, std::move (mb)).first;
        }

        data   = it->second->getData();
        size   = it->second->getSize();
        fontId = "file:" + path;
        return true;
    }

    int subscribe (std::function<void()> onLanguageChanged)
    {
        auto& r = ready();
        const int token = r.nextToken++;
        r.subs.push_back ({ token, std::move (onLanguageChanged) });
        return token;
    }

    void unsubscribe (int token)
    {
        auto& r = reg();
        r.subs.erase (std::remove_if (r.subs.begin(), r.subs.end(),
                                      [token] (const Registry::Sub& s) { return s.token == token; }),
                      r.subs.end());
    }

    juce::File getUserLanguageDirectory()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        dir = dir.getChildFile ("Application Support");
       #endif
        return dir.getChildFile ("Y2Kmeter").getChildFile ("lang");
    }
}
