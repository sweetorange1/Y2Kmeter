#pragma once

#include <JuceHeader.h>
#include <functional>
#include <vector>

#include "source/ui/modules/MilkdropVisualState.h"

// ==========================================================
// MilkdropEffect —— Milkdrop 后处理效果系统的可扩展注册表
//
// 设计目标：将原先散落在 MilkdropModule 中的开关型效果（invert / shadows）
// 抽象为"声明即注册"的插拔式效果表。effects 面板 UI 完全由注册表驱动，
// 新增一个开关型效果只需：
//   1. 在 MilkdropVisualState 中追加对应 bool 字段并纳入 isNeutral()；
//   2. 在本文件 GetMilkdropEffectDefs() 中追加一行注册项（含 get/set lambda）；
//   3. 在 MilkdropTintPass 的 shader 中追加对应分支 + uniform；
//   4. 在 PluginProcessor 持久化中追加对应顶层属性。
// 无需再改动 effects 面板布局代码。
// ==========================================================

// 开关型效果的唯一标识（color 面板的 bright 为参数化效果，不在此列）。
// 对齐 MilkDrop3 Effect Injection（ADDMILKEFFECT）目录。
enum class MilkdropEffectId
{
    kInvert,    // ret = 1 - ret（effbottom）
    kShadows,   // ret += pow(gray(flip(uv)), 2)（effbottom，加性叠加，不压暗）
    kSolarize,  // ret = ret*(1-ret)*4（effbottom）
    kSplit,     // uv = float2(abs(uv.x-0.5), uv.y)（efftop）
    kZoom,      // uv = 0.25 + 0.5*uv（efftop）
    kMulti,     // uv 多重折叠 multiplicate（efftop）
    kRainbow,   // 彩虹染色（effbottom，程序化降级）
    kBlow,      // 加性模糊叠加 ret += blur(uv)（effbottom，近似）
    kBurn,      // 灼烧混合 overlay（effbottom，近似）
    kKaleidoscope,  // 万花筒：极坐标角度折叠（efftop）
    kSwirl,         // 漩涡扭曲：绕中心旋转（efftop）
    kPinch,         // 鱼眼/挤压：径向缩放（efftop）
    kPixelate,      // 像素化：uv 量化（efftop）
    kGlitch,        // 故障色差：RGB 通道微偏移采样（effbottom 采样）
    kPosterize,     // 色调分离：ret 量化（effbottom）
    kSepia,         // 复古棕褐：颜色矩阵（effbottom）
    kGrayscale,     // 灰度：亮度加权（effbottom）
    kEdge,          // 边缘检测：邻域差分（effbottom）
    kVignette,      // 暗角：径向暗化（effbottom）
    kTunnel,        // 隧道纵深：极坐标映射（efftop）
    kRipple,        // 同心水波涟漪：径向正弦扰动（efftop）
    kMelt,          // 融化下垂：uv.y 正弦下垂（efftop）
    kFisheye,       // 超广角鱼眼：强桶形畸变（efftop）
    kNoiseWarp,     // 高频噪波扰动：uv 伪随机偏移（efftop）
    kMirrorMaze,    // 无限镜像嵌套：递归折叠（efftop）
    kFragment,      // 像素破碎：小块打乱（efftop）
    kSpiral,        // 螺旋扭曲：角度+半径复合旋转（efftop）
    kTwist,         // 垂直扭转：沿 y 旋转（efftop）
    kColorShift,    // 色彩爆炸：RGB 通道大幅分离+增强（effbottom）
    kNeon,          // 霓虹光晕：亮部提取+光晕扩散（effbottom）
    kThermal,       // 热成像：亮度映射热力色谱（effbottom）
    kAcid,          // 酸性色调：高对比绿品红偏移（effbottom）
    kVhs,           // VHS 故障：横向条纹+色差+噪点（effbottom）
    kCrt,           // CRT 扫描线：扫描线+磷光（effbottom）
    kDuotone,       // 双色调：亮度映射双色（effbottom）
    kBloom,         // 泛光爆发：亮部模糊扩散叠加（effbottom）
    kBinary,        // 极端二值化：硬阈值黑白（effbottom）
    kPrismatic,     // 棱镜色散：径向色差（effbottom）
};

// 单个开关型效果的元数据，供 effects 面板动态生成 UI。
struct MilkdropEffectDef
{
    MilkdropEffectId id;
    const char* display_name;                       // UI 显示名
    bool implemented;                               // 是否已实现（false 时 UI 不显示）
    std::function<bool (const MilkdropVisualState&)> get;
    std::function<void (MilkdropVisualState&, bool)> set;
};

// 返回开关型效果的注册表（静态单例，避免反复构造 vector）。
inline const std::vector<MilkdropEffectDef>& GetMilkdropEffectDefs()
{
    static const std::vector<MilkdropEffectDef> defs = {
        { MilkdropEffectId::kInvert, "invert", true,
          [] (const MilkdropVisualState& s) { return s.invert; },
          [] (MilkdropVisualState& s, bool v) { s.invert = v; } },

        { MilkdropEffectId::kShadows, "shadows", true,
          [] (const MilkdropVisualState& s) { return s.shadows; },
          [] (MilkdropVisualState& s, bool v) { s.shadows = v; } },

        { MilkdropEffectId::kSolarize, "solarize", true,
          [] (const MilkdropVisualState& s) { return s.solarize; },
          [] (MilkdropVisualState& s, bool v) { s.solarize = v; } },

        { MilkdropEffectId::kSplit, "split", true,
          [] (const MilkdropVisualState& s) { return s.split; },
          [] (MilkdropVisualState& s, bool v) { s.split = v; } },

        { MilkdropEffectId::kZoom, "zoom", true,
          [] (const MilkdropVisualState& s) { return s.zoom; },
          [] (MilkdropVisualState& s, bool v) { s.zoom = v; } },

        { MilkdropEffectId::kMulti, "multi", true,
          [] (const MilkdropVisualState& s) { return s.multi; },
          [] (MilkdropVisualState& s, bool v) { s.multi = v; } },

        { MilkdropEffectId::kRainbow, "rainbow", true,
          [] (const MilkdropVisualState& s) { return s.rainbow; },
          [] (MilkdropVisualState& s, bool v) { s.rainbow = v; } },

        { MilkdropEffectId::kBlow, "blow", true,
          [] (const MilkdropVisualState& s) { return s.blow; },
          [] (MilkdropVisualState& s, bool v) { s.blow = v; } },

        { MilkdropEffectId::kBurn, "burn", true,
          [] (const MilkdropVisualState& s) { return s.burn; },
          [] (MilkdropVisualState& s, bool v) { s.burn = v; } },

        { MilkdropEffectId::kKaleidoscope, "kaleidoscope", true,
          [] (const MilkdropVisualState& s) { return s.kaleidoscope; },
          [] (MilkdropVisualState& s, bool v) { s.kaleidoscope = v; } },

        { MilkdropEffectId::kSwirl, "swirl", true,
          [] (const MilkdropVisualState& s) { return s.swirl; },
          [] (MilkdropVisualState& s, bool v) { s.swirl = v; } },

        { MilkdropEffectId::kPinch, "pinch", true,
          [] (const MilkdropVisualState& s) { return s.pinch; },
          [] (MilkdropVisualState& s, bool v) { s.pinch = v; } },

        { MilkdropEffectId::kPixelate, "pixelate", true,
          [] (const MilkdropVisualState& s) { return s.pixelate; },
          [] (MilkdropVisualState& s, bool v) { s.pixelate = v; } },

        { MilkdropEffectId::kGlitch, "glitch", true,
          [] (const MilkdropVisualState& s) { return s.glitch; },
          [] (MilkdropVisualState& s, bool v) { s.glitch = v; } },

        { MilkdropEffectId::kPosterize, "posterize", true,
          [] (const MilkdropVisualState& s) { return s.posterize; },
          [] (MilkdropVisualState& s, bool v) { s.posterize = v; } },

        { MilkdropEffectId::kSepia, "sepia", true,
          [] (const MilkdropVisualState& s) { return s.sepia; },
          [] (MilkdropVisualState& s, bool v) { s.sepia = v; } },

        { MilkdropEffectId::kGrayscale, "grayscale", true,
          [] (const MilkdropVisualState& s) { return s.grayscale; },
          [] (MilkdropVisualState& s, bool v) { s.grayscale = v; } },

        { MilkdropEffectId::kEdge, "edge", true,
          [] (const MilkdropVisualState& s) { return s.edge; },
          [] (MilkdropVisualState& s, bool v) { s.edge = v; } },

        { MilkdropEffectId::kVignette, "vignette", true,
          [] (const MilkdropVisualState& s) { return s.vignette; },
          [] (MilkdropVisualState& s, bool v) { s.vignette = v; } },

        { MilkdropEffectId::kTunnel, "tunnel", true,
          [] (const MilkdropVisualState& s) { return s.tunnel; },
          [] (MilkdropVisualState& s, bool v) { s.tunnel = v; } },

        { MilkdropEffectId::kRipple, "ripple", true,
          [] (const MilkdropVisualState& s) { return s.ripple; },
          [] (MilkdropVisualState& s, bool v) { s.ripple = v; } },

        { MilkdropEffectId::kMelt, "melt", true,
          [] (const MilkdropVisualState& s) { return s.melt; },
          [] (MilkdropVisualState& s, bool v) { s.melt = v; } },

        { MilkdropEffectId::kFisheye, "fisheye", true,
          [] (const MilkdropVisualState& s) { return s.fisheye; },
          [] (MilkdropVisualState& s, bool v) { s.fisheye = v; } },

        { MilkdropEffectId::kNoiseWarp, "noise_warp", true,
          [] (const MilkdropVisualState& s) { return s.noise_warp; },
          [] (MilkdropVisualState& s, bool v) { s.noise_warp = v; } },

        { MilkdropEffectId::kMirrorMaze, "mirror_maze", true,
          [] (const MilkdropVisualState& s) { return s.mirror_maze; },
          [] (MilkdropVisualState& s, bool v) { s.mirror_maze = v; } },

        { MilkdropEffectId::kFragment, "fragment", true,
          [] (const MilkdropVisualState& s) { return s.fragment; },
          [] (MilkdropVisualState& s, bool v) { s.fragment = v; } },

        { MilkdropEffectId::kSpiral, "spiral", true,
          [] (const MilkdropVisualState& s) { return s.spiral; },
          [] (MilkdropVisualState& s, bool v) { s.spiral = v; } },

        { MilkdropEffectId::kTwist, "twist", true,
          [] (const MilkdropVisualState& s) { return s.twist; },
          [] (MilkdropVisualState& s, bool v) { s.twist = v; } },

        { MilkdropEffectId::kColorShift, "color_shift", true,
          [] (const MilkdropVisualState& s) { return s.color_shift; },
          [] (MilkdropVisualState& s, bool v) { s.color_shift = v; } },

        { MilkdropEffectId::kNeon, "neon", true,
          [] (const MilkdropVisualState& s) { return s.neon; },
          [] (MilkdropVisualState& s, bool v) { s.neon = v; } },

        { MilkdropEffectId::kThermal, "thermal", true,
          [] (const MilkdropVisualState& s) { return s.thermal; },
          [] (MilkdropVisualState& s, bool v) { s.thermal = v; } },

        { MilkdropEffectId::kAcid, "acid", true,
          [] (const MilkdropVisualState& s) { return s.acid; },
          [] (MilkdropVisualState& s, bool v) { s.acid = v; } },

        { MilkdropEffectId::kVhs, "vhs", true,
          [] (const MilkdropVisualState& s) { return s.vhs; },
          [] (MilkdropVisualState& s, bool v) { s.vhs = v; } },

        { MilkdropEffectId::kCrt, "crt", true,
          [] (const MilkdropVisualState& s) { return s.crt; },
          [] (MilkdropVisualState& s, bool v) { s.crt = v; } },

        { MilkdropEffectId::kDuotone, "duotone", true,
          [] (const MilkdropVisualState& s) { return s.duotone; },
          [] (MilkdropVisualState& s, bool v) { s.duotone = v; } },

        { MilkdropEffectId::kBloom, "bloom", true,
          [] (const MilkdropVisualState& s) { return s.bloom; },
          [] (MilkdropVisualState& s, bool v) { s.bloom = v; } },

        { MilkdropEffectId::kBinary, "binary", true,
          [] (const MilkdropVisualState& s) { return s.binary; },
          [] (MilkdropVisualState& s, bool v) { s.binary = v; } },

        { MilkdropEffectId::kPrismatic, "prismatic", true,
          [] (const MilkdropVisualState& s) { return s.prismatic; },
          [] (MilkdropVisualState& s, bool v) { s.prismatic = v; } },
    };
    return defs;
}

// 返回"已实现"效果的数量（effects 面板据此计算动态高度）。
inline int CountImplementedMilkdropEffects()
{
    int count = 0;
    for (const auto& def : GetMilkdropEffectDefs())
        if (def.implemented)
            ++count;
    return count;
}

// 返回第 row 个"已实现"效果的注册项（row 越界返回 nullptr）。
// effects 面板的 hit-test 与绘制按 row 遍历已实现效果，新增效果后无需改布局代码。
inline const MilkdropEffectDef* GetImplementedMilkdropEffect (int row)
{
    for (const auto& def : GetMilkdropEffectDefs())
    {
        if (! def.implemented)
            continue;
        if (row == 0)
            return &def;
        --row;
    }
    return nullptr;
}
