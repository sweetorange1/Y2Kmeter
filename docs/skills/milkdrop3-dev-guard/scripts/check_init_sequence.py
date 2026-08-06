#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_init_sequence.py
======================

静态扫描 Y2Kmeter 项目的 Standalone 启动序列，命中反模式返回非零退出码。

用法（从项目根 I:/Y2KMeter/ 执行，注意：本机须用 py -3 而非 python）::

    py -3 docs/skills/milkdrop3-dev-guard/scripts/check_init_sequence.py

判定规则（对应 milkdrop3-dev-guard Skill 铁律 6）::

  1. 存在 pluginHolder = std::make_unique<... 的行；
  2. 存在 mainWindow  = std::make_unique<... 的行；
  3. pluginHolder 行号必须早于 mainWindow 行号；
  4. `SetThreadDpiAwarenessContext(` 严禁出现（应用级只用 SetProcessDpiAwarenessContext）；
  5. `TimerThreadBoot` / `SharedResourcePointer<TimerThread>` 严禁出现；
  6. `mainWindow->setVisible(false)` 严禁出现（会造成 setVisible 分裂显示）；
  7. `mainWindow->addToDesktop()` 与 `mainWindow->setVisible(true)` 必须相邻（行差 ≤ 3）
     且行号大于 mainWindow 构造行号；
  8. `SetProcessDpiAwarenessContext(` 若出现，其位置应位于 initialise() 内、
     pluginHolder 构造之前（我们仅做一次弱校验：位于文件前 1/3 处）。

任何一条不满足 → 输出违规详情并 exit(1)；全部通过 → exit(0)。

设计说明：脚本刻意保持零依赖（纯正则 + 行号比较），可作为 pre-commit hook 或 CI 一步。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

TARGET = Path("source/standalone/Y2KStandaloneApp.cpp")


def find_lines(text: str, pattern: str) -> list[int]:
    """返回 1-based 行号列表。"""
    rx = re.compile(pattern)
    return [i + 1 for i, line in enumerate(text.splitlines()) if rx.search(line)]


def main() -> int:
    if not TARGET.exists():
        print(f"[ERR] 未找到 {TARGET}，请从项目根目录执行本脚本。", file=sys.stderr)
        return 2

    text = TARGET.read_text(encoding="utf-8", errors="replace")

    violations: list[str] = []

    plugin_lines = find_lines(text, r"pluginHolder\s*=\s*std::make_unique")
    window_lines = find_lines(text, r"mainWindow\s*=\s*std::make_unique")
    if not plugin_lines:
        violations.append("找不到 `pluginHolder = std::make_unique<...>` 构造语句。")
    if not window_lines:
        violations.append("找不到 `mainWindow = std::make_unique<...>` 构造语句。")

    # 3) pluginHolder 必须先于 mainWindow
    if plugin_lines and window_lines:
        first_plugin = plugin_lines[0]
        first_window = window_lines[0]
        if first_plugin >= first_window:
            violations.append(
                f"启动序列错乱：pluginHolder 构造在行 {first_plugin}，"
                f"但 mainWindow 构造在行 {first_window}（应更晚）。"
                f"违反铁律 6，会导致 LdrLockLoaderLock 死锁。"
            )

    # 4) SetThreadDpiAwarenessContext 严禁
    thread_dpi = find_lines(text, r"SetThreadDpiAwarenessContext\s*\(")
    if thread_dpi:
        violations.append(
            f"命中禁用 API `SetThreadDpiAwarenessContext`（行 {thread_dpi}）。"
            f"应用级只应使用 `SetProcessDpiAwarenessContext`。"
        )

    # 5) TimerThreadBoot / SharedResourcePointer<TimerThread> 严禁
    boot = find_lines(text, r"TimerThreadBoot|SharedResourcePointer\s*<\s*TimerThread\s*>")
    if boot:
        violations.append(
            f"命中禁用 workaround `TimerThreadBoot / SharedResourcePointer<TimerThread>`"
            f"（行 {boot}）。这类预热无法根治 LoaderLock 竞争。"
        )

    # 6) mainWindow->setVisible(false) 严禁（在 initialise 内）
    hide_calls = find_lines(text, r"mainWindow\s*->\s*setVisible\s*\(\s*false")
    # 允许 shutdown() 里出现；只在 initialise 定义体内检测
    if hide_calls:
        # 找 initialise 起止行大致范围
        init_start_matches = find_lines(text, r"void\s+initialise\s*\(")
        # shutdown 起止
        shutdown_start_matches = find_lines(text, r"void\s+shutdown\s*\(")
        if init_start_matches:
            init_start = init_start_matches[0]
            init_end = shutdown_start_matches[0] if shutdown_start_matches else 10**9
            bad = [ln for ln in hide_calls if init_start <= ln < init_end]
            if bad:
                violations.append(
                    f"在 initialise() 内命中 `mainWindow->setVisible(false)`（行 {bad}）。"
                    f"会导致 setVisible 分裂显示 → toFront 二次触发 → LdrLockLoaderLock 死锁。"
                )

    # 7) addToDesktop / setVisible(true) 相邻
    add_calls = find_lines(text, r"mainWindow\s*->\s*addToDesktop\s*\(")
    show_calls = find_lines(text, r"mainWindow\s*->\s*setVisible\s*\(\s*true")
    if add_calls and show_calls:
        best_pair_gap: int | None = None
        for a in add_calls:
            for s in show_calls:
                if s >= a:
                    gap = s - a
                    if best_pair_gap is None or gap < best_pair_gap:
                        best_pair_gap = gap
        if best_pair_gap is None or best_pair_gap > 3:
            violations.append(
                f"`mainWindow->addToDesktop()` 与 `mainWindow->setVisible(true)` 未紧邻"
                f"（最佳行差 {best_pair_gap}）。两者必须相邻（≤3 行）且 setVisible 更晚。"
            )
        # 且必须晚于 mainWindow = ... 构造
        if window_lines:
            first_window = window_lines[0]
            if add_calls and add_calls[0] < first_window:
                violations.append(
                    f"`mainWindow->addToDesktop()` 出现在 mainWindow 构造之前"
                    f"（addToDesktop 行 {add_calls[0]} < mainWindow 构造行 {first_window}）。"
                )

    # 8) SetProcessDpiAwarenessContext 位置弱校验（可选：只提示）
    dpi_calls = find_lines(text, r"::?SetProcessDpiAwarenessContext\s*\(")
    if dpi_calls and plugin_lines:
        if dpi_calls[-1] > plugin_lines[0]:
            violations.append(
                f"`SetProcessDpiAwarenessContext` 应在 `pluginHolder = std::make_unique`"
                f"之前调用（当前 DPI 调用行 {dpi_calls}, pluginHolder 行 {plugin_lines[0]}）。"
            )

    # 输出结果
    if violations:
        print("[FAIL] Y2KStandaloneApp 启动序列违反 milkdrop3-dev-guard 铁律 6：", file=sys.stderr)
        for v in violations:
            print(f"  - {v}", file=sys.stderr)
        print(
            "\n参考修复：docs/skills/milkdrop3-dev-guard/references/init-sequence.md",
            file=sys.stderr,
        )
        return 1

    print("[OK] Y2KStandaloneApp::initialise 启动序列符合铁律 6。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
