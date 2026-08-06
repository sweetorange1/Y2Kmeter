#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_forbidden_patterns.py
============================

静态扫描 Y2Kmeter 项目里 milkdrop3 模块相关代码是否命中 milkdrop3-dev-guard
Skill §4.3 禁止事项清单。命中任何一条 → 输出违规并 exit(1)。

用法（从项目根 I:/Y2KMeter/ 执行，注意：本机须用 py -3 而非 python）::

    py -3 docs/skills/milkdrop3-dev-guard/scripts/check_forbidden_patterns.py

判定规则（每条对应 forbidden-list.md 的 F 编号）::

  F2  右键处理             ： Milkdrop3Module.* 中 isRightButtonDown / WM_RBUTTONDOWN
  F3  转发消息             ： Milkdrop3Module.cpp 中 PostMessage/SendMessage 到 GetParent
  F5  调试痕迹             ： MD3_BUILD_TAG / Md3BuildTagLogger / MonitorFromPoint /
                              GetDpiForMonitor / GetDpiForWindow / SetThreadDpiAwarenessContext
  F6  已删死接口复活       ： Api::Initialize / CreateRenderWindow / SetPresetDir /
                              render_scale_ / CycleRenderScale / hub_retained_
  F7  死锁陷阱             ： ModuleWorkspace 中 factory(milkdrop3) / new Milkdrop3Module
  F11 JUCE 窗口框架触碰    ： third_party/JUCE 目录是否有未提交改动
  F12 ModulePanel private  ： Milkdrop3Module.* 直接访问 closeButtonPressed/closeButtonHovered
  F13 SetWindowPos 语义反 ： Milkdrop3Module.cpp 中 SetWindowPos 用 d3d_child_hwnd_ 作
                              hWndInsertAfter 参数
  F14 EDIT + WM_CHAR       ： Milkdrop3Module.cpp 中 EditSubclassProc 内在 WM_CHAR 分支
                              判断 VK_RETURN

设计说明：零依赖（纯正则），可作 pre-commit hook 或 CI 一步。
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

MODULE_FILES = [
    Path("source/ui/modules/Milkdrop3Module.cpp"),
    Path("source/ui/modules/Milkdrop3Module.h"),
    Path("source/ui/modules/Milkdrop3Api.cpp"),
    Path("source/ui/modules/Milkdrop3Api.h"),
    Path("source/ui/modules/Md3DebugLog.cpp"),
    Path("source/ui/modules/Md3DebugLog.h"),
]

WORKSPACE_FILE = Path("source/ui/ModuleWorkspace.cpp")


def scan(path: Path, pattern: str) -> list[tuple[int, str]]:
    """返回 [(1-based line, line_content), ...]。"""
    if not path.exists():
        return []
    rx = re.compile(pattern)
    hits = []
    for i, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines()):
        if rx.search(line):
            hits.append((i + 1, line.rstrip()))
    return hits


def stripped(line: str) -> str:
    """去掉行首行尾空白与结尾注释，方便匹配。"""
    s = line.strip()
    # 简单去掉行尾 // 注释
    idx = s.find("//")
    if idx >= 0:
        s = s[:idx].rstrip()
    return s


def main() -> int:
    violations: list[str] = []

    # ---- F2 右键处理 ----
    for f in MODULE_FILES:
        for ln, content in scan(f, r"isRightButtonDown|WM_RBUTTONDOWN|WM_RBUTTONUP|WM_CONTEXTMENU"):
            # 忽略纯注释行
            if stripped(content).startswith(("//", "/*", "*")):
                continue
            violations.append(
                f"[F2] {f}:{ln} 命中右键相关代码：{content.strip()}"
            )

    # ---- F3 转发消息到父窗口 ----
    for f in MODULE_FILES:
        for ln, content in scan(
            f, r"(PostMessage|SendMessage[AW]?)\s*\(\s*GetParent\s*\("
        ):
            if stripped(content).startswith(("//", "/*", "*")):
                continue
            violations.append(
                f"[F3] {f}:{ln} 命中向 JUCE 父窗口转发消息：{content.strip()}"
            )

    # ---- F5 调试痕迹 ----
    debug_pat = (
        r"MD3_BUILD_TAG"
        r"|Md3BuildTagLogger"
        r"|MonitorFromPoint\s*\("
        r"|GetDpiForMonitor\s*\("
        r"|GetDpiForWindow\s*\("
        r"|SetThreadDpiAwarenessContext\s*\("
    )
    for f in MODULE_FILES + [Path("source/standalone/Y2KStandaloneApp.cpp")]:
        for ln, content in scan(f, debug_pat):
            if stripped(content).startswith(("//", "/*", "*")):
                continue
            violations.append(
                f"[F5] {f}:{ln} 命中调试痕迹：{content.strip()}"
            )

    # ---- F6 已删死接口复活 ----
    dead_pat = (
        r"\bApi::Initialize\s*\("
        r"|\bCreateRenderWindow\s*\("
        r"|\bSetPresetDir\s*\("
        r"|\brender_scale_\b"
        r"|\bCycleRenderScale\s*\("
        r"|\bApplyRenderScale\s*\("
        r"|\bhub_retained_\b"
    )
    for f in MODULE_FILES:
        for ln, content in scan(f, dead_pat):
            if stripped(content).startswith(("//", "/*", "*")):
                continue
            violations.append(
                f"[F6] {f}:{ln} 命中已删接口/字段：{content.strip()}"
            )

    # ---- F7 ModuleWorkspace 中 factory(milkdrop3) / new Milkdrop3Module ----
    if WORKSPACE_FILE.exists():
        text = WORKSPACE_FILE.read_text(encoding="utf-8", errors="replace")
        for i, line in enumerate(text.splitlines(), start=1):
            s = stripped(line)
            if s.startswith(("//", "/*", "*")):
                continue
            if re.search(r"factory\s*\(\s*(ModuleType\s*::\s*)?milkdrop3\s*\)", line) or \
               re.search(r"new\s+Milkdrop3Module\b", line):
                # 允许 CreateModule 工厂本身；但禁止在 getDefaultSizeForType /
                # getHoverPreviewImage 里出现。这里做粗略窗口判断：前后 40 行看是否命中
                # getDefaultSizeForType / getHoverPreviewImage。
                lines = text.splitlines()
                start = max(0, i - 40)
                end = min(len(lines), i + 5)
                ctx = "\n".join(lines[start:end])
                if re.search(
                    r"getDefaultSizeForType|getHoverPreviewImage",
                    ctx,
                ):
                    violations.append(
                        f"[F7] {WORKSPACE_FILE}:{i} 在 getDefaultSizeForType/"
                        f"getHoverPreviewImage 中构造 milkdrop3 实例：{line.strip()}"
                    )

    # ---- F11 third_party/JUCE 被改动 ----
    try:
        r = subprocess.run(
            ["git", "status", "--short", "third_party/JUCE"],
            capture_output=True, text=True, timeout=10,
        )
        if r.returncode == 0:
            changed = [ln for ln in r.stdout.splitlines() if ln.strip()]
            if changed:
                violations.append(
                    "[F11] third_party/JUCE 目录存在未提交/未暂存改动：\n"
                    + "\n".join("      " + x for x in changed)
                    + "\n      milkdrop3 的任何修复都不得触碰 JUCE 原生窗口代码；"
                    "  请 `git checkout -- third_party/JUCE` 完整回滚。"
                )
    except Exception as e:
        # git 不可用不视为违规，仅提示
        print(f"[warn] 无法调用 git 检查 third_party/JUCE：{e}", file=sys.stderr)

    # ---- F12 直接访问 ModulePanel private 成员 ----
    for f in [Path("source/ui/modules/Milkdrop3Module.cpp"),
              Path("source/ui/modules/Milkdrop3Module.h")]:
        for ln, content in scan(f, r"\bcloseButton(Pressed|Hovered)\b"):
            s = stripped(content)
            if s.startswith(("//", "/*", "*")):
                continue
            violations.append(
                f"[F12] {f}:{ln} 直接访问 ModulePanel 的 private 成员："
                f"{content.strip()}\n       "
                f"这两个成员在基类是 private，子类不可引用；"
                f"建议保留基类默认 paint 或让子类自行维护 hover/press 状态。"
            )

    # ---- F13 SetWindowPos 用 d3d_child_hwnd_ 作 hWndInsertAfter ----
    for f in [Path("source/ui/modules/Milkdrop3Module.cpp")]:
        text = f.read_text(encoding="utf-8", errors="replace") if f.exists() else ""
        # 匹配 SetWindowPos( ..., d3d_child_hwnd_, ... ) —— 允许换行；
        # 用简单启发：SetWindowPos( ... , 后立刻 d3d_child_hwnd_（可能跨行）
        rx = re.compile(
            r"SetWindowPos\s*\(\s*[^,]+,\s*d3d_child_hwnd_",
            re.DOTALL,
        )
        for m in rx.finditer(text):
            # 求所在行号
            ln = text[: m.start()].count("\n") + 1
            snippet = text[m.start(): m.start() + 80].replace("\n", " ")
            violations.append(
                f"[F13] {f}:{ln} SetWindowPos 用 d3d_child_hwnd_ 作 "
                f"hWndInsertAfter：{snippet}...\n       "
                f"Win32 语义会把 overlay 塞到 D3D popup 之下；应用 HWND_TOP。"
            )

    # ---- F14 EDIT + WM_CHAR + VK_RETURN ----
    edit_file = Path("source/ui/modules/Milkdrop3Module.cpp")
    if edit_file.exists():
        text = edit_file.read_text(encoding="utf-8", errors="replace")
        # 找到 EditSubclassProc 的粗略范围（若存在），检查其内是否有 WM_CHAR 分支中
        # 出现 VK_RETURN
        proc_start = text.find("EditSubclassProc")
        if proc_start >= 0:
            # 取 proc_start 之后至下一个 "^\}\s*$" 之间的 3000 字符窗口，粗略检查
            window = text[proc_start: proc_start + 4000]
            # 判定：window 里同时出现 WM_CHAR 与 VK_RETURN 且 VK_RETURN 距 WM_CHAR 较近
            for m in re.finditer(r"case\s+WM_CHAR\s*:", window):
                start = m.end()
                # 在该 case 之后到下一个 "case " 之间检查是否有 VK_RETURN
                nxt = re.search(r"case\s+WM_[A-Z_]+\s*:|return\s+DefSubclassProc",
                                window[start:])
                block_end = start + (nxt.start() if nxt else 400)
                block = window[start:block_end]
                if "VK_RETURN" in block:
                    ln = text[: proc_start + start].count("\n") + 1
                    violations.append(
                        f"[F14] {edit_file}:{ln} 在 EditSubclassProc 的 WM_CHAR 分支"
                        f"里检测 VK_RETURN；单行 EDIT 中此路径不可靠，"
                        f"应搬到 WM_KEYDOWN。"
                    )

    # ---- 输出 ----
    if violations:
        print("[FAIL] milkdrop3-dev-guard 禁止事项自检发现违规：", file=sys.stderr)
        for v in violations:
            print(f"  - {v}", file=sys.stderr)
        print(
            "\n参考修复：docs/skills/milkdrop3-dev-guard/references/forbidden-list.md",
            file=sys.stderr,
        )
        return 1

    print("[OK] milkdrop3 相关代码未命中禁止事项。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
