#include "source/ui/modules/VirtuPetModule.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"

#include <cmath>
#include <random>

namespace
{
// v1.8.3：从任一 VirtuPet 面板查取父 workspace 的布局锁定态（与 ModulePanel.cpp 内
//   同名 helper 作用相同，但位于不同翻译单元，互不影响）。
bool isPanelLayoutLocked (const juce::Component& panel) noexcept
{
    if (auto* ws = dynamic_cast<const ModuleWorkspace*> (panel.getParentComponent()))
        return ws->isLayoutLocked();
    return false;
}

// 键控模式（绿幕 / 透明）下 Craft::drawPanelWell / drawBadgePill 会为普通仪表画布早退；
//   VirtuPet 悬停 HUD、对话气泡与确认弹窗属于显式交互 chrome，需按当前 SkinId（tokens.frameStyle）
//   忠实呈现对应视觉风格工艺（Pixel XP 0px 硬斜角 vs Modern 圆角微胶囊 vs Chrome/Glass/Neon/CRT）。
void drawKeyedPetCard (juce::Graphics& g,
                       juce::Rectangle<int> rect,
                       juce::Colour fill,
                       const PinkXP::SkinTokens& tokens)
{
    if (rect.isEmpty())
        return;

    juce::Graphics::ScopedSaveState ss (g);
    const bool isDark = PinkXP::face.getPerceivedBrightness() < 0.45f;

    switch (tokens.frameStyle)
    {
        case PinkXP::FrameStyle::pixelBevel:
            g.setColour (fill);
            g.fillRect (rect);
            g.setColour (PinkXP::shdw.withAlpha (isDark ? 0.62f : 0.48f));
            g.drawRect (rect, 1);
            if (rect.getWidth() >= 4 && rect.getHeight() >= 4)
            {
                g.setColour (PinkXP::hl.withAlpha (isDark ? 0.16f : 0.65f));
                g.fillRect (rect.getX() + 1, rect.getY() + 1, rect.getWidth() - 2, 1);
                g.fillRect (rect.getX() + 1, rect.getY() + 1, 1, rect.getHeight() - 2);
            }
            break;

        case PinkXP::FrameStyle::machinedChrome:
        {
            // 4-stop 拉丝金属面板折痕渐变 + 1px 暗 milled 边 + 1px 顶/左镜面铬沿
            juce::ColourGradient grad (fill.interpolatedWith (juce::Colours::white, isDark ? 0.14f : 0.24f),
                                       0.0f, (float) rect.getY(),
                                       fill.interpolatedWith (juce::Colours::black, isDark ? 0.12f : 0.08f),
                                       0.0f, (float) rect.getBottom(), false);
            grad.addColour (0.38, fill.interpolatedWith (juce::Colours::white, isDark ? 0.05f : 0.10f));
            grad.addColour (0.42, fill.interpolatedWith (juce::Colours::black, isDark ? 0.05f : 0.03f));
            g.setGradientFill (grad);
            g.fillRect (rect);
            g.setColour (PinkXP::dark.withAlpha (isDark ? 0.88f : 0.75f));
            g.drawRect (rect, 1);
            if (rect.getWidth() >= 4 && rect.getHeight() >= 4)
            {
                g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.48f)
                                       .withAlpha (isDark ? 0.28f : 0.72f));
                g.fillRect (rect.getX() + 1, rect.getY() + 1, rect.getWidth() - 2, 1);
                g.fillRect (rect.getX() + 1, rect.getY() + 1, 1, rect.getHeight() - 2);
            }
            break;
        }

        case PinkXP::FrameStyle::frostedGlass:
        {
            // Frosted Glass：6px 圆角晶透亚克力 + 顶部 40% 玻璃穹顶反光 + 1px 亮白顶沿
            const auto rf = rect.toFloat();
            const float rad = juce::jmin (tokens.cardRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.45f);
            g.setColour (fill);
            g.fillRoundedRectangle (rf, rad);
            if (rf.getHeight() >= 10.0f)
            {
                auto sheen = rf.reduced (1.5f);
                sheen.setHeight (sheen.getHeight() * 0.40f);
                juce::ColourGradient dome (juce::Colours::white.withAlpha (isDark ? 0.16f : 0.34f),
                                           0.0f, sheen.getY(),
                                           juce::Colours::white.withAlpha (0.02f),
                                           0.0f, sheen.getBottom(), false);
                g.setGradientFill (dome);
                g.fillRoundedRectangle (sheen, juce::jmax (1.0f, rad - 1.5f));
            }
            g.setColour (PinkXP::dark.withAlpha (isDark ? 0.40f : 0.32f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            if (rect.getWidth() >= 10 && rect.getHeight() >= 8)
            {
                const int inset = juce::jmax (3, (int) std::round (rad * 0.60f));
                g.setColour (juce::Colours::white.withAlpha (isDark ? 0.30f : 0.72f));
                g.fillRect (rect.getX() + inset, rect.getY() + 1, juce::jmax (1, rect.getWidth() - inset * 2), 1);
            }
            break;
        }

        case PinkXP::FrameStyle::neonTube:
        {
            // Neon Cyber：2px 科技微角暗烟熏 HUD 面板 + 双通道发光霓虹管边 + 四角发光角标
            const auto rf = rect.toFloat();
            const float rad = juce::jmin (2.0f, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.25f);
            g.setColour (fill);
            g.fillRoundedRectangle (rf, rad);
            g.setColour (PinkXP::sel.withAlpha (0.32f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 2.0f);
            g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.32f).withAlpha (0.88f));
            g.drawRoundedRectangle (rf.reduced (1.0f), juce::jmax (0.5f, rad - 0.5f), 1.0f);
            PinkXP::drawPixelCorners (g, rect.reduced (1),
                                      PinkXP::sel.interpolatedWith (juce::Colours::white, 0.52f), 4, 1);
            break;
        }

        case PinkXP::FrameStyle::crtPhosphor:
            // CRT Terminal：0px 锐利荧光终端卡片 + 1px 荧光边 + 微荧光内细线
            g.setColour (fill);
            g.fillRect (rect);
            g.setColour (PinkXP::sel.withAlpha (0.82f));
            g.drawRect (rect, 1);
            if (rect.getWidth() >= 6 && rect.getHeight() >= 6)
            {
                g.setColour (PinkXP::sel.withAlpha (0.20f));
                g.drawRect (rect.reduced (1), 1);
            }
            break;

        case PinkXP::FrameStyle::modernStudio:
        default:
        {
            const auto rf = rect.toFloat();
            const float rad = juce::jmin (tokens.cardRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.45f);
            g.setColour (fill);
            g.fillRoundedRectangle (rf, rad);
            g.setColour (PinkXP::skinHairlineOn (PinkXP::face, 0.20f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            break;
        }
    }
}

void drawKeyedPetBadge (juce::Graphics& g,
                        juce::Rectangle<int> rect,
                        juce::Colour fill,
                        const PinkXP::SkinTokens& tokens)
{
    if (rect.isEmpty())
        return;

    juce::Graphics::ScopedSaveState ss (g);
    switch (tokens.frameStyle)
    {
        case PinkXP::FrameStyle::pixelBevel:
            g.setColour (fill);
            g.fillRect (rect);
            g.setColour (PinkXP::hl);
            g.fillRect (rect.getX(), rect.getY(), rect.getWidth(), 1);
            g.fillRect (rect.getX(), rect.getY(), 1, rect.getHeight());
            g.setColour (PinkXP::dark);
            g.fillRect (rect.getX(), rect.getBottom() - 1, rect.getWidth(), 1);
            g.fillRect (rect.getRight() - 1, rect.getY(), 1, rect.getHeight());
            break;

        case PinkXP::FrameStyle::machinedChrome:
        {
            // 拉丝铝 HNG/HP 迷你铭牌（4-stop 折痕渐变 + 1px 暗边 + 顶/左镜面铬光）
            const bool isDark = fill.getPerceivedBrightness() < 0.45f;
            juce::ColourGradient grad (fill.interpolatedWith (juce::Colours::white, isDark ? 0.18f : 0.28f),
                                       0.0f, (float) rect.getY(),
                                       fill.darker (0.14f),
                                       0.0f, (float) rect.getBottom(), false);
            grad.addColour (0.38, fill.interpolatedWith (juce::Colours::white, 0.08f));
            grad.addColour (0.42, fill);
            g.setGradientFill (grad);
            g.fillRect (rect);
            g.setColour (PinkXP::dark);
            g.drawRect (rect, 1);
            if (rect.getWidth() >= 4 && rect.getHeight() >= 4)
            {
                g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.50f)
                                       .withAlpha (isDark ? 0.36f : 0.78f));
                g.fillRect (rect.getX() + 1, rect.getY() + 1, rect.getWidth() - 2, 1);
                g.fillRect (rect.getX() + 1, rect.getY() + 1, 1, rect.getHeight() - 2);
            }
            break;
        }

        case PinkXP::FrameStyle::neonTube:
        {
            const auto rf = rect.toFloat();
            const float rad = juce::jmin (2.0f, rf.getHeight() * 0.30f);
            g.setColour (fill.interpolatedWith (juce::Colours::black, 0.22f));
            g.fillRoundedRectangle (rf, rad);
            g.setColour (PinkXP::sel.withAlpha (0.34f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.8f);
            g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.30f).withAlpha (0.86f));
            g.drawRoundedRectangle (rf.reduced (0.75f), juce::jmax (0.5f, rad - 0.5f), 0.9f);
            break;
        }

        case PinkXP::FrameStyle::crtPhosphor:
            g.setColour (fill);
            g.fillRect (rect);
            g.setColour (PinkXP::sel.withAlpha (0.84f));
            g.drawRect (rect, 1);
            break;

        case PinkXP::FrameStyle::frostedGlass:
        {
            const auto rf = rect.toFloat();
            const float rad = juce::jmin (tokens.badgeRadius, rf.getHeight() * 0.5f);
            g.setColour (fill);
            g.fillRoundedRectangle (rf, rad);
            if (rf.getHeight() >= 8.0f)
            {
                auto sheen = rf.reduced (1.0f);
                sheen.setHeight (sheen.getHeight() * 0.44f);
                g.setColour (juce::Colours::white.withAlpha (0.28f));
                g.fillRoundedRectangle (sheen, juce::jmax (1.0f, rad - 1.0f));
            }
            g.setColour (PinkXP::dark.withAlpha (0.34f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            break;
        }

        case PinkXP::FrameStyle::modernStudio:
        default:
        {
            const auto rf = rect.toFloat();
            const float rad = juce::jmin (tokens.badgeRadius, rf.getHeight() * 0.5f);
            g.setColour (fill);
            g.fillRoundedRectangle (rf, rad);
            g.setColour (PinkXP::skinHairlineOn (fill, 0.22f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            break;
        }
    }
}

void drawKeyedPetTrough (juce::Graphics& g,
                         juce::Rectangle<int> rect,
                         juce::Colour fill,
                         const PinkXP::SkinTokens& tokens,
                         bool isDarkFace)
{
    if (rect.isEmpty())
        return;

    juce::Graphics::ScopedSaveState ss (g);
    switch (tokens.frameStyle)
    {
        case PinkXP::FrameStyle::pixelBevel:
            g.setColour (fill);
            g.fillRect (rect);
            g.setColour (PinkXP::dark);
            g.fillRect (rect.getX(), rect.getY(), rect.getWidth(), 1);
            g.fillRect (rect.getX(), rect.getY(), 1, rect.getHeight());
            g.setColour (PinkXP::hl.withAlpha (0.75f));
            g.fillRect (rect.getX() + 1, rect.getBottom() - 1, rect.getWidth() - 1, 1);
            g.fillRect (rect.getRight() - 1, rect.getY() + 1, 1, rect.getHeight() - 1);
            break;

        case PinkXP::FrameStyle::machinedChrome:
            // CNC 机加工凹槽（1px 暗边 + 2px 顶内阴影 + 1px 底部微铬反光沿）
            g.setColour (fill);
            g.fillRect (rect);
            g.setColour (PinkXP::dark);
            g.drawRect (rect, 1);
            if (rect.getWidth() >= 4 && rect.getHeight() >= 5)
            {
                g.setColour (juce::Colours::black.withAlpha (0.34f));
                g.fillRect (rect.getX() + 1, rect.getY() + 1, rect.getWidth() - 2, 1);
                if (rect.getHeight() >= 7)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.14f));
                    g.fillRect (rect.getX() + 1, rect.getY() + 2, rect.getWidth() - 2, 1);
                }
                g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.45f).withAlpha (0.45f));
                g.fillRect (rect.getX() + 1, rect.getBottom() - 2, rect.getWidth() - 2, 1);
            }
            break;

        case PinkXP::FrameStyle::neonTube:
        {
            const auto rf = rect.toFloat();
            const float rad = juce::jmin (2.0f, rf.getHeight() * 0.30f);
            g.setColour (fill);
            g.fillRoundedRectangle (rf, rad);
            g.setColour (PinkXP::sel.withAlpha (0.30f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.8f);
            g.setColour (PinkXP::sel.withAlpha (0.82f));
            g.drawRoundedRectangle (rf.reduced (0.75f), juce::jmax (0.5f, rad - 0.5f), 0.9f);
            break;
        }

        case PinkXP::FrameStyle::crtPhosphor:
            g.setColour (fill);
            g.fillRect (rect);
            g.setColour (PinkXP::sel.withAlpha (0.78f));
            g.drawRect (rect, 1);
            break;

        case PinkXP::FrameStyle::frostedGlass:
        {
            const auto rf = rect.toFloat();
            const float rad = juce::jmin (4.5f, rf.getHeight() * 0.5f);
            g.setColour (fill);
            g.fillRoundedRectangle (rf, rad);
            g.setColour (PinkXP::dark.withAlpha (0.36f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            if (rect.getWidth() >= 8 && rect.getHeight() >= 6)
            {
                g.setColour (juce::Colours::white.withAlpha (isDarkFace ? 0.20f : 0.45f));
                g.fillRect (rect.getX() + 3, rect.getBottom() - 2, rect.getWidth() - 6, 1);
            }
            break;
        }

        case PinkXP::FrameStyle::modernStudio:
        default:
        {
            const auto rf = rect.toFloat();
            const float rad = juce::jmin (tokens.barCellRadius + 2.0f, rf.getHeight() * 0.5f);
            g.setColour (fill);
            g.fillRoundedRectangle (rf, rad);
            g.setColour (PinkXP::skinHairlineOn (fill, isDarkFace ? 0.20f : 0.22f));
            g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            break;
        }
    }
}

#if defined(__APPLE__)
// macOS .app / .vst3 / .component bundle 资源路径辅助函数
//   可执行文件路径：<bundle>/Contents/MacOS/<name>
//   资源路径：      <bundle>/Contents/Resources/assets/
//   IDE 直跑时 CWD=项目根目录，assets/ 直接可访问；
//   但 DMG 安装到 /Applications 后从启动台或 Finder 启动时 CWD=/，
//   必须额外搜索 bundle 内部 Contents/Resources/assets/ 路径。
//
// 注意：本函数返回的是 Contents/Resources/（即"assets/"的父目录），
//   因为下方 findVirtuPet*Root() 里 tryFromBase 会在 base 上再拼一次
//   "assets"，若这里返回 ".../Resources/assets"，就会变成
//   ".../Resources/assets/assets/VirtuPet/..."，多一层 assets 导致找不到
//   （这是 v2.5.x 出现"assets/VirtuPet not found"的根因）。
//   currentApplicationFile 在 .app 里指向 bundle 根目录，其内
//   Contents/Resources/assets/ 才是真实资源目录。
static juce::File findBundleResourcesBaseDir()
{
    auto appFile = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    // 情况 A：Standalone 从启动台/Finder 启动，currentApplicationFile
    //         指向 Y2Kmeter.app 本身。
    auto contentsResources = appFile.getChildFile("Contents").getChildFile("Resources");
    if (contentsResources.isDirectory())
        return contentsResources;

    // 情况 B：从 IDE 直跑或其它嵌入场景，currentApplicationFile 可能
    //         指向可执行文件本身：<bundle>/Contents/MacOS/Y2Kmeter。
    //         向上两级得到 Contents/，再进 Resources/。
    auto viaExe = appFile.getParentDirectory()   // MacOS/
                         .getParentDirectory()   // Contents/
                         .getChildFile("Resources");
    if (viaExe.isDirectory())
        return viaExe;

    return {};
}
#endif

juce::File findVirtuPetAssetsRoot()

{
    auto tryFromBase = [] (juce::File base) -> juce::File
    {
        for (int i = 0; i < 10 && base.exists(); ++i)
        {
            auto roleCut = base.getChildFile ("assets")
                               .getChildFile ("VirtuPet")
                               .getChildFile ("role_cut_by_xlsx_40x40");
            if (roleCut.isDirectory())
                return roleCut;

            base = base.getParentDirectory();
        }
        return {};
    };

    if (auto f = tryFromBase (juce::File::getCurrentWorkingDirectory()); f.isDirectory())
        return f;

    if (auto f = tryFromBase (juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                                  .getParentDirectory()); f.isDirectory())
        return f;

#if defined(__APPLE__)
    if (auto f = tryFromBase (findBundleResourcesBaseDir()); f.isDirectory())
        return f;
#endif

    return {};
}

juce::File findVirtuPetMirrorAssetsRoot()

{
    auto tryFromBase = [] (juce::File base) -> juce::File
    {
        for (int i = 0; i < 10 && base.exists(); ++i)
        {
            auto roleCut = base.getChildFile ("assets")
                               .getChildFile ("VirtuPet")
                               .getChildFile ("role_cut_by_xlsx_40x40_fan");
            if (roleCut.isDirectory())
                return roleCut;

            base = base.getParentDirectory();
        }
        return {};
    };

    if (auto f = tryFromBase (juce::File::getCurrentWorkingDirectory()); f.isDirectory())
        return f;

    if (auto f = tryFromBase (juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                                  .getParentDirectory()); f.isDirectory())
        return f;

#if defined(__APPLE__)
    if (auto f = tryFromBase (findBundleResourcesBaseDir()); f.isDirectory())
        return f;
#endif

    return {};
}

juce::File findVirtuPetRolePngDir()
{
    auto tryFromBase = [] (juce::File base) -> juce::File
    {
        for (int i = 0; i < 10 && base.exists(); ++i)
        {
            auto roleDir = base.getChildFile ("assets")
                               .getChildFile ("VirtuPet")
                               .getChildFile ("role");
            if (roleDir.isDirectory())
                return roleDir;

            base = base.getParentDirectory();
        }
        return {};
    };

    if (auto f = tryFromBase (juce::File::getCurrentWorkingDirectory()); f.isDirectory())
        return f;

    if (auto f = tryFromBase (juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                                  .getParentDirectory()); f.isDirectory())
        return f;

#if defined(__APPLE__)
    if (auto f = tryFromBase (findBundleResourcesBaseDir()); f.isDirectory())
        return f;
#endif

    return {};
}

juce::File findVirtuPetEggAssetsDir()
{
    auto tryFromBase = [] (juce::File base) -> juce::File
    {
        for (int i = 0; i < 10 && base.exists(); ++i)
        {
            auto eggDir = base.getChildFile ("assets")
                              .getChildFile ("VirtuPet")
                              .getChildFile ("egg_38x38");
            if (eggDir.isDirectory())
                return eggDir;

            base = base.getParentDirectory();
        }
        return {};
    };

    if (auto f = tryFromBase (juce::File::getCurrentWorkingDirectory()); f.isDirectory())
        return f;

    if (auto f = tryFromBase (juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                                  .getParentDirectory()); f.isDirectory())
        return f;

#if defined(__APPLE__)
    if (auto f = tryFromBase (findBundleResourcesBaseDir()); f.isDirectory())
        return f;
#endif

    return {};
}

bool loadRandomEggFrames (juce::Array<juce::Image>& outEggFrames, int& outStyleId)
{
    outEggFrames.clearQuick();
    outStyleId = 1;

    const auto eggDir = findVirtuPetEggAssetsDir();
    if (! eggDir.isDirectory())
        return false;

    constexpr int styleCount = 8;
    constexpr int frameCount = 4;
    juce::Array<int> styleOrder;
    for (int i = 1; i <= styleCount; ++i)
        styleOrder.add (i);

    for (int i = styleOrder.size() - 1; i > 0; --i)
    {
        const int j = juce::Random::getSystemRandom().nextInt (i + 1);
        styleOrder.swap (i, j);
    }

    for (const int styleId : styleOrder)
    {
        juce::Array<juce::Image> candidateFrames;
        bool validStyle = true;

        for (int frame = 1; frame <= frameCount; ++frame)
        {
            const int seq = (styleId - 1) * frameCount + (frame - 1);
            const auto fileName = juce::String::formatted ("egg_%03d_%d_%d.png", seq, styleId, frame);
            const auto image = juce::ImageFileFormat::loadFrom (eggDir.getChildFile (fileName));
            if (image.isNull())
            {
                validStyle = false;
                break;
            }

            candidateFrames.add (image);
        }

        if (validStyle && candidateFrames.size() >= frameCount)
        {
            outEggFrames = candidateFrames;
            outStyleId = styleId;
            return true;
        }
    }

    return false;
}

bool loadRoleFramesFromDirectory (const juce::File& roleDir,
                                  const juce::File& mirrorRootDir,
                                  std::array<juce::Array<juce::Image>, 33>& animFrames,
                                  std::array<juce::Array<juce::Image>, 33>& animFramesRight,
                                  juce::Array<int>& availableAnimIds)
{
    if (! roleDir.isDirectory())
        return false;

    for (auto& arr : animFrames)
        arr.clearQuick();
    for (auto& arr : animFramesRight)
        arr.clearQuick();
    availableAnimIds.clearQuick();

    auto appendFramesFromDir = [] (const juce::File& dir,
                                   bool forceRight,
                                   std::array<juce::Array<juce::Image>, 33>& outLeft,
                                   std::array<juce::Array<juce::Image>, 33>& outRight)
    {
        if (! dir.isDirectory())
            return;

        juce::Array<juce::File> pngFiles;
        dir.findChildFiles (pngFiles, juce::File::findFiles, false, "*.png");

        for (const auto& pf : pngFiles)
        {
            auto src = juce::ImageFileFormat::loadFrom (pf);
            if (src.isNull())
                continue;

            const auto stem = pf.getFileNameWithoutExtension();
            juce::StringArray tokens;
            tokens.addTokens (stem, "_", "");
            if (tokens.size() < 4)
                continue;

            const int animId = tokens[tokens.size() - 2].getIntValue();
            if (animId < 1 || animId > 33)
                continue;

            bool isRightVariant = forceRight;
            if (! forceRight)
            {
                const juce::String lowerStem = stem.toLowerCase();
                isRightVariant = lowerStem.contains ("_right")
                              || lowerStem.contains ("_r")
                              || lowerStem.contains ("_mirror")
                              || lowerStem.contains ("_mirrored")
                              || lowerStem.contains ("_flip")
                              || lowerStem.contains ("_flipped");
            }

            if (isRightVariant)
                outRight[(size_t) (animId - 1)].add (src);
            else
                outLeft[(size_t) (animId - 1)].add (src);
        }
    };

    appendFramesFromDir (roleDir, false, animFrames, animFramesRight);

    if (mirrorRootDir.isDirectory())
    {
        const auto mirrorRoleDir = mirrorRootDir.getChildFile (roleDir.getFileName());
        appendFramesFromDir (mirrorRoleDir, true, animFrames, animFramesRight);
    }

    for (int i = 0; i < 33; ++i)
        if (animFrames[(size_t) i].size() > 0 || animFramesRight[(size_t) i].size() > 0)
            availableAnimIds.addIfNotAlreadyThere (i + 1);

    return ! availableAnimIds.isEmpty();
}

juce::String randomIdleSpeech()
{
    static const juce::StringArray lines {
        "Hmm... I should keep practicing my idle face.",
        "I wonder what's for dinner today.",
        "Nice breeze. Let me zone out for a bit.",
        "Please wait, I'm thinking about life.",
        "Should I go find a snack now?"
    };

    if (lines.isEmpty())
        return {};

    return lines[juce::Random::getSystemRandom().nextInt (lines.size())];
}

} // namespace

VirtuPetModule::VirtuPetModule()
    : ModulePanel (ModuleType::virtuPet)
{
    setDefaultSize (128, 128);
    setMinSize (minW, minH);

    // Toybox：整块排除（点击语义已被占用，§6.1）
    getProperties().set ("toy.optOut", true);

    // ---- 拖影 / focus 残留 修复（最终版）----
    //
    // 本模块 paint() 有大量非完整覆盖的区域（playArea 空白、focus rect 外的
    // 非聚焦帧、精灵透明通道）—— 必须声明为 non-opaque，让 JUCE 在每次
    // dirty repaint 时安排父组件（ModuleWorkspace）先重绘相同区域的底色（
    // content.withAlpha(0.70f) 透出桌面），再把本组件 paint 叠上来。
    //
    // 但 JUCE 的 repaint(localRect) 对 non-opaque 子组件并不会自动追质到根，
    // 当父组件也 non-opaque（ModuleWorkspace 本身就是）时，旧像素仍会留在
    // 缓冲中。解决方式：所有从本类发起的重绘均通过 repaintSelfAndParent()
    // 统一送出，其内部同时 getParentComponent()->repaint(parentRect) + repaint(rect)，
    // 来确保底色真的被重新绘制。
    //
    // 注：这会覆盖 ModulePanel 构造器默认的 setOpaque(true)。
    setOpaque (false);

    currentPatrolAction = PatrolAction::lookLeftSlow;
    patrolActionTicksRemaining = 0;
    patrolCooldownTicksRemaining = patrolCycleTicks;
    patrolLockedAnimId = 0;
    patrolFaceLeft = true;
    patrolActionMoveDir = 0.0f;
    patrolActionSpeedPxPerTick = 0.0f;

    showSpeechBubble = false;
    jumpFightActive = false;

    loadRandomRoleAnimations();

    currentVisualHz = getTargetVisualHzForMode (motionMode);
    currentTickDtSec = 1.0f / (float) juce::jmax (1, currentVisualHz);
    startTimerHz (currentVisualHz);
}

VirtuPetModule::~VirtuPetModule()
{
    stopTimer();
}

void VirtuPetModule::setFocusVisual (bool shouldFocus)

{
    if (focused == shouldFocus)
        return;

    focused = shouldFocus;
    if (! focused)
    {
        deleteBtnHovered = false;
        deleteBtnPressed = false;
        dismissConfirmOverlay();
        if (dragMode == DragMode::none)
            setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    repaintSelfAndParent();
}

void VirtuPetModule::paint (juce::Graphics& g)

{
    const auto& tokens = PinkXP::getSkinTokens();
    const bool hideChromaChrome = PinkXP::isChromaKeySkin() && ! PinkXP::isChromaRevealed (*this);
    auto bounds = getLocalBounds();

    if (! hideChromaChrome)
    {
        auto hud = getHudBounds();
        auto barRow = hud.removeFromTop (juce::jmin (16, hud.getHeight()));
        if (focused)
        {
            // 聚焦态右侧保留 [×] 删除按钮空间、左侧避开聚焦边框，避免与 HP 条重叠
            barRow.removeFromLeft (2);
            barRow.removeFromRight (deleteButtonSize + 4);
        }

        const int halfHudW = barRow.getWidth() / 2;
        auto hungerBounds = barRow.withWidth (halfHudW).reduced (2, 2);
        auto healthBounds = barRow.withTrimmedLeft (halfHudW).reduced (2, 2);

        drawPixelBar (g, hungerBounds, hunger / 100.0f, juce::Colour (0xfff39c3d), "HNG");
        drawPixelBar (g, healthBounds, health / 100.0f, juce::Colour (0xff5bd48a), "HP");
    }

    bounds.removeFromTop (juce::jmin (hudHeight, bounds.getHeight()));

    const auto frame = getCurrentFrame();
    if (frame.isNull())
    {
        g.setColour (PinkXP::ink.withAlpha (0.7f));
        g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
        g.drawText (y2k::tr ("VirtuPet"), bounds.removeFromTop (18), juce::Justification::centred, false);

        g.setColour (PinkXP::ink.withAlpha (0.55f));
        g.setFont (PinkXP::getFont (9.0f));
        g.drawFittedText (y2k::tr ("assets/VirtuPet not found"), bounds.reduced (4), juce::Justification::centred, 2);
    }
    else
    {
        // 反馈修复（外网 #2）：绘制坐标与脏区计算（getPetVisualBoundsFor）统一
        // 使用 floor 取整，避免 round/floor 混用在动画帧切换时产生 1px 脏区错位，
        // 旧帧边缘未被清干净而残留成细黑线。
        const int x = (int) std::floor (petPos.x);
        const int y = (int) std::floor (petPos.y);

        const bool shouldMirrorPatrol = (motionMode == MotionMode::patrol)
                                     && patrolMirrorX
                                     && ! hasRightVariantFrames (currentAnimId);
        if (shouldMirrorPatrol)

        {
            const auto t = juce::AffineTransform::scale (-1.0f, 1.0f)
                               .translated ((float) (x + frame.getWidth()), (float) y);
            g.drawImageTransformed (frame, t, false);
        }

        else
        {
            g.drawImageAt (frame, x, y);
        }

        if (motionMode == MotionMode::patrol && showSpeechBubble && currentSpeechText.isNotEmpty())
        {
            const auto bubbleFont = PinkXP::getFont (8.5f, juce::Font::plain);
            const int bubblePaddingX = 6;
            const int bubblePaddingY = 3;
            const int bubbleMinW = 36;
            const int bubbleMaxW = juce::jmax (bubbleMinW, getWidth() - 6);
            const int maxTextW = juce::jmax (16, bubbleMaxW - bubblePaddingX * 2);
            const int singleLineW = juce::jmax (16, bubbleFont.getStringWidth (y2k::tr (currentSpeechText)));
            const int textW = juce::jlimit (16, maxTextW, singleLineW);

            const auto bubbleFill = PinkXP::isGreenScreenBackdrop() ? PinkXP::content : PinkXP::content.withAlpha (0.95f);
            const auto bubbleInk  = PinkXP::readableInkOn (bubbleFill);

            juce::AttributedString bubbleText;
            bubbleText.setJustification (juce::Justification::centred);
            bubbleText.setWordWrap (juce::AttributedString::WordWrap::byWord);
            bubbleText.append (y2k::tr (currentSpeechText), bubbleFont, bubbleInk);

            juce::TextLayout textLayout;
            textLayout.createLayout (bubbleText, (float) textW);

            const int textH = juce::jmax ((int) std::ceil (bubbleFont.getHeight()), (int) std::ceil (textLayout.getHeight()));
            const int bubbleW = juce::jlimit (bubbleMinW, bubbleMaxW, textW + bubblePaddingX * 2);
            const int bubbleH = juce::jmax (14, textH + bubblePaddingY * 2);

            auto bubble = juce::Rectangle<int> ((int) x + frame.getWidth() / 2 - bubbleW / 2,
                                                (int) y - bubbleH - 4,
                                                bubbleW,
                                                bubbleH);

            bubble = bubble.withX (juce::jlimit (0, juce::jmax (0, getWidth() - bubble.getWidth()), bubble.getX()));
            bubble = bubble.withY (juce::jlimit (0, juce::jmax (0, getHeight() - bubble.getHeight()), bubble.getY()));

            const int tailX = juce::jlimit (bubble.getX() + 5,
                                            juce::jmax (bubble.getX() + 5, bubble.getRight() - 6),
                                            x + frame.getWidth() / 2);
            const bool drawTail = (bubble.getBottom() + 3 <= y) && (bubble.getWidth() >= 18);

            if (tokens.frameStyle == PinkXP::FrameStyle::pixelBevel)
            {
                // Pixel XP：0px 复古像素漫画对话框（2px 硬像素框 + 阶梯像素尾，零圆角）
                if (! PinkXP::isGreenScreenBackdrop())
                {
                    g.setColour (PinkXP::dark.withAlpha (0.28f));
                    g.fillRect (bubble.translated (2, 2));
                }

                g.setColour (bubbleFill);
                g.fillRect (bubble);

                // 2px 硬边框
                g.setColour (PinkXP::dark);
                g.fillRect (bubble.getX(), bubble.getY(), bubble.getWidth(), 2);
                g.fillRect (bubble.getX(), bubble.getBottom() - 2, bubble.getWidth(), 2);
                g.fillRect (bubble.getX(), bubble.getY(), 2, bubble.getHeight());
                g.fillRect (bubble.getRight() - 2, bubble.getY(), 2, bubble.getHeight());

                // 内层 1px 复古高光
                if (bubble.getWidth() >= 8 && bubble.getHeight() >= 8)
                {
                    g.setColour (PinkXP::hl.withAlpha (0.80f));
                    g.fillRect (bubble.getX() + 2, bubble.getY() + 2, bubble.getWidth() - 4, 1);
                    g.fillRect (bubble.getX() + 2, bubble.getY() + 2, 1, bubble.getHeight() - 4);
                }

                // 阶梯像素对话尾
                if (drawTail)
                {
                    const int ty = bubble.getBottom() - 2;
                    g.setColour (bubbleFill);
                    g.fillRect (tailX - 2, ty,     4, 2);
                    g.fillRect (tailX - 1, ty + 2, 3, 1);
                    g.fillRect (tailX,     ty + 3, 2, 1);

                    g.setColour (PinkXP::dark);
                    g.fillRect (tailX - 3, ty,     1, 2);
                    g.fillRect (tailX + 2, ty,     1, 2);
                    g.fillRect (tailX - 2, ty + 2, 1, 1);
                    g.fillRect (tailX + 2, ty + 2, 1, 1);
                    g.fillRect (tailX - 1, ty + 3, 1, 1);
                    g.fillRect (tailX + 2, ty + 3, 1, 1);
                    g.fillRect (tailX,     ty + 4, 2, 1);
                }
            }
            else if (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
            {
                // CRT Terminal：0px 复古荧光终端对话框 + 轻扫描线 + 阶梯像素尾
                g.setColour (bubbleFill);
                g.fillRect (bubble);
                if (! PinkXP::isGreenScreenBackdrop())
                {
                    g.setColour (juce::Colours::black.withAlpha (0.10f));
                    for (int py = bubble.getY() + 2; py < bubble.getBottom() - 1; py += 2)
                        g.fillRect (bubble.getX() + 1, py, bubble.getWidth() - 2, 1);
                }
                g.setColour (PinkXP::sel);
                g.drawRect (bubble, 1);
                if (drawTail)
                {
                    const int ty = bubble.getBottom() - 1;
                    g.setColour (bubbleFill);
                    g.fillRect (tailX - 2, ty,     4, 1);
                    g.fillRect (tailX - 1, ty + 1, 3, 1);
                    g.fillRect (tailX,     ty + 2, 2, 1);
                    g.setColour (PinkXP::sel);
                    g.fillRect (tailX - 3, ty,     1, 1);
                    g.fillRect (tailX + 2, ty,     1, 1);
                    g.fillRect (tailX - 2, ty + 1, 1, 1);
                    g.fillRect (tailX + 2, ty + 1, 1, 1);
                    g.fillRect (tailX - 1, ty + 2, 1, 1);
                    g.fillRect (tailX + 1, ty + 2, 1, 1);
                    g.fillRect (tailX,     ty + 3, 2, 1);
                }
            }
            else if (tokens.frameStyle == PinkXP::FrameStyle::neonTube)
            {
                // Neon Cyber：深色烟熏赛博 HUD 对话盒（双通道霓虹边 + 发光四角支架 + 霓虹对话尾）
                const auto neonBubbleFill = (bubbleFill.getPerceivedBrightness() < 0.45f)
                                              ? bubbleFill.interpolatedWith (juce::Colours::black, 0.22f)
                                              : bubbleFill;
                if (tokens.isChromaKey)
                    drawKeyedPetCard (g, bubble, neonBubbleFill, tokens);
                else
                    PinkXP::Craft::drawPanelWell (g, bubble, neonBubbleFill, PinkXP::WellKind::card);
                PinkXP::drawPixelCorners (g, bubble,
                                          PinkXP::sel.interpolatedWith (juce::Colours::white, 0.55f), 4, 1);
                if (drawTail)
                {
                    juce::Path tail;
                    tail.startNewSubPath ((float) (tailX - 3), (float) (bubble.getBottom() - 1));
                    tail.lineTo          ((float) (tailX + 3), (float) (bubble.getBottom() - 1));
                    tail.lineTo          ((float) tailX,       (float) (bubble.getBottom() + 3));
                    tail.closeSubPath();
                    g.setColour (neonBubbleFill);
                    g.fillPath (tail);
                    g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.35f).withAlpha (0.92f));
                    g.drawLine ((float) (tailX - 3), (float) (bubble.getBottom() - 1),
                                (float) tailX,       (float) (bubble.getBottom() + 3), 1.0f);
                    g.drawLine ((float) (tailX + 3), (float) (bubble.getBottom() - 1),
                                (float) tailX,       (float) (bubble.getBottom() + 3), 1.0f);
                }
            }
            else if (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome)
            {
                // Millennium Chrome：金属斜角复古未来风对话盒（微投影 + 4-stop 拉丝金属底 + 亮铬斜角沿 + 金属对话尾）
                if (! PinkXP::isGreenScreenBackdrop())
                {
                    g.setColour (juce::Colours::black.withAlpha (0.18f));
                    g.fillRect (bubble.translated (1, 2));
                }

                if (tokens.isChromaKey)
                    drawKeyedPetCard (g, bubble, bubbleFill, tokens);
                else
                    PinkXP::Craft::drawPanelWell (g, bubble, bubbleFill, PinkXP::WellKind::card);

                // 顶部 1px 亮铬高光条 + 底部 1px 金属微刻线
                if (bubble.getWidth() >= 6 && bubble.getHeight() >= 6)
                {
                    const bool isDarkBubble = bubbleFill.getPerceivedBrightness() < 0.45f;
                    g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.52f)
                                           .withAlpha (isDarkBubble ? 0.34f : 0.78f));
                    g.fillRect (bubble.getX() + 1, bubble.getY() + 1, bubble.getWidth() - 2, 1);
                    g.setColour (PinkXP::sel.withAlpha (0.75f));
                    g.fillRect (bubble.getX() + 1, bubble.getY() + 1, 3, 1);
                    g.fillRect (bubble.getRight() - 4, bubble.getY() + 1, 3, 1);
                }

                if (drawTail)
                {
                    juce::Path tail;
                    tail.startNewSubPath ((float) (tailX - 3), (float) (bubble.getBottom() - 1));
                    tail.lineTo          ((float) (tailX + 3), (float) (bubble.getBottom() - 1));
                    tail.lineTo          ((float) tailX,       (float) (bubble.getBottom() + 3));
                    tail.closeSubPath();
                    g.setColour (bubbleFill);
                    g.fillPath (tail);
                    g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.50f).withAlpha (0.65f));
                    g.drawLine ((float) (tailX - 2), (float) (bubble.getBottom() - 1),
                                (float) tailX,       (float) (bubble.getBottom() + 2), 1.0f);
                    g.setColour (PinkXP::dark.withAlpha (0.82f));
                    g.drawLine ((float) (tailX - 3), (float) (bubble.getBottom() - 1),
                                (float) tailX,       (float) (bubble.getBottom() + 3), 1.0f);
                    g.drawLine ((float) (tailX + 3), (float) (bubble.getBottom() - 1),
                                (float) tailX,       (float) (bubble.getBottom() + 3), 1.0f);
                }
            }
            else
            {
                // Modern / Frosted Glass：遵循 tokens.cardRadius 的干净圆角气泡
                const auto rf   = bubble.toFloat();
                const float rad = juce::jmin (tokens.cardRadius, rf.getHeight() * 0.45f);
                const auto borderCol = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
                                           ? PinkXP::dark.withAlpha (0.34f)
                                           : PinkXP::skinHairlineOn (bubbleFill, 0.24f);

                if (! PinkXP::isGreenScreenBackdrop())
                {
                    // 极克制的演播室微浮影（仅非绿幕，避免抠像杂边）
                    g.setColour (juce::Colours::black.withAlpha (0.09f));
                    g.fillRoundedRectangle (rf.translated (0.0f, 1.5f).expanded (0.5f), rad + 0.5f);
                }

                g.setColour (bubbleFill);
                g.fillRoundedRectangle (rf, rad);

                if (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass && rf.getHeight() >= 10.0f)
                {
                    auto sheen = rf.reduced (1.5f);
                    sheen.setHeight (sheen.getHeight() * 0.42f);
                    juce::ColourGradient dome (juce::Colours::white.withAlpha (0.34f), 0.0f, sheen.getY(),
                                               juce::Colours::white.withAlpha (0.04f), 0.0f, sheen.getBottom(), false);
                    g.setGradientFill (dome);
                    g.fillRoundedRectangle (sheen, juce::jmax (1.0f, rad - 1.5f));
                    if (bubble.getWidth() >= 10)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.72f));
                        g.fillRect (bubble.getX() + 3, bubble.getY() + 1, bubble.getWidth() - 6, 1);
                    }
                }

                g.setColour (borderCol);
                g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);

                if (drawTail)
                {
                    juce::Path tail;
                    tail.startNewSubPath ((float) (tailX - 3), rf.getBottom() - 1.5f);
                    tail.lineTo          ((float) (tailX + 3), rf.getBottom() - 1.5f);
                    tail.lineTo          ((float) tailX,       rf.getBottom() + 3.0f);
                    tail.closeSubPath();
                    g.setColour (bubbleFill);
                    g.fillPath (tail);
                    g.setColour (borderCol);
                    g.drawLine ((float) (tailX - 3), rf.getBottom() - 0.5f,
                                (float) tailX,       rf.getBottom() + 3.0f, 1.0f);
                    g.drawLine ((float) (tailX + 3), rf.getBottom() - 0.5f,
                                (float) tailX,       rf.getBottom() + 3.0f, 1.0f);
                }
            }

            textLayout.draw (g, bubble.reduced (bubblePaddingX, bubblePaddingY).toFloat());
        }

    }


    if (focused && ! hideChromaChrome)
    {
        auto focusBounds = getFocusBounds();
        switch (tokens.frameStyle)
        {
            case PinkXP::FrameStyle::pixelBevel:
                g.setColour (PinkXP::pink300.withAlpha (0.95f));
                g.drawRect (focusBounds, 1);
                PinkXP::drawPixelCorners (g, focusBounds, PinkXP::sel, 6, 2);
                break;

            case PinkXP::FrameStyle::machinedChrome:
                // CNC 金属瞄准聚焦框（暗 milled 外框 + 镜面内框 + 四角 4px 主题精密角标）
                g.setColour (PinkXP::dark.withAlpha (0.88f));
                g.drawRect (focusBounds, 1);
                g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.50f).withAlpha (0.72f));
                g.drawRect (focusBounds.reduced (1), 1);
                PinkXP::drawPixelCorners (g, focusBounds, PinkXP::sel, 5, 1);
                break;

            case PinkXP::FrameStyle::frostedGlass:
            {
                const auto ff = focusBounds.toFloat();
                const float rad = juce::jmin (tokens.cardRadius, 6.0f);
                g.setColour (PinkXP::sel.withAlpha (0.30f));
                g.drawRoundedRectangle (ff.expanded (0.5f), rad + 0.5f, 2.0f);
                g.setColour (PinkXP::sel.withAlpha (0.85f));
                g.drawRoundedRectangle (ff.reduced (0.5f), rad, 1.5f);
                g.setColour (juce::Colours::white.withAlpha (0.62f));
                g.drawRoundedRectangle (ff.reduced (1.5f), juce::jmax (1.0f, rad - 1.0f), 1.0f);
                break;
            }

            case PinkXP::FrameStyle::neonTube:
                g.setColour (PinkXP::sel.withAlpha (0.34f));
                g.drawRect (focusBounds, 2);
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.48f));
                g.drawRect (focusBounds.reduced (1), 1);
                PinkXP::drawPixelCorners (g, focusBounds,
                                          PinkXP::sel.interpolatedWith (juce::Colours::white, 0.65f), 6, 1);
                break;

            case PinkXP::FrameStyle::crtPhosphor:
                g.setColour (PinkXP::sel.withAlpha (0.88f));
                g.drawRect (focusBounds, 1);
                PinkXP::drawPixelCorners (g, focusBounds, PinkXP::sel, 5, 1);
                break;

            case PinkXP::FrameStyle::modernStudio:
            default:
                g.setColour (PinkXP::sel.withAlpha (0.85f));
                g.drawRoundedRectangle (focusBounds.toFloat().reduced (0.5f),
                                        juce::jmin (tokens.cardRadius, 5.0f), 1.25f);
                break;
        }

        auto del = getDeleteButtonBounds();
        const bool smoothDel = (! tokens.pixelSnapped && tokens.controlRadius > 0.5f);
        const bool craftDel  = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome
                                || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass
                                || tokens.frameStyle == PinkXP::FrameStyle::neonTube
                                || tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
        const auto delState = deleteBtnPressed ? PinkXP::ControlState::pressed
                            : deleteBtnHovered ? PinkXP::ControlState::hover
                                               : PinkXP::ControlState::normal;
        const auto delFill  = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor && (deleteBtnPressed || deleteBtnHovered))
                                ? PinkXP::sel
                            : ((tokens.frameStyle == PinkXP::FrameStyle::frostedGlass
                                || tokens.frameStyle == PinkXP::FrameStyle::neonTube) && deleteBtnPressed)
                                ? PinkXP::sel
                            : deleteBtnPressed ? PinkXP::pink100
                            : (deleteBtnHovered && ! smoothDel && ! craftDel) ? PinkXP::pink200
                                                                              : PinkXP::btnFace;
        PinkXP::Craft::drawControlChrome (g, del, delFill, delState);

        auto txt = del;
        if (deleteBtnPressed) txt.translate (1, 1);
        PinkXP::SymbolGlyphs::drawIconGlyph (g, "x", txt, 11.0f,
                                             PinkXP::readableInkOn (delFill), -1, -1);

        // 聚焦时在状态栏下方显示宠物名字
        if (roleName.isNotEmpty())
        {
            auto nameArea = getHudBounds().withTrimmedTop (18);
            g.setColour (PinkXP::readableInkOn (PinkXP::content));
            g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
            g.drawText (y2k::tr (roleName), nameArea.withHeight (14), juce::Justification::centred, false);
        }
    }

    // 删除确认对话框已迁移到 VirtuPetConfirmOverlay（workspace 层级渲染，
    // 不受模块边界裁剪），见 showConfirmOverlay()。
}

void VirtuPetModule::resized()
{
    const auto now = getLocalBounds();

    const bool grewByHeightOnly = (! lastLocalBounds.isEmpty())
                               && (now.getHeight() > lastLocalBounds.getHeight());

    const bool isVerticalResizeDragging = (dragMode == DragMode::resize)
                                       && (resizeEdge == Edge::bottom || resizeEdge == Edge::bottomRight);

    const auto frame = getCurrentFrame();
    if (frame.isNull())
    {
        lastLocalBounds = now;
        return;
    }

    const auto playArea = now.withTrimmedTop (juce::jmin (hudHeight, now.getHeight()));
    const float minX = 0.0f;
    const float maxX = (float) juce::jmax (0, playArea.getWidth() - frame.getWidth());
    const float floorY = (float) juce::jmax (0, now.getHeight() - frame.getHeight());
    const float halfW = (float) frame.getWidth() * 0.5f;
    const float anchorMin = juce::jmin (halfW, (float) playArea.getWidth() - halfW);
    const float anchorMax = juce::jmax (halfW, (float) playArea.getWidth() - halfW);

    if (! hasPetPosition)
    {
        petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, (anchorMin + anchorMax) * 0.5f);
        petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
        petPos.y = floorY;
        hasPetPosition = true;
    }

    else
    {
        // carried 拖拽期间不在此修改 petPos——mouseDrag 已全权接管，
        // 避免与 setTopLeftPosition 触发的异步重绘产生竞态闪烁。
        if (motionMode == MotionMode::carried)
        {
            lastLocalBounds = now;
            return;
        }

        petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, petGroundAnchorX);

        petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
        petPos.y = juce::jlimit (0.0f, floorY, petPos.y);

        if (motionMode == MotionMode::patrol)
            petPos.y = floorY;

    }

    if (motionMode == MotionMode::egg || motionMode == MotionMode::hatching)
    {
        petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, (anchorMin + anchorMax) * 0.5f);
        petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
        petPos.y = floorY;
    }

    if (isVerticalResizeDragging
        && grewByHeightOnly
        && motionMode != MotionMode::egg
        && motionMode != MotionMode::hatching
        // ---- 修复"拖动高度时动画卡住" ----
        //
        // 原问题：用户持续向下拖动 resize 时，每一次 mouseDrag 都会 setBounds →
        //        resized 被调用。若此处无下面三项保护，则每个 drag tick 都会
        //        switchMotionMode(startledIntro)。
        //        · 第一次拖动时 motionMode 是 patrol → 切到 startledIntro，合理。
        //        · startledIntro 播完会自动过渡到 falling；但若此时用户仍在拖动、
        //          高度又一次增大，resized 再次触发，旧逻辑没有排除 falling，
        //          于是 switchMotionMode 从 falling 切回 startledIntro →
        //          forceAnimation(startled, restartFrame=true) → currentFrameIdx=0。
        //        · 同理，刚切到 landingFall 又会被打回 startledIntro。
        //        · 动画帧节律是 1 秒 1 帧（animFrameIntervalSec=1.0f），若每次 drag
        //          tick（最密集 60Hz）都重置 currentFrameIdx，则角色**永远卡在
        //          startledIntro 的第 0 帧**无法前进 → 视觉上"卡住"。
        //
        // 修复：仅当 motionMode **不在**瞬时状态链（startledIntro/falling/landingFall）
        //       时才触发一次 startledIntro。这样：
        //         · 初次拖动：patrol → startledIntro（正常触发）
        //         · 后续持续拖动：已在 startledIntro/falling/landingFall → 不再打断
        //         · 播完 landingFall 后若仍在拖动且高度又变大 → patrol 结束后再次
        //           触发 startledIntro，形成"每一轮摔倒结束才再摔一次"的合理节奏
        && motionMode != MotionMode::startledIntro
        && motionMode != MotionMode::falling
        && motionMode != MotionMode::landingFall
        && hasAnimation ((int) PetAnim::startled))
    {
        idleTicksRemaining = 0;
        switchMotionMode (MotionMode::startledIntro);
    }

    lastLocalBounds = now;
}

void VirtuPetModule::mouseMove (const juce::MouseEvent& e)
{
    if (! focused)
    {
        updateCursorFor (Edge::none);
        return;
    }

    // 锁定态：不展示 resize 光标，也不展示删除按钮 hover。
    const bool locked = isPanelLayoutLocked (*this);

    const bool hoveredDel = (! locked) && getDeleteButtonBounds().contains (e.getPosition());
    if (hoveredDel != deleteBtnHovered)
    {
        deleteBtnHovered = hoveredDel;
        repaintSelfAndParent (getDeleteButtonBounds());
    }

    if (locked)
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    if (! hoveredDel)
        updateCursorFor (detectEdge (e.getPosition()));
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);

}

void VirtuPetModule::mouseExit (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    deleteBtnHovered = false;
    if (dragMode == DragMode::none)
        setMouseCursor (juce::MouseCursor::NormalCursor);
    repaintSelfAndParent (getDeleteButtonBounds());
}

void VirtuPetModule::mouseDown (const juce::MouseEvent& e)
{
    // 右键 → 冒泡给 workspace 弹出"添加模块"菜单（任何位置均可）
    if (e.mods.isPopupMenu())
    {
        if (onRightClick)
            onRightClick (*this, e.getPosition());
        return;
    }

    // ★ carried 闪烁修复：使用 toFront(false) 改变 z-order 但不触发重绘。
    // 后续 setFocusVisual(true) → repaintSelfAndParent() 统一处理视觉刷新。
    // 若用 toFront(true)，其内置重绘与 mouseDrag 中 setTopLeftPosition 的
    // 内置重绘形成竞态，对 non-opaque 组件在 OpenGL 下产生"闪现-闪回"。
    toFront (false);
    if (onBroughtToFront)
        onBroughtToFront (*this);

    setFocusVisual (true);

    // 布局锁定态：删除 / resize / move 均不启动（锁定不允许删除子组件）。
    if (isPanelLayoutLocked (*this))
        return;

    const auto pos = e.getPosition();
    if (getDeleteButtonBounds().contains (pos))
    {
        deleteBtnPressed = true;
        repaintSelfAndParent (getDeleteButtonBounds());
        return;
    }

    const auto edge = detectEdge (pos);

    if (edge != Edge::none)
    {
        dragMode = DragMode::resize;
        resizeEdge = edge;
        dragStartMouse = e.getEventRelativeTo (getParentComponent()).getPosition();
        dragStartBounds = getBounds();
        return;
    }

    dragMode = DragMode::move;
    dragStartMouse = e.getEventRelativeTo (getParentComponent()).getPosition();
    dragStartBounds = getBounds();
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);

    repaintSelfAndParent();
}

void VirtuPetModule::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == DragMode::none)
        return;

    const auto mouseInParent = e.getEventRelativeTo (getParentComponent()).getPosition();
    const auto delta = mouseInParent - dragStartMouse;

    if (dragMode == DragMode::move)
    {
        auto newTopLeft = dragStartBounds.getTopLeft() + delta;

        if (auto* parent = getParentComponent())
        {
            const int maxX = juce::jmax (0, parent->getWidth()  - getWidth());
            const int maxY = juce::jmax (0, parent->getHeight() - getHeight());
            newTopLeft.x = juce::jlimit (0, maxX, newTopLeft.x);
            newTopLeft.y = juce::jlimit (0, maxY, newTopLeft.y);
        }

        // ------- carried 状态触发与同步（在 setTopLeftPosition 之前完成）-------
        //
        // ★ 关键：petPos 必须在 setTopLeftPosition 之前计算完毕。
        // setTopLeftPosition 会触发 JUCE 内置重绘（标记脏区），
        // 若此时 petPos 还是旧值，non-opaque 组件的一次异步重绘就能读到错误
        // 位置，造成"闪现—闪回"的闪烁抖动。
        //
        // 因此这里用 newTopLeft（即将生效的位置）预计算 petPos，
        // 确保 setTopLeftPosition 触发的任何重绘都使用正确的 petPos。
        constexpr float kCarriedTriggerDistPx = 4.0f;

        if (motionMode != MotionMode::carried
            && motionMode != MotionMode::egg
            && motionMode != MotionMode::hatching
            && delta.getDistanceFromOrigin() >= kCarriedTriggerDistPx)
        {
            carriedPetWsX = (float) dragStartBounds.getX() + petPos.x;
            carriedPetWsY = (float) dragStartBounds.getY() + petPos.y;
            carriedDragSuppressRepaint = true;
            switchMotionMode (MotionMode::carried);
        }

        if (motionMode == MotionMode::carried)
        {
            const auto cf = getCurrentFrame();
            if (! cf.isNull())
            {
                // 用 newTopLeft（将生效的坐标）预计算，而非 getX()/getY()（当前坐标）
                const float desiredX = carriedPetWsX - (float) newTopLeft.x;
                const float desiredY = carriedPetWsY - (float) newTopLeft.y;

                const auto area = getLocalBounds();
                const auto playArea = area.withTrimmedTop (juce::jmin (hudHeight, area.getHeight()));
                const float cfHalfW = (float) cf.getWidth() * 0.5f;
                const float cfFloorY = (float) juce::jmax (0, area.getHeight() - cf.getHeight());

                const float pMinX = 0.0f;
                const float pMinY = (float) playArea.getY();  // HUD 下边界，宠物不可进入 HUD 区
                const float pMaxX = (float) juce::jmax (0, playArea.getWidth() - cf.getWidth());
                petPos.x = juce::jlimit (pMinX, pMaxX, desiredX);
                petPos.y = juce::jlimit (pMinY, cfFloorY, desiredY);

                // 夹持后同步锚点，避免反向拖动时粘滞在边界
                carriedPetWsX = (float) newTopLeft.x + petPos.x;
                carriedPetWsY = (float) newTopLeft.y + petPos.y;
                petGroundAnchorX = petPos.x + cfHalfW;
            }
        }

        setTopLeftPosition (newTopLeft);  // petPos 已就绪 → 重绘使用正确坐标
    }
    else
    {
        auto newBounds = dragStartBounds;

        if (resizeEdge == Edge::right || resizeEdge == Edge::bottomRight)
            newBounds.setWidth (juce::jmax (minW, dragStartBounds.getWidth() + delta.x));
        if (resizeEdge == Edge::bottom || resizeEdge == Edge::bottomRight)
            newBounds.setHeight (juce::jmax (minH, dragStartBounds.getHeight() + delta.y));

        if (auto* parent = getParentComponent())
        {
            newBounds.setWidth  (juce::jmin (newBounds.getWidth(),  parent->getWidth()  - newBounds.getX()));
            newBounds.setHeight (juce::jmin (newBounds.getHeight(), parent->getHeight() - newBounds.getY()));
        }

        setBounds (newBounds);
    }

    if (onBoundsDragging)
        onBoundsDragging (*this);
}

void VirtuPetModule::mouseUp (const juce::MouseEvent& e)
{
    if (deleteBtnPressed)
    {
        deleteBtnPressed = false;
        const bool stillOnDel = getDeleteButtonBounds().contains (e.getPosition());
        repaintSelfAndParent (getDeleteButtonBounds());

        if (stillOnDel && onCloseClicked)
        {
            showConfirmOverlay();
        }
        return;
    }

    if (dragMode != DragMode::none)
    {
        // 拖动松手：若正处于 carried 状态 → 宠物触发物理跌落
        if (motionMode == MotionMode::carried)
        {
            carriedDragSuppressRepaint = false;

            // 保存拖动最终位置的 petPos，switchMotionMode(falling)→
            // forceAnimation(startled) 会通过 petGroundAnchorX 反算漂移 petPos.x。
            // switchMotionMode 内部也会 enqueuePetDirtyRepaint，但其 dirty bounds
            // 基于被漂移后的 petPos，不能直接 flush。此处用 frozenPos 恢复后做
            // 全量 repaintSelfAndParent 确保宠物立即绘制在正确位置。
            const auto frozenPos = petPos;
            fallVelocityPxPerTick = 0.0f;
            switchMotionMode (MotionMode::falling);
            petPos = frozenPos;
            petGroundAnchorX = petPos.x + (float) getCurrentFrame().getWidth() * 0.5f;

            hasQueuedPetDirty = false;   // 丢弃 switchMotionMode 积累的 dirty bounds
            repaintSelfAndParent();      // 全量重绘，确保 petPos 正确
        }

        dragMode = DragMode::none;
        resizeEdge = Edge::none;
        setMouseCursor (juce::MouseCursor::NormalCursor);

        if (onBoundsChangedByUser)
            onBoundsChangedByUser (*this);
    }
}

void VirtuPetModule::timerCallback()
{
    const int targetHz = getTargetVisualHzForMode (evaluateAutoMotionMode());

    if (targetHz != currentVisualHz)
    {
        currentVisualHz = targetHz;
        currentTickDtSec = 1.0f / (float) juce::jmax (1, currentVisualHz);
        startTimerHz (currentVisualHz);

        if (currentVisualHz >= ticksPerSecond)
            flushVisualRepaintQueue (true);
    }

    updateNeeds();

    switchMotionMode (evaluateAutoMotionMode());

    stepWander();

    frameAccumSec += currentTickDtSec;
    constexpr float animFrameIntervalSec = 1.0f;
    if (frameAccumSec >= animFrameIntervalSec)
    {
        frameAccumSec = std::fmod (frameAccumSec, animFrameIntervalSec);
        stepOneFrame();
    }

    flushVisualRepaintQueue (false);
}

bool VirtuPetModule::loadRandomRoleAnimations()
{
    for (auto& arr : animFrames)
        arr.clearQuick();
    for (auto& arr : animFramesRight)
        arr.clearQuick();
    availableAnimIds.clearQuick();

    currentAnimId = 1;
    currentFrameIdx = 0;
    hasPetPosition = false;
    showSpeechBubble = false;
    currentSpeechText.clear();
    jumpFightActive = false;
    jumpFightTick = 0;

    const bool hasEggFrames = loadRandomEggFrames (eggFrames, eggStyleId);

    const auto root = findVirtuPetAssetsRoot();
    const auto mirrorRoot = findVirtuPetMirrorAssetsRoot();

    if (root.isDirectory())

    {
        juce::Array<juce::File> roleDirs;
        root.findChildFiles (roleDirs, juce::File::findDirectories, false);
        if (! roleDirs.isEmpty())
        {
            auto chosenRoleDir = roleDirs.getReference (
                juce::Random::getSystemRandom().nextInt (roleDirs.size()));

            if (loadRoleFramesFromDirectory (chosenRoleDir, mirrorRoot, animFrames, animFramesRight, availableAnimIds))

            {
                roleName = chosenRoleDir.getFileName();
                motionMode = hasEggFrames ? MotionMode::egg : MotionMode::patrol;
                if (! hasEggFrames)
                    beginPatrolCycle();
                hasPetPosition = false;
                repaintSelfAndParent();
                return true;
            }
        }
    }

    // 兜底：如果切帧目录不可用，退化为 role 目录随机单图（至少有可见内容）
    const auto rolePngDir = findVirtuPetRolePngDir();
    if (! rolePngDir.isDirectory())
        return false;

    juce::Array<juce::File> rolePngs;
    rolePngDir.findChildFiles (rolePngs, juce::File::findFiles, false, "*.png");
    if (rolePngs.isEmpty())
        return false;

    auto selected = rolePngs.getReference (juce::Random::getSystemRandom().nextInt (rolePngs.size()));
    auto single = juce::ImageFileFormat::loadFrom (selected);
    if (single.isNull())
        return false;

    roleName = selected.getFileNameWithoutExtension();
    currentAnimId = 1;
    currentFrameIdx = 0;
    animFrames[0].clearQuick();
    animFrames[0].add (single);
    for (auto& arr : animFramesRight)
        arr.clearQuick();

    availableAnimIds.clearQuick();
    availableAnimIds.add (1);
    motionMode = hasEggFrames ? MotionMode::egg : MotionMode::patrol;
    if (! hasEggFrames)
        beginPatrolCycle();
    hasPetPosition = false;

    repaintSelfAndParent();
    return true;

}

void VirtuPetModule::chooseNextAnimation()
{
    if (availableAnimIds.isEmpty())
        return;

    const auto oldFrame = getCurrentFrame();
    if (! oldFrame.isNull())
        petGroundAnchorX = petPos.x + (float) oldFrame.getWidth() * 0.5f;

    const int nextAnim = chooseNextAnimByState();
    currentAnimId = juce::jlimit (1, 33, nextAnim);
    currentFrameIdx = 0;

    const auto newFrame = getCurrentFrame();
    if (! newFrame.isNull())
        petPos.x = petGroundAnchorX - (float) newFrame.getWidth() * 0.5f;
}

void VirtuPetModule::stepOneFrame()
{
    const auto oldMode = motionMode;
    const int oldAnimId = currentAnimId;
    const int oldFrameIdx = currentFrameIdx;
    const bool oldBubbleVisible = showSpeechBubble;
    const auto oldBubbleText = currentSpeechText;
    const auto oldFrame = getCurrentFrame();
    const auto oldPetBounds = getPetVisualBoundsFor (oldFrame, petPos, oldBubbleVisible, oldBubbleText, oldMode);

    if (motionMode == MotionMode::egg)
    {
        currentFrameIdx = 0;

        if (oldFrameIdx != currentFrameIdx)
            enqueuePetDirtyRepaint (oldPetBounds.getUnion (getCurrentPetVisualBounds()));
        return;
    }

    if (motionMode == MotionMode::hatching)
    {
        if (eggFrames.size() < 4)
        {
            onAnimationFinished();
        }
        else
        {
            ++currentFrameIdx;
            if (currentFrameIdx >= 4)
                onAnimationFinished();
        }

        const auto newPetBounds = getCurrentPetVisualBounds();
        const bool changed = oldMode != motionMode
                          || oldAnimId != currentAnimId
                          || oldFrameIdx != currentFrameIdx
                          || oldBubbleVisible != showSpeechBubble
                          || oldBubbleText != currentSpeechText
                          || oldPetBounds != newPetBounds;
        if (changed)
            enqueuePetDirtyRepaint (oldPetBounds.getUnion (newPetBounds));
        return;
    }

    // carried 期间冻结动画帧推进：petPos 由 mouseDrag 独占管理，
    // 不走 forceAnimation 锚点反算，避免浮点漂移导致携带闪烁与松手跳位。
    if (motionMode == MotionMode::carried)
        return;

    if (availableAnimIds.isEmpty())
        return;

    const auto* frames = &getFramesForAnim (currentAnimId);
    const auto* rightFrames = &getRightFramesForAnim (currentAnimId);
    if (shouldUseRightVariantForAnim (currentAnimId) && ! rightFrames->isEmpty())
        frames = rightFrames;

    if (frames->isEmpty())
    {
        chooseNextAnimation();
        frames = &getFramesForAnim (currentAnimId);
        rightFrames = &getRightFramesForAnim (currentAnimId);
        if (shouldUseRightVariantForAnim (currentAnimId) && ! rightFrames->isEmpty())
            frames = rightFrames;
        if (frames->isEmpty())
            return;
    }

    ++currentFrameIdx;
    if (currentFrameIdx >= frames->size())
        onAnimationFinished();

    const auto newPetBounds = getCurrentPetVisualBounds();
    const bool changed = oldMode != motionMode
                      || oldAnimId != currentAnimId
                      || oldFrameIdx != currentFrameIdx
                      || oldBubbleVisible != showSpeechBubble
                      || oldBubbleText != currentSpeechText
                      || oldPetBounds != newPetBounds;
    if (changed)
        enqueuePetDirtyRepaint (oldPetBounds.getUnion (newPetBounds));
}

int VirtuPetModule::randomAnimFrom (std::initializer_list<int> ids) const
{
    juce::Array<int> candidates;
    for (int id : ids)
    {
        const int safeId = juce::jlimit (1, 33, id);
        if (availableAnimIds.contains (safeId))
            candidates.addIfNotAlreadyThere (safeId);
    }

    if (candidates.isEmpty())
    {
        if (availableAnimIds.isEmpty())
            return 1;  // 防御：没有任何可用动画资源时返回 1
        return availableAnimIds.getReference (juce::Random::getSystemRandom().nextInt (availableAnimIds.size()));
    }

    return candidates.getReference (juce::Random::getSystemRandom().nextInt (candidates.size()));
}

bool VirtuPetModule::hasAnimation (int animId) const
{
    const int safeId = juce::jlimit (1, 33, animId);
    return availableAnimIds.contains (safeId) && ! animFrames[(size_t) (safeId - 1)].isEmpty();
}

void VirtuPetModule::forceAnimation (int animId, bool restartFrame)
{
    if (! hasAnimation (animId))
        return;

    const auto oldFrame = getCurrentFrame();
    if (! oldFrame.isNull())
        petGroundAnchorX = petPos.x + (float) oldFrame.getWidth() * 0.5f;

    currentAnimId = juce::jlimit (1, 33, animId);
    if (restartFrame)
        currentFrameIdx = 0;

    const auto newFrame = getCurrentFrame();
    if (! newFrame.isNull())
        petPos.x = petGroundAnchorX - (float) newFrame.getWidth() * 0.5f;
}

void VirtuPetModule::playToyReaction (int animId)
{
    // 仅触发动画，不改饥饿/健康/运动状态（决策 6）。
    forceAnimation (animId, true);
}

bool VirtuPetModule::shouldUseRightVariantForAnim (int animId) const
{
    const int safeId = juce::jlimit (1, 33, animId);

    if (motionMode == MotionMode::patrol)
    {
        if (! patrolMirrorX)
            return false;

        return safeId == (int) PetAnim::lookLeft || safeId == (int) PetAnim::moveLeft;
    }

    if (motionMode == MotionMode::eating)
    {
        if (! eatingMirrorX)
            return false;

        return safeId == (int) PetAnim::runLeftToFood
            || safeId == (int) PetAnim::runLeftToHate
            || safeId == (int) PetAnim::runLeftToLike
            || safeId == (int) PetAnim::eatLeft;
    }

    return false;
}

const juce::Array<juce::Image>& VirtuPetModule::getFramesForAnim (int animId) const
{
    const int safeId = juce::jlimit (1, 33, animId);
    return animFrames[(size_t) (safeId - 1)];
}

const juce::Array<juce::Image>& VirtuPetModule::getRightFramesForAnim (int animId) const
{
    const int safeId = juce::jlimit (1, 33, animId);
    return animFramesRight[(size_t) (safeId - 1)];
}

bool VirtuPetModule::hasRightVariantFrames (int animId) const
{
    const auto& rightFrames = getRightFramesForAnim (animId);
    return ! rightFrames.isEmpty();
}

int VirtuPetModule::chooseNextAnimByState() const

{
    switch (motionMode)
    {
        case MotionMode::egg:
        case MotionMode::hatching:
            return 1;

        case MotionMode::toilet:
            return hasAnimation ((int) PetAnim::toiletSquat)
                ? (int) PetAnim::toiletSquat
                : randomAnimFrom ({ (int) PetAnim::wantToToilet, (int) PetAnim::depressed });

        case MotionMode::patrol:
        {
            if (patrolLockedAnimId > 0 && hasAnimation (patrolLockedAnimId))
                return patrolLockedAnimId;

            switch (currentPatrolAction)
            {
                case PatrolAction::lookLeftSlow:
                case PatrolAction::lookRightSlow:
                    return hasAnimation ((int) PetAnim::lookLeft)
                        ? (int) PetAnim::lookLeft
                        : randomAnimFrom ({ (int) PetAnim::lookAround });

                case PatrolAction::moveLeftFast:
                case PatrolAction::moveRightFast:
                    return hasAnimation ((int) PetAnim::moveLeft)
                        ? (int) PetAnim::moveLeft
                        : randomAnimFrom ({ (int) PetAnim::dazeThenWalkLeft });

                case PatrolAction::talk:
                    return randomAnimFrom ({ (int) PetAnim::talkHappier,
                                             (int) PetAnim::talkHappiest,
                                             (int) PetAnim::talkShy });

                case PatrolAction::jumpFight:
                    return hasAnimation ((int) PetAnim::fight)
                        ? (int) PetAnim::fight
                        : randomAnimFrom ({ (int) PetAnim::moveLeft });
            }
            break;
        }

        case MotionMode::startledIntro:
            return hasAnimation ((int) PetAnim::startled)
                ? (int) PetAnim::startled
                : randomAnimFrom ({ (int) PetAnim::shocked, (int) PetAnim::fall });

        case MotionMode::falling:
            return hasAnimation ((int) PetAnim::startled)
                ? (int) PetAnim::startled
                : randomAnimFrom ({ (int) PetAnim::shocked });

        case MotionMode::landingFall:
            return hasAnimation ((int) PetAnim::fall)
                ? (int) PetAnim::fall
                : randomAnimFrom ({ (int) PetAnim::shocked, (int) PetAnim::lookAround });

        case MotionMode::drowsy:
            return hasAnimation ((int) PetAnim::wantToEat)
                ? (int) PetAnim::wantToEat
                : randomAnimFrom ({ (int) PetAnim::depressed, (int) PetAnim::lookAround });

        case MotionMode::sleeping:
            return hasAnimation ((int) PetAnim::sleep)
                ? (int) PetAnim::sleep
                : randomAnimFrom ({ (int) PetAnim::dazeThenWalkLeft, (int) PetAnim::lookAround });

        case MotionMode::eating:
            return randomAnimFrom ({ (int) PetAnim::runLeftToFood,
                                     (int) PetAnim::runLeftToHate,
                                     (int) PetAnim::runLeftToLike,
                                     (int) PetAnim::eatLeft });

        case MotionMode::hungry:
            return hasAnimation ((int) PetAnim::depressed)
                ? (int) PetAnim::depressed
                : randomAnimFrom ({ (int) PetAnim::wantToEat, (int) PetAnim::cry });

        case MotionMode::starving:
            return hasAnimation ((int) PetAnim::talkVeryNegative)
                ? (int) PetAnim::talkVeryNegative
                : randomAnimFrom ({ (int) PetAnim::depressed, (int) PetAnim::cry });

        case MotionMode::sick:
            return hasAnimation ((int) PetAnim::almostSick)
                ? (int) PetAnim::almostSick
                : randomAnimFrom ({ (int) PetAnim::sick, (int) PetAnim::nearDeath });

        case MotionMode::criticalSick:
            return hasAnimation ((int) PetAnim::sick)
                ? (int) PetAnim::sick
                : randomAnimFrom ({ (int) PetAnim::almostSick, (int) PetAnim::nearDeath });

        case MotionMode::dead:
            return hasAnimation ((int) PetAnim::death)
                ? (int) PetAnim::death
                : randomAnimFrom ({ (int) PetAnim::nearDeath, (int) PetAnim::sick });
    }

    return randomAnimFrom ({ (int) PetAnim::lookAround });
}

void VirtuPetModule::onAnimationFinished()
{
    currentFrameIdx = 0;

    switch (motionMode)
    {
        case MotionMode::egg:
            currentFrameIdx = 0;
            break;

        case MotionMode::hatching:
                switchMotionMode (MotionMode::patrol);
            break;

        case MotionMode::toilet:
            forceAnimation ((int) PetAnim::toiletSquat, true);
            break;

        case MotionMode::patrol:
            chooseNextAnimation();
            break;

        case MotionMode::startledIntro:
                switchMotionMode (MotionMode::falling);
            break;

        case MotionMode::falling:
            forceAnimation ((int) PetAnim::startled);
            break;

        case MotionMode::carried:
            // 拖动期间持续循环 startled 动画
            forceAnimation ((int) PetAnim::startled, true);
            break;

        case MotionMode::landingFall:
                switchMotionMode (MotionMode::patrol);
            break;

        case MotionMode::drowsy:
            forceAnimation ((int) PetAnim::wantToEat, true);
            break;

        case MotionMode::sleeping:
            forceAnimation ((int) PetAnim::sleep, true);
            break;

        case MotionMode::eating:
            forceAnimation (eatingCurrentAnimId, true);
            break;

        case MotionMode::hungry:
        case MotionMode::starving:
        case MotionMode::sick:
        case MotionMode::criticalSick:
        case MotionMode::dead:
            chooseNextAnimation();
            break;

    }
}

void VirtuPetModule::setSignalLevel01 (float level01) noexcept
{
    signalLevel01 = juce::jlimit (0.0f, 1.0f, level01);
}

bool VirtuPetModule::isInEggPhase() const noexcept
{
    return motionMode == MotionMode::egg || motionMode == MotionMode::hatching;
}

void VirtuPetModule::updateNeeds()
{
    // 使用当前 tick 时长，保证动态刷新率下需求变化仍按真实时间推进
    const float dt = currentTickDtSec;

    const float growthBaseRate = 1.2f;
    const float growthGainK = 12.0f;
    const float quietDecayRate = 0.8f;
    const float prevHunger = hunger;
    const float prevHealth = health;

    const bool hasSignal = signalLevel01 > 0.02f;

    if (hasSignal)
        silentTicks = 0;
    else
        silentTicks = juce::jmin (sleepSilentTicks + ticksPerSecond * 60, silentTicks + 1);

    if (motionMode == MotionMode::eating)
    {
        if (hasSignal)
            eatingLowSignalTicksRemaining = eatingLowSignalHoldTicks;
        else
            eatingLowSignalTicksRemaining = juce::jmax (0, eatingLowSignalTicksRemaining - 1);
    }
    else
    {
        eatingLowSignalTicksRemaining = 0;
    }

    float hungerDelta = 0.0f;
    if (hasSignal)
    {
        hungerDelta = growthBaseRate + growthGainK * signalLevel01;
    }
    else if (silentTicks > hungerDecaySilentDelayTicks)
    {
        hungerDelta = -quietDecayRate;
    }

    hungerDeltaPerSecond = hungerDelta;
    hunger += hungerDelta * dt;

    float healthDelta = 0.0f;
    if (hunger < 40.0f)
        healthDelta -= 0.35f * dt;
    if (hunger > 70.0f)
        healthDelta += 0.5f * dt;
    if (hunger > 90.0f)
        healthDelta += 1.2f * dt;

    health += healthDelta;

    hunger = juce::jlimit (0.0f, 100.0f, hunger);
    health = juce::jlimit (0.0f, 100.0f, health);

    const bool hungerIncreasingThisTick = hunger > prevHunger + 0.0001f;

    if (! hungerRiseFrom50Tracking)
    {
        if (prevHunger <= 50.0f
            && hunger >= 50.0f
            && hunger < 100.0f
            && hungerIncreasingThisTick)
        {
            hungerRiseFrom50Tracking = true;
        }
    }
    else
    {
        if (! hungerIncreasingThisTick || hunger < 50.0f)
        {
            hungerRiseFrom50Tracking = false;
        }
        else if (hunger >= 100.0f)
        {
            hungerRiseFrom50Tracking = false;
            toiletTriggerPending = true;
        }
    }

    if (std::abs (hunger - prevHunger) >= 0.1f || std::abs (health - prevHealth) >= 0.1f)
        repaintSelfAndParent (getHudBounds());
}

void VirtuPetModule::drawPixelBar (juce::Graphics& g,
                                     juce::Rectangle<int> area,
                                     float value01,
                                     juce::Colour fill,
                                     juce::StringRef label) const
{
    if (area.getWidth() < 8 || area.getHeight() < 6)
        return;

    value01 = juce::jlimit (0.0f, 1.0f, value01);

    const auto& tokens = PinkXP::getSkinTokens();
    const bool smoothBar = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio
                            || tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isDarkFace = PinkXP::face.getPerceivedBrightness() < 0.45f;

    // 1. 左侧独立高对比标签槽（与 LED 电平槽彻底分离，杜绝亮橙/亮绿灯块与文字重叠发糊）
    const juce::String shown = y2k::tr (label);
    const auto font = PinkXP::fitFontToHeight (PinkXP::getFont (8.0f, juce::Font::bold),
                                               (float) juce::jmax (1, area.getHeight() - 2));
    const int measuredTextW = (int) std::ceil (PinkXP::SymbolGlyphs::getTextWidth (shown, font));
    const int maxLabelW     = juce::jmax (0, area.getWidth() - 18); // 至少保留 18px 给电平条
    const int labelW        = juce::jmin (maxLabelW, measuredTextW + 6);

    if (shown.isNotEmpty() && labelW >= 12)
    {
        const auto labelArea = area.removeFromLeft (labelW);
        area.removeFromLeft (2);

        const auto badgeFill = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio)
                                   ? (isDarkFace ? PinkXP::face.interpolatedWith (juce::Colours::white, 0.08f)
                                                 : PinkXP::content)
                                   : PinkXP::btnFace;
        if (tokens.isChromaKey)
            drawKeyedPetBadge (g, labelArea, badgeFill, tokens);
        else
            PinkXP::Craft::drawBadgePill (g, labelArea, badgeFill);

        g.setColour (PinkXP::readableInkOn (badgeFill));
        PinkXP::SymbolGlyphs::drawText (g, shown, labelArea.reduced (1, 0),
                                        juce::Justification::centred, font);
    }

    if (area.getWidth() < 6)
        return;

    // 2. 主题感知凹槽底色（替代写死的 0xff1a1a1a，在任意主题下协调且保持高对比度）
    const auto troughFill = smoothBar
        ? (isDarkFace ? PinkXP::face.darker (0.36f)
                      : PinkXP::face.darker (0.11f))
        : PinkXP::dark.interpolatedWith (PinkXP::content, 0.12f).darker (0.28f);

    if (tokens.frameStyle == PinkXP::FrameStyle::modernStudio)
    {
        // Modern Minimal：精致圆角微胶囊凹槽 + 细腻 1px 描边
        const auto rf = area.toFloat();
        const float rad = juce::jmin (tokens.barCellRadius + 2.0f, rf.getHeight() * 0.5f);
        g.setColour (troughFill);
        g.fillRoundedRectangle (rf, rad);
        g.setColour (PinkXP::skinHairlineOn (troughFill, isDarkFace ? 0.20f : 0.22f));
        g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
    }
    else if (tokens.isChromaKey)
    {
        drawKeyedPetTrough (g, area, troughFill, tokens, isDarkFace);
    }
    else
    {
        PinkXP::Craft::drawPanelWell (g, area, troughFill, PinkXP::WellKind::trough);
    }

    const int inset = (smoothBar || area.getHeight() < 10) ? 1 : 2;
    auto inner = area.reduced (inset);
    if (! inner.isEmpty())
    {
        if (smoothBar)
        {
            // Modern / Frosted Glass / Green Screen 悬停：精致圆角微进度条
            const auto innerF = inner.toFloat();
            const float fillW = innerF.getWidth() * value01;
            if (fillW > 0.5f)
                PinkXP::Craft::drawMeterCell (g, innerF.withWidth (fillW), fill, true);
        }
        else
        {
            // Pixel XP / Chrome / Neon / CRT：分段硬件像素 LED 格（经由 Craft::drawMeterCell）
            const int cellGap  = juce::jmax (1, tokens.defaultCellGapPx);
            const int cellW    = 3;
            const int step     = cellW + cellGap;
            const int segCount = juce::jmax (1, inner.getWidth() / step);
            const int lit      = juce::roundToInt ((float) segCount * value01);

            for (int i = 0; i < segCount; ++i)
            {
                juce::Rectangle<int> seg (inner.getX() + i * step, inner.getY(), cellW, inner.getHeight());
                PinkXP::Craft::drawMeterCell (g, seg, fill, i < lit);
            }
        }
    }
}

juce::Rectangle<int> VirtuPetModule::getHudBounds() const
{
    auto b = getLocalBounds();
    return b.removeFromTop (juce::jmin (hudHeight, b.getHeight()));
}

VirtuPetModule::MotionMode VirtuPetModule::evaluateAutoMotionMode() const
{
    // egg/hatching 启动流程：蛋阶段等待音频，孵化阶段必须完整播放
    if (motionMode == MotionMode::egg)
        return signalLevel01 > 0.0f ? MotionMode::hatching : MotionMode::egg;

    if (motionMode == MotionMode::hatching)
        return MotionMode::hatching;

    // 最高优先级：toilet（触发后持续 6 秒）
    if (motionMode == MotionMode::toilet && toiletTicksRemaining > 0)
        return MotionMode::toilet;

    if (toiletTriggerPending)
        return MotionMode::toilet;

    // 瞬时状态链优先：由边界拖拽触发后，必须完整经历 惊吓 -> 下落 -> 摔倒
    // carried 也属于不可打断的瞬时状态（拖动中）
    if (motionMode == MotionMode::startledIntro
        || motionMode == MotionMode::falling
        || motionMode == MotionMode::landingFall
        || motionMode == MotionMode::carried)
        return motionMode;

    // 需求状态优先级：死亡 > 吃饭(含低信号保持) > 重病 > 生病 > 睡觉 > 困倦 > 极度饥饿 > 饥饿 > 巡逻
    if (health <= 0.0f)
        return MotionMode::dead;

    if (motionMode == MotionMode::eating
        && signalLevel01 <= 0.02f
        && eatingLowSignalTicksRemaining > 0)
        return MotionMode::eating;

    if (hunger < 100.0f && hungerDeltaPerSecond > 0.0f)
        return MotionMode::eating;

    if (health < 25.0f)
        return MotionMode::criticalSick;

    if (health < 50.0f)
        return MotionMode::sick;

    if (silentTicks >= sleepSilentTicks)
        return MotionMode::sleeping;

    if (silentTicks >= drowsySilentTicks)
        return MotionMode::drowsy;

    if (hunger <= 0.0f)
        return MotionMode::starving;

    if (hunger < 40.0f)
        return MotionMode::hungry;

    return MotionMode::patrol;
}

void VirtuPetModule::switchMotionMode (MotionMode newMode)

{
    if (motionMode == newMode)
        return;

    const auto oldMode = motionMode;
    const bool oldBubbleVisible = showSpeechBubble;
    const auto oldBubbleText = currentSpeechText;
    const auto oldFrame = getCurrentFrame();
    const auto oldPetBounds = getPetVisualBoundsFor (oldFrame, petPos, oldBubbleVisible, oldBubbleText, oldMode);

    motionMode = newMode;

    showSpeechBubble = false;
    currentSpeechText.clear();
    jumpFightActive = false;
    jumpFightTick = 0;
    patrolMirrorX = false;
    eatingMirrorX = false;
    eatingLowSignalTicksRemaining = 0;

    switch (motionMode)
    {
        case MotionMode::egg:
            currentFrameIdx = 0;
            break;

        case MotionMode::hatching:
            currentFrameIdx = 0;
            break;

        case MotionMode::toilet:
            toiletTriggerPending = false;
            hungerRiseFrom50Tracking = false;
            toiletTicksRemaining = toiletDurationTicks;
            forceAnimation ((int) PetAnim::toiletSquat);
            break;

        case MotionMode::patrol:
            beginPatrolCycle();
            break;

        case MotionMode::startledIntro:
            forceAnimation ((int) PetAnim::startled);
            break;

        case MotionMode::falling:
            fallVelocityPxPerTick = 0.0f;
            forceAnimation ((int) PetAnim::startled);
            break;

        case MotionMode::landingFall:
            forceAnimation ((int) PetAnim::fall);
            break;

        case MotionMode::carried:
            // 拖动拖动——播放和下跌同样的 startled 动画
            // carriedPetWsX/Y 已在 mouseDown 中记录，此处不动
            forceAnimation ((int) PetAnim::startled);
            break;

        case MotionMode::drowsy:
            forceAnimation ((int) PetAnim::wantToEat);
            break;

        case MotionMode::sleeping:
            forceAnimation ((int) PetAnim::sleep);
            break;

        case MotionMode::eating:
        {
            eatingCurrentAnimId = randomAnimFrom ({ (int) PetAnim::runLeftToFood,
                                                    (int) PetAnim::runLeftToHate,
                                                    (int) PetAnim::runLeftToLike,
                                                    (int) PetAnim::eatLeft });
            eatingAnimTicksRemaining = eatingSwitchTicks;
            eatingMoveDir = juce::Random::getSystemRandom().nextBool() ? 1.0f : -1.0f;
            eatingMirrorX = (eatingMoveDir > 0.0f);
            eatingLowSignalTicksRemaining = eatingLowSignalHoldTicks;
            eatingMoveRetargetTicksRemaining = juce::Random::getSystemRandom().nextInt (ticksPerSecond * 3 / 2) + ticksPerSecond / 2;
            forceAnimation (eatingCurrentAnimId);
            break;
        }

        case MotionMode::hungry:
            forceAnimation ((int) PetAnim::depressed);
            break;

        case MotionMode::starving:
            forceAnimation ((int) PetAnim::talkVeryNegative);
            break;

        case MotionMode::sick:
            forceAnimation ((int) PetAnim::almostSick);
            break;

        case MotionMode::criticalSick:
            forceAnimation ((int) PetAnim::sick);
            break;

        case MotionMode::dead:
            forceAnimation ((int) PetAnim::death);
            break;
    }

    enqueuePetDirtyRepaint (oldPetBounds.getUnion (getCurrentPetVisualBounds()));
}

juce::Image VirtuPetModule::getCurrentFrame() const

{
    if (motionMode == MotionMode::egg || motionMode == MotionMode::hatching)
    {
        if (eggFrames.isEmpty())
            return {};

        const int maxIdx = juce::jmax (0, eggFrames.size() - 1);
        const int frameIdx = (motionMode == MotionMode::egg)
            ? 0
            : juce::jlimit (0, maxIdx, currentFrameIdx);
        return eggFrames.getReference (frameIdx);
    }

    const auto& baseFrames = getFramesForAnim (currentAnimId);
    const auto& rightFrames = getRightFramesForAnim (currentAnimId);
    const bool useRight = shouldUseRightVariantForAnim (currentAnimId) && ! rightFrames.isEmpty();
    const auto& frames = useRight ? rightFrames : baseFrames;

    if (frames.isEmpty())
        return {};

    const int frameIdx = juce::jlimit (0, frames.size() - 1, currentFrameIdx);
    return frames.getReference (frameIdx);
}

juce::Rectangle<int> VirtuPetModule::getPetVisualBoundsFor (const juce::Image& frame,
                                                              juce::Point<float> pos,
                                                              bool bubbleVisible,
                                                              const juce::String& bubbleText,
                                                              MotionMode mode) const
{
    if (frame.isNull())
        return {};

    auto bounds = juce::Rectangle<int> ((int) std::floor (pos.x),
                                        (int) std::floor (pos.y),
                                        frame.getWidth(),
                                        frame.getHeight());

    if (mode == MotionMode::patrol && bubbleVisible && bubbleText.isNotEmpty())
    {
        const auto bubbleFont = PinkXP::getFont (8.5f, juce::Font::plain);
        const int bubblePaddingX = 6;
        const int bubblePaddingY = 3;
        const int bubbleMinW = 36;
        const int bubbleMaxW = juce::jmax (bubbleMinW, getWidth() - 6);
        const int maxTextW = juce::jmax (16, bubbleMaxW - bubblePaddingX * 2);
        const int singleLineW = juce::jmax (16, bubbleFont.getStringWidth (y2k::tr (bubbleText)));
        const int textW = juce::jlimit (16, maxTextW, singleLineW);

        juce::AttributedString bubbleTextLayout;
        bubbleTextLayout.setJustification (juce::Justification::centred);
        bubbleTextLayout.setWordWrap (juce::AttributedString::WordWrap::byWord);
        bubbleTextLayout.append (y2k::tr (bubbleText), bubbleFont, PinkXP::ink);

        juce::TextLayout textLayout;
        textLayout.createLayout (bubbleTextLayout, (float) textW);

        const int textH = juce::jmax ((int) std::ceil (bubbleFont.getHeight()), (int) std::ceil (textLayout.getHeight()));
        const int bubbleW = juce::jlimit (bubbleMinW, bubbleMaxW, textW + bubblePaddingX * 2);
        const int bubbleH = juce::jmax (14, textH + bubblePaddingY * 2);

        auto bubble = juce::Rectangle<int> ((int) std::floor (pos.x) + frame.getWidth() / 2 - bubbleW / 2,
                                            (int) std::floor (pos.y) - bubbleH - 4,
                                            bubbleW,
                                            bubbleH);
        bubble = bubble.withX (juce::jlimit (0, juce::jmax (0, getWidth() - bubble.getWidth()), bubble.getX()));
        bubble = bubble.withY (juce::jlimit (0, juce::jmax (0, getHeight() - bubble.getHeight()), bubble.getY()));

        // 包含硬阴影（+2px）与下方阶梯像素对话尾（+4px），确保 non-opaque 脏区清干净
        bounds = bounds.getUnion (bubble.withBottom (bubble.getBottom() + 4).expanded (2));
    }

    return bounds.getIntersection (getLocalBounds());
}

juce::Rectangle<int> VirtuPetModule::getCurrentPetVisualBounds() const
{
    return getPetVisualBoundsFor (getCurrentFrame(), petPos, showSpeechBubble, currentSpeechText, motionMode);
}

int VirtuPetModule::getTargetVisualHzForMode (MotionMode mode) const noexcept
{
    switch (mode)
    {
        case MotionMode::patrol:
        case MotionMode::startledIntro:
        case MotionMode::falling:
        case MotionMode::landingFall:
        case MotionMode::hatching:
        case MotionMode::eating:
        case MotionMode::carried:
            return 20;

        case MotionMode::drowsy:
        case MotionMode::hungry:
        case MotionMode::starving:
        case MotionMode::sick:
        case MotionMode::criticalSick:
        case MotionMode::toilet:
            return 10;

        case MotionMode::egg:
        case MotionMode::sleeping:
        case MotionMode::dead:
            return 5;
    }

    return 10;
}

void VirtuPetModule::enqueuePetDirtyRepaint (juce::Rectangle<int> area)
{
    // 反馈修复（外网 #2）：脏区向外扩 1px —— 动画帧切换/镜像路径的最近邻采样
    // 可能在计算边界的相邻像素上留下旧帧残影，扩张后 repaintSelfAndParent
    // 会把旧帧边缘也彻底清底，杜绝细黑线残留。
    area = area.expanded (1);
    area = area.getIntersection (getLocalBounds());
    if (area.isEmpty())
        return;

    if (hasQueuedPetDirty)
        queuedPetDirtyBounds = queuedPetDirtyBounds.getUnion (area);
    else
        queuedPetDirtyBounds = area;

    hasQueuedPetDirty = true;
}

// ---- non-opaque 正确重绘：先让父组件 repaint 对应区域，再 repaint 本组件 ----
// 组件是 non-opaque（背景需要透出 workspace 半透明 content + 桌面），
// 必须让父组件先把底色画一遍再把本组件叠上去，否则旧像素残留形成拖影或
// focus/delete 的残留。JUCE 对嵌套 non-opaque 链路的 repaint 不会自动追到根，
// 所以在这里显式转换坐标并调父组件 repaint()。
void VirtuPetModule::repaintSelfAndParent (juce::Rectangle<int> localRect)
{
    localRect = localRect.getIntersection (getLocalBounds());
    if (localRect.isEmpty())
        return;

    if (auto* parent = getParentComponent())
    {
        const auto parentRect = localRect + getBounds().getTopLeft();
        parent->repaint (parentRect);
    }

    repaint (localRect);
}

void VirtuPetModule::repaintSelfAndParent()
{
    repaintSelfAndParent (getLocalBounds());
}

void VirtuPetModule::flushVisualRepaintQueue (bool forceNow)
{
    if (! hasQueuedPetDirty)
        return;

    // carried 拖拽期间抑制 timer 驱动的异步重绘，避免与 setTopLeftPosition
    // 触发的内置重绘产生竞态导致 non-opaque 组件闪烁。
    if (carriedDragSuppressRepaint && ! forceNow)
    {
        hasQueuedPetDirty = false;
        queuedPetDirtyBounds = {};
        return;
    }

    const auto dirty = queuedPetDirtyBounds.getIntersection (getLocalBounds());
    hasQueuedPetDirty = false;
    queuedPetDirtyBounds = {};

    if (! dirty.isEmpty())
        repaintSelfAndParent (dirty);
}

void VirtuPetModule::beginPatrolCycle()

{
    if (availableAnimIds.isEmpty())
        return;  // 防御：没有任何可用动画资源时放弃巡逻

    currentPatrolAction = PatrolAction::lookLeftSlow;
    patrolActionTicksRemaining = 0;
    patrolCooldownTicksRemaining = patrolCycleTicks;
    patrolLockedAnimId = 0;
    patrolFaceLeft = true;
    patrolMirrorX = false;
    patrolActionMoveDir = 0.0f;
    patrolActionSpeedPxPerTick = 0.0f;
    showSpeechBubble = false;
    currentSpeechText.clear();
    jumpFightActive = false;
    jumpFightTick = 0;

    forceAnimation (hasAnimation ((int) PetAnim::dazeThenWalkLeft)
                        ? (int) PetAnim::dazeThenWalkLeft
                        : randomAnimFrom ({ (int) PetAnim::lookLeft, (int) PetAnim::moveLeft }));
}

void VirtuPetModule::beginPatrolAction (PatrolAction action)
{
    currentPatrolAction = action;
    patrolCooldownTicksRemaining = 0;
    patrolLockedAnimId = 0;
    showSpeechBubble = false;
    currentSpeechText.clear();
    jumpFightActive = false;
    jumpFightTick = 0;

    switch (currentPatrolAction)
    {
        case PatrolAction::lookLeftSlow:
            patrolActionTicksRemaining = patrolCycleTicks;
            patrolFaceLeft = true;
            patrolMirrorX = false;
            patrolActionMoveDir = -1.0f;
            patrolActionSpeedPxPerTick = 0.45f;
            patrolLockedAnimId = (int) PetAnim::lookLeft;
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::lookRightSlow:
            patrolActionTicksRemaining = patrolCycleTicks;
            patrolFaceLeft = false;
            patrolMirrorX = true;
            patrolActionMoveDir = 1.0f;
            patrolActionSpeedPxPerTick = 0.45f;
            patrolLockedAnimId = (int) PetAnim::lookLeft;
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::moveLeftFast:
            patrolActionTicksRemaining = patrolFastMoveTicks;
            patrolFaceLeft = true;
            patrolMirrorX = false;
            patrolActionMoveDir = -1.0f;
            patrolActionSpeedPxPerTick = 1.25f;
            patrolLockedAnimId = (int) PetAnim::moveLeft;
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::moveRightFast:
            patrolActionTicksRemaining = patrolFastMoveTicks;
            patrolFaceLeft = false;
            patrolMirrorX = true;
            patrolActionMoveDir = 1.0f;
            patrolActionSpeedPxPerTick = 1.25f;
            patrolLockedAnimId = (int) PetAnim::moveLeft;
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::talk:
            patrolActionTicksRemaining = patrolCycleTicks;
            patrolFaceLeft = true;
            patrolMirrorX = false;
            patrolActionMoveDir = 0.0f;
            patrolActionSpeedPxPerTick = 0.0f;
            showSpeechBubble = true;
            currentSpeechText = randomIdleSpeech();
            patrolLockedAnimId = randomAnimFrom ({ (int) PetAnim::talkHappier,
                                                   (int) PetAnim::talkHappiest,
                                                   (int) PetAnim::talkShy });
            forceAnimation (patrolLockedAnimId);
            break;

        case PatrolAction::jumpFight:
            patrolActionTicksRemaining = jumpFightTotalTicks;
            patrolFaceLeft = true;
            patrolMirrorX = false;
            patrolActionMoveDir = 0.0f;
            patrolActionSpeedPxPerTick = 0.0f;
            jumpFightActive = true;
            jumpFightTick = 0;
            jumpFightBaseY = petPos.y;
            patrolLockedAnimId = (int) PetAnim::fight;
            forceAnimation (patrolLockedAnimId);
            break;
    }
}

void VirtuPetModule::stepJumpFight()
{
    if (! jumpFightActive)
        return;

    const float progress = (float) jumpFightTick / (float) juce::jmax (1, jumpFightTotalTicks - 1);
    const float phase = progress * juce::MathConstants<float>::twoPi * (float) jumpFightCount;
    const float hop = juce::jmax (0.0f, std::sin (phase));
    petPos.y = jumpFightBaseY - hop * jumpFightAmplitudePx;

    ++jumpFightTick;
    if (jumpFightTick >= jumpFightTotalTicks)
    {
        jumpFightActive = false;
        petPos.y = jumpFightBaseY;
    }
}

void VirtuPetModule::stepPatrolAction()
{
    if (motionMode != MotionMode::patrol)
        return;

    const bool allowMovement = ! focused;

    auto finishCurrentAction = [this]()
    {
        patrolActionTicksRemaining = 0;
        patrolCooldownTicksRemaining = patrolCycleTicks;
        patrolMirrorX = false;
        patrolActionMoveDir = 0.0f;
        patrolActionSpeedPxPerTick = 0.0f;
        jumpFightActive = false;

        patrolLockedAnimId = hasAnimation ((int) PetAnim::dazeThenWalkLeft)
                                 ? (int) PetAnim::dazeThenWalkLeft
                                 : 0;
        if (showSpeechBubble)
        {
            showSpeechBubble = false;
            currentSpeechText.clear();
        }
        if (patrolLockedAnimId > 0)
            forceAnimation (patrolLockedAnimId);
    };

    if (patrolActionTicksRemaining > 0)
    {
        if (allowMovement)
            petGroundAnchorX += patrolActionMoveDir * patrolActionSpeedPxPerTick;

        if (jumpFightActive)
            stepJumpFight();

        const bool isEdgeStopAction = currentPatrolAction == PatrolAction::lookLeftSlow
                                   || currentPatrolAction == PatrolAction::lookRightSlow
                                   || currentPatrolAction == PatrolAction::moveLeftFast
                                   || currentPatrolAction == PatrolAction::moveRightFast;

        if (isEdgeStopAction)
        {
            const auto frame = getCurrentFrame();
            if (! frame.isNull())
            {
                const auto area = getLocalBounds();
                const auto playArea = area.withTrimmedTop (juce::jmin (hudHeight, area.getHeight()));
                const float halfW = (float) frame.getWidth() * 0.5f;
                const float anchorMin = juce::jmin (halfW, (float) playArea.getWidth() - halfW);
                const float anchorMax = juce::jmax (halfW, (float) playArea.getWidth() - halfW);

                petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, petGroundAnchorX);

                const bool hitLeftEdge = petGroundAnchorX <= anchorMin + 0.01f;
                const bool hitRightEdge = petGroundAnchorX >= anchorMax - 0.01f;
                if (hitLeftEdge || hitRightEdge)
                {
                    finishCurrentAction();
                    return;
                }
            }
        }

        --patrolActionTicksRemaining;
        if (patrolActionTicksRemaining <= 0)
            finishCurrentAction();

        return;
    }

    if (patrolCooldownTicksRemaining > 0)
    {
        --patrolCooldownTicksRemaining;
        if (patrolCooldownTicksRemaining > 0)
            return;
    }

    juce::Array<PatrolAction> candidates;

    const auto area = getLocalBounds();
    const auto playArea = area.withTrimmedTop (juce::jmin (hudHeight, area.getHeight()));
    const float playCenterX = (float) playArea.getWidth() * 0.5f;

    if (petGroundAnchorX <= playCenterX)
    {
        // 在左侧：不随机到行为1和3（向左）
        candidates.add (PatrolAction::lookRightSlow);
        candidates.add (PatrolAction::moveRightFast);
    }
    else
    {
        // 在右侧：不随机到行为2和4（向右）
        candidates.add (PatrolAction::lookLeftSlow);
        candidates.add (PatrolAction::moveLeftFast);
    }

    candidates.add (PatrolAction::talk);
    candidates.add (PatrolAction::jumpFight);

    beginPatrolAction (candidates.getReference (juce::Random::getSystemRandom().nextInt (candidates.size())));
}

juce::String VirtuPetModule::getRoleName() const noexcept
{
    return roleName;
}

float VirtuPetModule::getHunger() const noexcept
{
    return hunger;
}

float VirtuPetModule::getHealth() const noexcept
{
    return health;
}

void VirtuPetModule::restorePersistentState (const juce::String& savedRoleName,
                                               float savedHunger,
                                               float savedHealth)
{
    hunger = juce::jlimit (0.0f, 100.0f, savedHunger);
    health = juce::jlimit (0.0f, 100.0f, savedHealth);

    if (savedRoleName.isEmpty())
    {
        // 新建模块：保留构造阶段已初始化的随机角色与蛋状态，仅恢复数值
        repaintSelfAndParent();
        return;
    }

    const auto root = findVirtuPetAssetsRoot();
    const auto mirrorRoot = findVirtuPetMirrorAssetsRoot();
    if (root.isDirectory())
    {
        juce::Array<juce::File> roleDirs;
        root.findChildFiles (roleDirs, juce::File::findDirectories, false);

        for (const auto& dir : roleDirs)
        {
            if (dir.getFileName() == savedRoleName)
            {
                if (loadRoleFramesFromDirectory (dir, mirrorRoot, animFrames, animFramesRight, availableAnimIds))

                {

                    roleName = savedRoleName;
                    currentAnimId = 1;
                    currentFrameIdx = 0;
                    hasPetPosition = false;
                    motionMode = MotionMode::patrol;
                    beginPatrolCycle();
                    repaintSelfAndParent();
                    return;
                }
                break;
            }
        }
    }

    // 如果找不到历史角色，保留当前随机角色，仅恢复数值
    beginPatrolCycle();
    repaintSelfAndParent();
}

void VirtuPetModule::stepWander()

{
    const auto oldMode = motionMode;
    const int oldAnimId = currentAnimId;
    const int oldFrameIdx = currentFrameIdx;
    const auto oldPos = petPos;
    const bool oldBubbleVisible = showSpeechBubble;
    const auto oldBubbleText = currentSpeechText;
    const auto oldFrame = getCurrentFrame();
    const auto oldPetBounds = getPetVisualBoundsFor (oldFrame, oldPos, oldBubbleVisible, oldBubbleText, oldMode);

    const auto frame = getCurrentFrame();
    if (frame.isNull())
        return;

    const auto area = getLocalBounds();
    const auto playArea = area.withTrimmedTop (juce::jmin (hudHeight, area.getHeight()));
    const float halfW = (float) frame.getWidth() * 0.5f;
    const float anchorMin = juce::jmin (halfW, (float) playArea.getWidth() - halfW);
    const float anchorMax = juce::jmax (halfW, (float) playArea.getWidth() - halfW);
    const float floorY = (float) juce::jmax (0, area.getHeight() - frame.getHeight());

    if (! hasPetPosition)
    {
        petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, (anchorMin + anchorMax) * 0.5f);
        petPos = { juce::jlimit (0.0f, (float) juce::jmax (0, playArea.getWidth() - frame.getWidth()), petGroundAnchorX - halfW), floorY };
        jumpFightBaseY = floorY;
        hasPetPosition = true;
    }

    switch (motionMode)
    {
        case MotionMode::egg:
        case MotionMode::hatching:
            petPos.y = floorY;
            break;

        case MotionMode::toilet:
            petPos.y = floorY;
            toiletTicksRemaining = juce::jmax (0, toiletTicksRemaining - 1);
            break;

        case MotionMode::patrol:
        {
            if (! jumpFightActive)
                petPos.y = floorY;

            stepPatrolAction();
            break;
        }

        case MotionMode::startledIntro:
            break;

        case MotionMode::falling:
        {
            fallVelocityPxPerTick += gravityPxPerTick2;
            petPos.y += fallVelocityPxPerTick;

            if (petPos.y >= floorY)
            {
                petPos.y = floorY;
                switchMotionMode (MotionMode::landingFall);
            }

            break;
        }

        case MotionMode::landingFall:
            petPos.y = floorY;
            break;

        case MotionMode::carried:
            // petPos 同步（含夹持与锚点更新）已由 mouseDrag 在每次鼠标移动时实时完成。
            // 动画帧推进由 stepOneFrame + flushVisualRepaintQueue 处理。
            // 此处不做额外计算，避免与 mouseDrag 重绘路径竞争导致 non-opaque 组件闪烁。
            break;

        case MotionMode::drowsy:
        case MotionMode::sleeping:
            petPos.y = floorY;
            break;

        case MotionMode::eating:
        {
            petPos.y = floorY;

            if (--eatingAnimTicksRemaining <= 0)
            {
                const int oldEatingAnimId = eatingCurrentAnimId;
                for (int attempt = 0; attempt < 4; ++attempt)
                {
                    eatingCurrentAnimId = randomAnimFrom ({ (int) PetAnim::runLeftToFood,
                                                            (int) PetAnim::runLeftToHate,
                                                            (int) PetAnim::runLeftToLike,
                                                            (int) PetAnim::eatLeft });
                    if (eatingCurrentAnimId != oldEatingAnimId)
                        break;
                }

                eatingAnimTicksRemaining = eatingSwitchTicks;
                forceAnimation (eatingCurrentAnimId, true);
            }

            if (--eatingMoveRetargetTicksRemaining <= 0)
            {
                eatingMoveDir = juce::Random::getSystemRandom().nextBool() ? 1.0f : -1.0f;
                eatingMoveRetargetTicksRemaining = juce::Random::getSystemRandom().nextInt (ticksPerSecond * 2) + ticksPerSecond / 2;
            }

            eatingMirrorX = (eatingMoveDir > 0.0f);
            const float eatingSpeedByHealth = juce::jmap (juce::jlimit (0.0f, 100.0f, health),
                                                          0.0f,
                                                          100.0f,
                                                          eatingMoveSpeedPxPerTick * 0.5f,
                                                          eatingMoveSpeedPxPerTick * 1.8f);
            petGroundAnchorX += eatingMoveDir * eatingSpeedByHealth;
            if (petGroundAnchorX <= anchorMin + 0.01f)
            {
                petGroundAnchorX = anchorMin;
                eatingMoveDir = 1.0f;
                eatingMirrorX = true;
            }
            else if (petGroundAnchorX >= anchorMax - 0.01f)
            {
                petGroundAnchorX = anchorMax;
                eatingMoveDir = -1.0f;
                eatingMirrorX = false;
            }
            break;
        }

        case MotionMode::hungry:
        case MotionMode::starving:
        case MotionMode::sick:
        case MotionMode::criticalSick:
        case MotionMode::dead:
            petPos.y = floorY;
            break;
    }

    petGroundAnchorX = juce::jlimit (anchorMin, anchorMax, petGroundAnchorX);

    const float minX = 0.0f;
    const float maxX = (float) juce::jmax (0, playArea.getWidth() - frame.getWidth());
    petPos.x = juce::jlimit (minX, maxX, petGroundAnchorX - halfW);
    petPos.y = juce::jlimit ((float) playArea.getY(), floorY, petPos.y);

    const auto newPetBounds = getCurrentPetVisualBounds();
    const bool moved = std::abs (petPos.x - oldPos.x) > 0.01f || std::abs (petPos.y - oldPos.y) > 0.01f;
    const bool changed = moved
                      || oldMode != motionMode
                      || oldAnimId != currentAnimId
                      || oldFrameIdx != currentFrameIdx
                      || oldBubbleVisible != showSpeechBubble
                      || oldBubbleText != currentSpeechText
                      || oldPetBounds != newPetBounds;

    if (changed)
        enqueuePetDirtyRepaint (oldPetBounds.getUnion (newPetBounds));
}

juce::Rectangle<int> VirtuPetModule::getFocusBounds() const
{
    return getLocalBounds().reduced (1);
}

juce::Rectangle<int> VirtuPetModule::getDeleteButtonBounds() const
{
    auto focus = getFocusBounds();
    return { focus.getRight() - deleteButtonSize - 2, focus.getY() + 2, deleteButtonSize, deleteButtonSize };
}

// 悬停提示：VirtuPet 没有标题栏，只有聚焦后右上角的自绘 × 删除按钮
ModulePanel::HoverHelp VirtuPetModule::getHoverHelp (juce::Point<int> localPos) const
{
    HoverHelp h;

    if (focused && getDeleteButtonBounds().contains (localPos))
    {
        h.text = isPanelLayoutLocked (*this)
            ? y2k::help::make ("Remove pet", "Layout is locked (L) - unlock it to remove the pet.")
            : y2k::help::make ("Remove pet", "Say goodbye to this pet (asks for confirmation). "
                                             "A new egg can be added with a right-click.");
        return h;
    }

    h.text = y2k::help::moduleHelp (ModuleType::virtuPet);
    if (isInEggPhase())
        h.text << "\n\nStill an egg: play some music to hatch it!";
    h.contentZone = true;
    return h;
}

VirtuPetModule::Edge VirtuPetModule::detectEdge (juce::Point<int> pos) const
{
    const auto b = getLocalBounds();
    const bool nearRight  = pos.x >= b.getRight()  - edgeHotSize;
    const bool nearBottom = pos.y >= b.getBottom() - edgeHotSize;

    if (nearRight && nearBottom) return Edge::bottomRight;
    if (nearRight)               return Edge::right;
    if (nearBottom)              return Edge::bottom;
    return Edge::none;
}

void VirtuPetModule::updateCursorFor (Edge e)
{
    switch (e)
    {
        case Edge::right:        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Edge::bottom:       setMouseCursor (juce::MouseCursor::UpDownResizeCursor); break;
        case Edge::bottomRight:  setMouseCursor (juce::MouseCursor::BottomRightCornerResizeCursor); break;
        default:                 setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}

// ==========================================================
// VirtuPetConfirmOverlay —— 删除二次确认覆盖层
//   添加到 workspace 层级渲染，不受模块边界裁剪。
// ==========================================================

static juce::Rectangle<int> calcDlgBounds (const juce::Rectangle<int>& modArea)
{
    const int dlgW = juce::jlimit (160, 220, modArea.getWidth()  - 16);
    const int dlgH = juce::jlimit (90,  110, modArea.getHeight() - 16);
    return juce::Rectangle<int> (dlgW, dlgH).withCentre (modArea.getCentre());
}

VirtuPetConfirmOverlay::VirtuPetConfirmOverlay (
    const juce::Rectangle<int>& moduleBoundsInWorkspace,
    std::function<void()> onConfirm,
    std::function<void()> onDismiss)
    : onConfirmCb (std::move (onConfirm))
    , onDismissCb (std::move (onDismiss))
{
    // 对话框区域（workspace 坐标，居中于模块）
    const auto dlgInWorkspace = calcDlgBounds (moduleBoundsInWorkspace);

    // 覆盖层区域 = 模块 + 对话框的并集（确保两者都不被裁剪）
    const auto totalBounds = moduleBoundsInWorkspace.getUnion (dlgInWorkspace);
    setBounds (totalBounds);

    // 转为相对本组件坐标
    moduleArea = moduleBoundsInWorkspace.withPosition (
        moduleBoundsInWorkspace.getPosition() - getPosition());
    dlgBounds  = dlgInWorkspace.withPosition (
        dlgInWorkspace.getPosition() - getPosition());

    setOpaque (false);
    setInterceptsMouseClicks (true, true);
}

void VirtuPetConfirmOverlay::paint (juce::Graphics& g)
{
    const auto& tokens = PinkXP::getSkinTokens();

    // 半透明遮罩（仅覆盖模块区域，不遮住 workspace 其他内容）
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRect (moduleArea);

    // 对话框外壳：全风格统一经由 PinkXP::drawRaised
    PinkXP::drawRaised (g, dlgBounds, PinkXP::face);

    if (dlgBounds.getHeight() < 52) return;

    // 标题栏：使用 PinkXP::drawPinkTitleBar（彻底消除绕过标题栏工艺的原语分叉）
    const int titleH = juce::jmin (20, dlgBounds.getHeight() - 32);
    const juce::Rectangle<int> titleRow (dlgBounds.getX() + 2, dlgBounds.getY() + 2,
                                         juce::jmax (1, dlgBounds.getWidth() - 4), titleH);
    PinkXP::drawPinkTitleBar (g, titleRow, "r u sure?", 10.0f);
    g.setColour (PinkXP::titleBarDividerColour());
    g.fillRect (titleRow.getX(), titleRow.getBottom(), titleRow.getWidth(), 1);

    auto cancelBounds  = getButtonBounds (0);
    auto confirmBounds = getButtonBounds (1);

    // 正文卡片槽（Craft::drawPanelWell(..., WellKind::card)；键控模式下忠实跟随当前 SkinId）
    const bool modernDlg = (tokens.frameStyle == PinkXP::FrameStyle::modernStudio);
    const bool chromeDlg = (tokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool glassDlg  = (tokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool neonDlg   = (tokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool crtDlg    = (tokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool isDarkFace = PinkXP::face.getPerceivedBrightness() < 0.45f;
    const auto cardFill = modernDlg
                            ? (isDarkFace ? PinkXP::face.interpolatedWith (juce::Colours::white, 0.048f)
                                          : PinkXP::content)
                        : chromeDlg
                            ? (isDarkFace ? PinkXP::face.interpolatedWith (juce::Colours::white, 0.085f)
                                          : PinkXP::face.interpolatedWith (PinkXP::content, 0.48f))
                        : glassDlg
                            ? (isDarkFace ? PinkXP::face.interpolatedWith (juce::Colours::white, 0.08f)
                                          : PinkXP::face.interpolatedWith (juce::Colours::white, 0.44f))
                        : neonDlg
                            ? (isDarkFace ? PinkXP::face.interpolatedWith (juce::Colours::black, 0.24f)
                                          : PinkXP::face.interpolatedWith (PinkXP::content, 0.48f))
                            : PinkXP::content;
    const int bodyTop = titleRow.getBottom() + 4;
    const int bodyH   = juce::jmax (1, cancelBounds.getY() - bodyTop - ((chromeDlg || glassDlg || neonDlg || crtDlg) ? 6 : 4));
    const juce::Rectangle<int> bodyArea (dlgBounds.getX() + 6, bodyTop,
                                         juce::jmax (1, dlgBounds.getWidth() - 12), bodyH);
    if (bodyArea.getHeight() >= 14)
    {
        if (tokens.isChromaKey)
            drawKeyedPetCard (g, bodyArea, cardFill, tokens);
        else
            PinkXP::Craft::drawPanelWell (g, bodyArea, cardFill, PinkXP::WellKind::card);

        g.setColour (PinkXP::readableInkOn (cardFill));
        g.setFont (PinkXP::getFont (9.0f));
        g.drawFittedText (y2k::tr ("say bye to ur pet? :)\ncan't undo this~"),
                          bodyArea.reduced (4, 2),
                          juce::Justification::centred, 2);
    }

    if ((chromeDlg || glassDlg || neonDlg || crtDlg) && cancelBounds.getY() - bodyArea.getBottom() >= 4)
    {
        const int seamY = cancelBounds.getY() - 3;
        const int seamW = juce::jmax (1, dlgBounds.getWidth() - 12);
        if (chromeDlg)
        {
            g.setColour (PinkXP::dark.withAlpha (isDarkFace ? 0.72f : 0.48f));
            g.fillRect (dlgBounds.getX() + 6, seamY, seamW, 1);
            g.setColour (PinkXP::hl.interpolatedWith (juce::Colours::white, 0.50f)
                                   .withAlpha (isDarkFace ? 0.26f : 0.74f));
            g.fillRect (dlgBounds.getX() + 6, seamY + 1, seamW, 1);
        }
        else if (glassDlg)
        {
            g.setColour (PinkXP::dark.withAlpha (isDarkFace ? 0.28f : 0.16f));
            g.fillRect (dlgBounds.getX() + 6, seamY, seamW, 1);
            g.setColour (juce::Colours::white.withAlpha (isDarkFace ? 0.16f : 0.65f));
            g.fillRect (dlgBounds.getX() + 6, seamY + 1, seamW, 1);
        }
        else
        {
            g.setColour (PinkXP::sel.withAlpha (neonDlg ? 0.72f : 0.65f));
            g.fillRect (dlgBounds.getX() + 6, seamY, seamW, 1);
        }
    }

    auto drawDlgBtn = [&] (juce::Rectangle<int> r, const char* key, bool hovered, bool primary)
    {
        juce::Colour fill;
        PinkXP::ControlState state = hovered ? PinkXP::ControlState::hover : PinkXP::ControlState::normal;
        if (modernDlg)
        {
            if (primary)
            {
                // Modern Minimal 主确认键：清晰强调色胶囊，与中性 Cancel 形成明确层级
                const auto rf = r.toFloat();
                const float rad = juce::jmin (tokens.controlRadius, rf.getHeight() * 0.45f);
                fill = hovered ? PinkXP::sel.brighter (0.08f) : PinkXP::sel;
                g.setColour (fill);
                g.fillRoundedRectangle (rf, rad);
                g.setColour (PinkXP::sel.darker (0.18f));
                g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
            }
            else
            {
                fill = PinkXP::btnFace;
                PinkXP::Craft::drawControlChrome (g, r, fill, state);
            }
        }
        else if (chromeDlg || glassDlg || neonDlg || crtDlg)
        {
            // Chrome / Glass / Neon / CRT：风格原生硬件按钮（主确认键带 active 强调态）
            fill = (crtDlg && (primary || hovered)) ? PinkXP::sel : PinkXP::btnFace;
            const auto craftState = hovered ? PinkXP::ControlState::hover
                                  : primary ? PinkXP::ControlState::active
                                            : PinkXP::ControlState::normal;
            PinkXP::Craft::drawControlChrome (g, r, fill, craftState);
        }
        else
        {
            fill = hovered ? PinkXP::pink200 : PinkXP::btnFace;
            PinkXP::Craft::drawControlChrome (g, r, fill, state);
        }

        const auto font = PinkXP::fitFontToHeight (PinkXP::getFont (9.0f, juce::Font::bold),
                                                   (float) juce::jmax (1, r.getHeight()));
        g.setColour (PinkXP::readableInkOn (fill));
        g.setFont (font);
        g.drawText (y2k::tr (key),
                    r.toFloat().translated (0.0f, PinkXP::opticalNudge (font)),
                    juce::Justification::centred, false);
    };

    drawDlgBtn (cancelBounds,  "Cancel", hoveredBtn == 0, false);
    drawDlgBtn (confirmBounds, "OK",     hoveredBtn == 1, true);
}

void VirtuPetConfirmOverlay::mouseDown (const juce::MouseEvent& e)
{
    const int hit = hitTestButton (e.getPosition());
    if (hit == 1)       // 确认 → 删除模块
    {
        if (onConfirmCb)
            onConfirmCb();
    }
    else                // 取消 / 点击对话框外
    {
        if (onDismissCb)
            onDismissCb();
    }
}

void VirtuPetConfirmOverlay::mouseMove (const juce::MouseEvent& e)
{
    const int hit = hitTestButton (e.getPosition());
    if (hit != hoveredBtn)
    {
        hoveredBtn = hit;
        repaint();
    }
}

void VirtuPetConfirmOverlay::dismiss()
{
    if (onDismissCb)
        onDismissCb();
}

juce::Rectangle<int> VirtuPetConfirmOverlay::getButtonBounds (int idx) const
{
    auto inner = dlgBounds.reduced (6);
    inner.removeFromTop (juce::jmin (18, inner.getHeight() - 24));

    const int btnH = 20;
    auto btnRow = juce::Rectangle<int> (inner.getX(), inner.getBottom() - btnH,
                                         inner.getWidth(), btnH);
    const int btnW = juce::jmin (55, (btnRow.getWidth() - 8) / 2);

    if (idx == 0)
        return juce::Rectangle<int> (btnRow.getCentreX() - btnW - 4,
                                      btnRow.getY(), btnW, btnH);
    else
        return juce::Rectangle<int> (btnRow.getCentreX() + 4,
                                      btnRow.getY(), btnW, btnH);
}

int VirtuPetConfirmOverlay::hitTestButton (juce::Point<int> pos) const
{
    if (getButtonBounds (1).contains (pos)) return 1;
    if (getButtonBounds (0).contains (pos)) return 0;
    return -1;
}

// ==========================================================
// VirtuPetModule 覆盖层管理
// ==========================================================
void VirtuPetModule::showConfirmOverlay()
{
    if (confirmOverlay != nullptr) return;

    auto* ws = getParentComponent();
    if (ws == nullptr) return;

    confirmOverlay = std::make_unique<VirtuPetConfirmOverlay> (
        getBounds(),  // 模块在 workspace 坐标系中的矩形

        // 确认 → 删除模块
        [this]()
        {
            confirmOverlay.reset();
            if (onCloseClicked)
                onCloseClicked (*this);
        },

        // 取消 → 关闭覆盖层
        [this]()
        {
            confirmOverlay.reset();
            repaintSelfAndParent();
        });

    ws->addAndMakeVisible (*confirmOverlay);
    confirmOverlay->toFront (false);
}

void VirtuPetModule::dismissConfirmOverlay()
{
    if (confirmOverlay != nullptr)
    {
        confirmOverlay.reset();
        repaintSelfAndParent();
    }
}