#pragma once

#include <algorithm>
#include <cmath>

// ==========================================================
// MilkdropVisualOffsetState —— Milkdrop 后处理 uv 几何畸变的数据载体
//
// 这些参数原本按"注入 per_frame 变量"实现（在预设代码末尾追加
// `zoom=zoom+delta;` 等语句）。但该方案只对 warp shader 引用了这些变量的
// 预设生效，且需要重载预设，无法实时。现已改为**后处理层 uv 重映射**：
//   · 数据作为 MilkdropVisualState 的成员（offset），随视觉状态每帧传递到
//     MilkdropTintPass::apply，在采样 projectM 输出纹理前对 uv 做几何变换；
//   · 不修改 .milk 预设，且每帧实时生效（无需重载预设）。
//
// 字段语义：
// 连续浮点控制器（偏移量，0 = 中性）：
//   value[0] zoom —— 整体缩放偏移
//   value[1] rot  —— 整体旋转偏移
//   value[2] warp —— 径向扭曲强度
//   value[3] dx   —— 水平平移
//   value[4] dy   —— 垂直平移
//   value[5] sx   —— 水平拉伸偏移
//   value[6] sy   —— 垂直拉伸偏移
// 整数控制器（万花镜对称，0 = 关闭）：
//   ivalue[0] kaleido —— 径向角度瓣数（1~16，围绕中心做镜像折叠）
//   ivalue[1] fold_x  —— 水平对称折叠次数（1~16）
//   ivalue[2] fold_y  —— 垂直对称折叠次数（1~16）
// ==========================================================
struct MilkdropVisualOffsetState
{
    // 7 个连续浮点畸变偏移量（索引与 GetVisualOffsetParams() 元数据一一对应）。
    static constexpr int kParamCount = 7;
    float value[kParamCount] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    // 3 个整数控制器（索引与 GetVisualOffsetIntParams() 元数据一一对应）。
    static constexpr int kIntParamCount = 3;
    int ivalue[kIntParamCount] = {0, 0, 0};

    // 全部控制器均为中性/关闭时返回 true（不进行任何 uv 变换）。
    bool isNeutral() const noexcept
    {
        for (int i = 0; i < kParamCount; ++i)
            if (std::fabs(value[i]) > 1e-6f)
                return false;
        for (int i = 0; i < kIntParamCount; ++i)
            if (ivalue[i] != 0)
                return false;
        return true;
    }

    // 恢复默认（所有浮点归零、整数归零），供 Reset 按钮调用。
    void reset() noexcept
    {
        for (int i = 0; i < kParamCount; ++i)
            value[i] = 0.0f;
        for (int i = 0; i < kIntParamCount; ++i)
            ivalue[i] = 0;
    }
};

// 每个连续浮点畸变参数的原数据：UI 显示名 / 滑块取值范围 [min_value, max_value]。
struct VisualOffsetParamMeta
{
    const char* display_name;  // UI 标签
    float       min_value;     // 滑块最小值
    float       max_value;     // 滑块最大值
};

// 获取 7 个连续浮点参数的元数据表（顺序与 MilkdropVisualOffsetState::value 一致）。
inline const VisualOffsetParamMeta* GetVisualOffsetParams()
{
    static const VisualOffsetParamMeta kParams[MilkdropVisualOffsetState::kParamCount] = {
        {"zoom", -0.3f, 1.0f},
        {"rot",  -1.0f, 1.0f},
        {"warp", -1.0f, 1.0f},
        {"dx",   -1.0f, 1.0f},
        {"dy",   -1.0f, 1.0f},
        {"sx",   -1.0f, 1.0f},
        {"sy",   -1.0f, 1.0f},
    };
    return kParams;
}

// 每个整数控制器（万花镜对称）的原数据：UI 显示名 / 取值范围 [min_value, max_value]。
struct VisualOffsetIntParamMeta
{
    const char* display_name;  // UI 标签
    int         min_value;     // 最小值（1）
    int         max_value;     // 最大值
};

// 获取 3 个整数控制器的元数据表（顺序与 MilkdropVisualOffsetState::ivalue 一致）。
inline const VisualOffsetIntParamMeta* GetVisualOffsetIntParams()
{
    static const VisualOffsetIntParamMeta kIntParams[MilkdropVisualOffsetState::kIntParamCount] = {
        {"kaleido", 1, 16},  // 万花镜角度瓣数：围绕中心按极坐标镜像折叠，形成 N 重旋转对称
        {"fold x",  1, 16},  // 水平对称折叠次数：沿 X 方向切成 N 段并镜像
        {"fold y",  1, 16},  // 垂直对称折叠次数：沿 Y 方向切成 N 段并镜像
    };
    return kIntParams;
}