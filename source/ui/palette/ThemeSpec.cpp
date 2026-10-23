// ==========================================================
// ThemeSpec.cpp —— 序列化实现（纯 C++，不依赖 JUCE）
// ==========================================================
#include "source/ui/palette/ThemeSpec.h"
#include <vector>
#include <cstring>

namespace y2kpal
{
    namespace
    {
        // ---- 位流读写 ----
        class BitWriter
        {
        public:
            explicit BitWriter(size_t bytes) : data_(bytes, 0) {}

            void put(uint32_t value, int bits)
            {
                for (int i = 0; i < bits; ++i)
                {
                    if ((value >> i) & 1u)
                        data_[bitPos_ >> 3] |= (uint8_t) (1u << (bitPos_ & 7));
                    ++bitPos_;
                }
            }

            std::string finish() const { return std::string(data_.begin(), data_.end()); }

        private:
            std::vector<uint8_t> data_;
            size_t bitPos_ = 0;
        };

        class BitReader
        {
        public:
            explicit BitReader(const std::string& bin) : data_(bin.begin(), bin.end()) {}

            uint32_t get(int bits)
            {
                uint32_t v = 0;
                for (int i = 0; i < bits; ++i)
                {
                    const size_t byte = bitPos_ >> 3;
                    if (byte >= data_.size())
                        break;
                    if ((data_[byte] >> (bitPos_ & 7)) & 1u)
                        v |= (1u << i);
                    ++bitPos_;
                }
                return v;
            }

        private:
            std::vector<uint8_t> data_;
            size_t bitPos_ = 0;
        };

        // ---- Base64url（无 padding）----
        const char kBase64Table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

        std::string base64Encode(const std::string& bin)
        {
            std::string out;
            out.reserve((bin.size() + 2) / 3 * 4);
            for (size_t i = 0; i < bin.size(); i += 3)
            {
                const uint32_t a = (uint8_t) bin[i];
                const uint32_t b = (i + 1 < bin.size()) ? (uint8_t) bin[i + 1] : 0;
                const uint32_t c = (i + 2 < bin.size()) ? (uint8_t) bin[i + 2] : 0;
                const uint32_t v = (a << 16) | (b << 8) | c;
                out += kBase64Table[(v >> 18) & 63];
                out += kBase64Table[(v >> 12) & 63];
                if (i + 1 < bin.size()) out += kBase64Table[(v >> 6) & 63];
                if (i + 2 < bin.size()) out += kBase64Table[v & 63];
            }
            return out;
        }

        int base64Value(char ch)
        {
            if (ch >= 'A' && ch <= 'Z') return ch - 'A';
            if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
            if (ch >= '0' && ch <= '9') return ch - '0' + 52;
            if (ch == '-') return 62;
            if (ch == '_') return 63;
            return -1;
        }

        bool base64Decode(const std::string& text, std::string& out)
        {
            out.clear();
            uint32_t acc = 0;
            int      nbits = 0;
            for (char ch : text)
            {
                const int v = base64Value(ch);
                if (v < 0)
                    return false;
                acc = (acc << 6) | (uint32_t) v;
                nbits += 6;
                if (nbits >= 8)
                {
                    nbits -= 8;
                    out += (char) ((acc >> nbits) & 0xff);
                }
            }
            return true;
        }

        // 二进制布局（28 字节定长，见 ThemeSpec.h 说明）：
        //   12 hue | 2 brightness | 3 harmony | 3 pattern | 3 icon | 5 locks
        //   | 8 vibe | 8 tint | 7 x (1 set + 24 rgb) | 5 保留
        constexpr size_t kBinarySize = 28;
    }

    std::string ThemeSpec::toBinary() const
    {
        BitWriter w(kBinarySize);

        const uint32_t hue10 = (uint32_t) std::lroundf(clamp01(hue / 360.0f) * 3599.0f);
        w.put(hue10, 12);
        w.put((uint32_t) brightness, 2);
        w.put((uint32_t) harmony, 3);
        w.put((uint32_t) pattern, 3);
        w.put((uint32_t) (titleIcon & 7u), 3);
        w.put(lockHue ? 1u : 0u, 1);
        w.put(lockBrightness ? 1u : 0u, 1);
        w.put(lockVibe ? 1u : 0u, 1);
        w.put(lockHarmony ? 1u : 0u, 1);
        w.put(lockBgTint ? 1u : 0u, 1);
        w.put((uint32_t) std::lroundf(clamp01(vibe) * 255.0f), 8);
        w.put((uint32_t) std::lroundf(clamp01(bgTint) * 255.0f), 8);

        for (const auto& ov : overrides)
        {
            w.put(ov.set ? 1u : 0u, 1);
            w.put((uint32_t) ov.rgb.r, 8);
            w.put((uint32_t) ov.rgb.g, 8);
            w.put((uint32_t) ov.rgb.b, 8);
        }
        return w.finish();
    }

    bool ThemeSpec::fromBinary(const std::string& bin, ThemeSpec& out)
    {
        if (bin.size() < kBinarySize)
            return false;

        BitReader r(bin);
        out = ThemeSpec{};

        out.hue        = (float) r.get(12) * (360.0f / 3599.0f);
        out.brightness = (Brightness) std::min<uint32_t>(r.get(2), 2);
        out.harmony    = (Harmony)    std::min<uint32_t>(r.get(3), 4);
        out.pattern    = (Pattern)    std::min<uint32_t>(r.get(3), 6);
        out.titleIcon  = (uint8_t) r.get(3);

        out.lockHue        = r.get(1) != 0;
        out.lockBrightness = r.get(1) != 0;
        out.lockVibe       = r.get(1) != 0;
        out.lockHarmony    = r.get(1) != 0;
        out.lockBgTint     = r.get(1) != 0;

        out.vibe   = (float) r.get(8) / 255.0f;
        out.bgTint = (float) r.get(8) / 255.0f;

        for (auto& ov : out.overrides)
        {
            ov.set     = r.get(1) != 0;
            ov.rgb.r   = (uint8_t) r.get(8);
            ov.rgb.g   = (uint8_t) r.get(8);
            ov.rgb.b   = (uint8_t) r.get(8);
        }
        return true;
    }

    std::string ThemeSpec::shareCode() const
    {
        return "Y2KT1-" + base64Encode(toBinary());
    }

    bool ThemeSpec::fromShareCode(const std::string& code, ThemeSpec& out)
    {
        // 允许 `Y2KT1-` / `Y2KT1:` 前缀，也容忍无前缀的裸 base64
        std::string body = code;
        for (const char* prefix : { "Y2KT1-", "Y2KT1:" })
        {
            if (body.rfind(prefix, 0) == 0)
            {
                body = body.substr(std::strlen(prefix));
                break;
            }
        }

        std::string bin;
        if (!base64Decode(body, bin))
            return false;
        return fromBinary(bin, out);
    }

    const char* roleName(Role r)
    {
        switch (r)
        {
            case Role::Accent:     return "Accent";
            case Role::Accent2:    return "Accent 2";
            case Role::Background: return "Background";
            case Role::Surface:    return "Surface";
            case Role::Control:    return "Control";
            case Role::Text:       return "Text";
            case Role::OnAccent:   return "On Accent";
            default:               return "?";
        }
    }
}
