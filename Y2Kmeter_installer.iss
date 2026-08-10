#define MyAppName      "Y2Kmeter"
#define MyAppVersion   "2.5.2"
#define MyAppPublisher "iisaacbeats.cn"
#define MyAppExeName   "Y2Kmeter.exe"
#define MyPluginBundle "Y2Kmeter.vst3"

; -----------------------------------------------------------------------
; 安装目录说明：
;   Standalone (EXE) → {app}                               ← 由 wpSelectDir 选择
;   VST3             → 由独立向导页 Vst3DirPage 选择，默认
;                        {commoncf}\VST3\iisaacbeats.cn
; 两个组件独立安装，用户可在"选择组件"页面分别勾选，默认全选。
; -----------------------------------------------------------------------

[Setup]
AppId={{C9B9FCA5-9D58-4F34-A856-214E5F972B25}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
; 主安装目录指向 EXE 组件的目标路径（VST3 组件有自己的目录页，不受此影响）
DefaultDirName={autopf64}\iisaacbeats.cn\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputDir=dist
OutputBaseFilename={#MyAppName}_Setup_{#MyAppVersion}_x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupLogging=yes
UsePreviousAppDir=no
DisableProgramGroupPage=yes
DisableDirPage=no
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=assets\icon.ico

; -----------------------------------------------------------------------
; 关键：升级安装时自动关闭占用中的 Y2Kmeter.exe，避免"旧文件未被新文件覆盖"
;   · CloseApplications=force     安装/卸载开始前强制关闭匹配到的进程
;   · RestartApplications=no      安装完成后不自动重启（用户可自行在 [Run] 中勾选启动）
; 结合 [InstallDelete] 强制先删除旧 EXE，确保旧版本 → 1.8.3 升级一定覆盖成功。
; -----------------------------------------------------------------------
CloseApplications=force
CloseApplicationsFilter=*.exe,*.dll
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

; -----------------------------------------------------------------------
; 组件定义：两个独立组件，默认全选
; -----------------------------------------------------------------------
[Components]
Name: "standalone"; Description: "Standalone Application (EXE)"; Types: full compact custom; Flags: fixed
Name: "vst3";       Description: "VST3 Plugin";                   Types: full custom

; -----------------------------------------------------------------------
; 安装前先删除旧文件，强制覆盖安装（解决"升级后仍是旧版本"问题）
; -----------------------------------------------------------------------
[InstallDelete]
; Standalone：删除旧 EXE（若存在）
Type: files; Name: "{app}\{#MyAppExeName}"; Components: standalone
; Standalone：删除旧 Tamagotchi 动画资源目录（若存在，v2.3.1 及之前为散装文件安装）
Type: filesandordirs; Name: "{app}\assets\Tamagotchi"; Components: standalone
; Standalone：删除旧 Tamagotchi 压缩包（升级清理）
Type: files; Name: "{app}\assets\tamagotchi_assets.zip"; Components: standalone
; Standalone：删除旧 Milkdrop 运行时（projectM DLL + glew32 + 预设/纹理目录/压缩包，确保升级时全量覆盖）
Type: files; Name: "{app}\projectM-4.dll"; Components: standalone
Type: files; Name: "{app}\glew32.dll"; Components: standalone
Type: filesandordirs; Name: "{app}\assets\milkdrop_presets"; Components: standalone
Type: filesandordirs; Name: "{app}\assets\milkdrop_textures"; Components: standalone
Type: files; Name: "{app}\assets\milkdrop_presets.zip"; Components: standalone
; AppData 共享目录：删除旧版散装文件（若存在，v2.3.1 及之前为直接安装），升级时统一改为 ZIP 解压
Type: filesandordirs; Name: "{userappdata}\Y2Kmeter\milkdrop_textures"; Components: standalone
Type: files; Name: "{userappdata}\Y2Kmeter\milkdrop_textures.zip"; Components: standalone
Type: files; Name: "{userappdata}\Y2Kmeter\milkdrop_presets.zip"; Components: standalone
; VST3：删除旧 bundle 目录（若存在），用户选择的目录由 [Code] 段 GetVst3Dir 决定
Type: filesandordirs; Name: "{code:GetVst3Dir}\{#MyPluginBundle}"; Components: vst3
; VST3：删除系统默认 VST3 路径下的旧版 Milkdrop 预设/纹理（迁移至 AppData 集中存储后不再随 VST3 bundle 分发）
Type: filesandordirs; Name: "{commoncf}\VST3\iisaacbeats.cn\{#MyPluginBundle}\Contents\x86_64-win\milkdrop_presets"; Components: vst3
Type: filesandordirs; Name: "{commoncf}\VST3\iisaacbeats.cn\{#MyPluginBundle}\Contents\x86_64-win\milkdrop_textures"; Components: vst3

; -----------------------------------------------------------------------
; 文件：按组件分别复制
; -----------------------------------------------------------------------
[Files]
; Standalone EXE
Source: "cmake-build-release-visual-studio\Y2Kmeter_artefacts\Release\Standalone\{#MyAppExeName}"; \
    DestDir: "{app}"; \
    Flags: ignoreversion; \
    Components: standalone

; Standalone Tamagotchi 动画资源（打包为 ZIP 以加速安装，2652 个 PNG 文件合并为 1 个）
;   · ZIP 预置于 assets/tamagotchi_assets.zip，更新动画资源后需重新手动打包
;   · 内部路径保留 Tamagotchi/ 前缀，安装后由 [Code] CurStepChanged(ssPostInstall) 解压到 {app}\assets\
Source: "assets\tamagotchi_assets.zip"; \
    DestDir: "{app}\assets"; \
    Flags: ignoreversion; \
    Components: standalone

; Standalone Milkdrop 运行时 DLL（projectM 渲染库 + OpenGL 扩展加载器）
Source: "cmake-build-release-visual-studio\Y2Kmeter_artefacts\Release\Standalone\projectM-4.dll"; \
    DestDir: "{app}"; \
    Flags: ignoreversion; \
    Components: standalone
Source: "cmake-build-release-visual-studio\Y2Kmeter_artefacts\Release\Standalone\glew32.dll"; \
    DestDir: "{app}"; \
    Flags: ignoreversion; \
    Components: standalone

; Standalone Milkdrop 预设（打包为单一 ZIP 以加速安装，9927 个 .milk 文件合并为 1 个）
;   · ZIP 预置于 assets/milkdrop_presets.zip，更新预设后需重新手动打包
;   · v2.3.1: 预设集中存放于 %APPDATA%\Y2Kmeter\，Standalone 和 VST3 共享同一份
;   · 安装后由 [Code] CurStepChanged(ssPostInstall) 自动解压
Source: "assets\milkdrop_presets.zip"; \
    DestDir: "{userappdata}\Y2Kmeter"; \
    Flags: ignoreversion; \
    Components: standalone

; Standalone Milkdrop 纹理（打包为 ZIP 以加速安装，66 个 jpg 文件合并为 1 个）
;   · ZIP 预置于 assets/milkdrop_textures.zip，更新纹理后需重新手动打包
;   · v2.3.1: 纹理集中存放于 %APPDATA%\Y2Kmeter\，Standalone 和 VST3 共享同一份
;   · 安装后由 [Code] CurStepChanged(ssPostInstall) 自动解压
Source: "assets\milkdrop_textures.zip"; \
    DestDir: "{userappdata}\Y2Kmeter"; \
    Flags: ignoreversion; \
    Components: standalone

; VST3（整个 .vst3 bundle 目录递归复制）
;   v2.3.1: 排除 milkdrop_presets/ 和 milkdrop_textures/，预设和纹理已集中存放于
;   %APPDATA%\Y2Kmeter\，Standalone 和 VST3 共享同一份，不再随 bundle 冗余分发。
; DestDir 走 [Code] 段 GetVst3Dir —— 用户在独立向导页里选择的路径
Source: "cmake-build-release-visual-studio\Y2Kmeter_artefacts\Release\VST3\{#MyPluginBundle}\*"; \
    DestDir: "{code:GetVst3Dir}\{#MyPluginBundle}"; \
    Flags: ignoreversion recursesubdirs createallsubdirs; \
    Excludes: "milkdrop_presets\*;milkdrop_textures\*"; \
    Components: vst3

; -----------------------------------------------------------------------
; 快捷方式：仅 Standalone 组件安装时创建
; -----------------------------------------------------------------------
[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Components: standalone
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\{#MyAppExeName}"; Components: standalone

; -----------------------------------------------------------------------
; 安装后启动：仅 Standalone 组件
; -----------------------------------------------------------------------
[Run]
; 安装后启动
Filename: "{app}\{#MyAppExeName}"; \
    Description: "启动 {#MyAppName}"; \
    Flags: nowait postinstall skipifsilent; \
    Components: standalone

; -----------------------------------------------------------------------
; 代码：
;   1) 创建 VST3 独立目录选择页（Vst3DirPage）
;   2) 只有勾选了 vst3 组件才显示该页
;   3) VST3 目录非默认路径时提示用户在 DAW 中手动添加扫描路径
;   4) GetVst3Dir() 返回用户选择的 VST3 路径（供 [Files]/[InstallDelete] 引用）
; -----------------------------------------------------------------------
[Code]
var
  Vst3DirPage:         TInputDirWizardPage;
  Vst3DirWarningShown: Boolean;
  TelemetryPage:       TWizardPage;
  TelemetryMemo:       TNewMemo;
  TelemetryCheckBox:   TNewCheckBox;
  TelemetryAgreed:     Boolean;

function DefaultVst3Dir: string;
begin
  Result := ExpandConstant('{commoncf}\VST3\iisaacbeats.cn');
end;

// 供 [Files] / [InstallDelete] 使用的常量函数：code:GetVst3Dir
function GetVst3Dir(Param: string): string;
begin
  if (Vst3DirPage <> nil) and (Vst3DirPage.Values[0] <> '') then
    Result := Vst3DirPage.Values[0]
  else
    Result := DefaultVst3Dir;
end;

procedure InitializeWizard;
begin
  // ============================================================
  // 隐私授权页：安装在选择目录后、选择组件前显示
  //   · 锚点 wpSelectDir：内置页顺序为
  //     Welcome → SelectDir → [本页] → SelectComponents → Ready
  //   · 使用标准用户协议页布局：上方可滚动文本区 + 下方同意勾选框
  //   · 用户必须勾选 CheckBox 方可继续（NextButtonClick 阻止）
  //   · 同意后会在 ssPostInstall 中写入注册表
  //     HKCU\Software\iisaacbeats\Y2Kmeter\TelemetryEnabled = 1
  //   · 不同意则注册表键不存在或为 0，软件默认不发送数据
  //   · 软件安装后不提供界面开关，用户只能通过重装/卸载改变此设置
  // ============================================================
  TelemetryPage := CreateCustomPage(
    wpSelectDir,
    'Anonymous Usage Statistics',
    'Help improve Y2Kmeter'
  );

  // 可滚动文本区域 —— 展示完整的隐私政策说明（中英双语）
  TelemetryMemo := TNewMemo.Create(WizardForm);
  TelemetryMemo.Parent := TelemetryPage.Surface;
  TelemetryMemo.Left := 0;
  TelemetryMemo.Top := 0;
  TelemetryMemo.Width := TelemetryPage.SurfaceWidth;
  TelemetryMemo.Height := TelemetryPage.SurfaceHeight - ScaleY(40);
  TelemetryMemo.ReadOnly := True;
  TelemetryMemo.ScrollBars := ssVertical;
  TelemetryMemo.Lines.Text :=
    'Y2Kmeter collects anonymous usage statistics to help us understand ' +
    'how the software is being used and to improve future versions.' + #13#10#13#10 +

    'The collected data includes:' + #13#10 +
    '  • Software version and build type' + #13#10 +
    '  • Operating system and CPU information' + #13#10 +
    '  • Host application name (for VST3 plugin mode)' + #13#10 +
    '  • Display count and primary screen resolution' + #13#10 +
    '  • System language/region and timezone offset' + #13#10#13#10 +

    'What we DO NOT collect:' + #13#10 +
    '  • User name, host name, IP address, MAC address' + #13#10 +
    '  • Audio file paths or content' + #13#10 +
    '  • Any personally identifiable information' + #13#10#13#10 +

    'This setting can only be changed by reinstalling the software.' + #13#10 +
    'There is no toggle to disable this after installation.' + #13#10#13#10 +

    '-------------------------------------------------------------' + #13#10#13#10 +

    'Y2Kmeter 收集匿名使用统计数据，以帮助我们了解软件的使用情况' +
    '并改进未来版本。' + #13#10#13#10 +

    '收集的数据包括：' + #13#10 +
    '  • 软件版本与构建类型' + #13#10 +
    '  • 操作系统与 CPU 信息' + #13#10 +
    '  • 宿主软件名称（VST3 插件模式）' + #13#10 +
    '  • 显示器数量与主屏分辨率' + #13#10 +
    '  • 系统语言/区域与时区偏移' + #13#10#13#10 +

    '明确不会收集：' + #13#10 +
    '  • 用户名、主机名、IP 地址、MAC 地址' + #13#10 +
    '  • 音频文件路径或内容' + #13#10 +
    '  • 任何个人身份信息' + #13#10#13#10 +

    '此设置仅可通过重新安装软件来更改。安装完成后不提供关闭选项。';

  // 同意勾选框 —— 位于页面底部，用户必须勾选才能继续
  TelemetryCheckBox := TNewCheckBox.Create(WizardForm);
  TelemetryCheckBox.Parent := TelemetryPage.Surface;
  TelemetryCheckBox.Left := 0;
  TelemetryCheckBox.Top := TelemetryPage.SurfaceHeight - ScaleY(24);
  TelemetryCheckBox.Width := TelemetryPage.SurfaceWidth;
  TelemetryCheckBox.Caption := 'I agree to send anonymous usage statistics (我同意发送匿名使用统计)';
  TelemetryCheckBox.Checked := False;

  // 新建一个"选择 VST3 安装目录"的向导页。
  //   · 锚点必须是 wpSelectComponents，不是 wpSelectDir —— 内置页顺序是
  //     SelectDir 先于 SelectComponents，所以要想"先让用户勾 VST3 组件，
  //     再决定 VST3 安装路径"，必须把该页插在 SelectComponents 之后。
  //   · 未勾选 VST3 时 ShouldSkipPage 会直接跳过此页。
  Vst3DirPage := CreateInputDirPage(
    wpSelectComponents,
    'Select VST3 Install Location',
    'Where should the VST3 plug-in be installed?',
    'The VST3 plug-in will be installed into the following folder.' + #13#10 +
    'To continue, click Next. If you would like to select a different folder, click Browse.' + #13#10#13#10 +
    'Most DAWs automatically scan the default VST3 folder:' + #13#10 +
    '    ' + DefaultVst3Dir,
    False,   // AppendDir = False —— 不在用户选择的路径后追加 AppName
    ''       // NewFolderName —— 为空，不自动创建子目录
  );
  Vst3DirPage.Add('');
  Vst3DirPage.Values[0] := DefaultVst3Dir;
end;

// 只有勾选了 vst3 组件才显示独立目录页；未勾选时直接跳过。
// 隐私授权页始终显示（不可跳过）。
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (Vst3DirPage <> nil) and (PageID = Vst3DirPage.ID) then
    Result := not WizardIsComponentSelected('vst3');
  // TelemetryPage 永不跳过
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ChosenPath: string;
begin
  Result := True;

  // 隐私授权页：必须勾选 CheckBox 才能继续安装
  if (TelemetryPage <> nil) and (CurPageID = TelemetryPage.ID) then
  begin
    if not TelemetryCheckBox.Checked then
    begin
      MsgBox(
        'You must agree to send anonymous usage statistics to continue installation.' + #13#10#13#10 +
        '您必须同意发送匿名使用统计才能继续安装。',
        mbError, MB_OK);
      Result := False;
      Exit;
    end;
    TelemetryAgreed := True;
  end;

  // VST3 目录页：Next 时校验一下，若非默认路径则给出一次性提示
  if (Vst3DirPage <> nil) and (CurPageID = Vst3DirPage.ID) then
  begin
    ChosenPath := Vst3DirPage.Values[0];

    if (CompareText(
          RemoveBackslashUnlessRoot(ChosenPath),
          RemoveBackslashUnlessRoot(DefaultVst3Dir)
        ) <> 0) and (not Vst3DirWarningShown) then
    begin
      MsgBox(
        'You have selected a non-default VST3 folder:' + #13#10 +
        ChosenPath + #13#10#13#10 +
        'After installation, you may need to manually add this folder to your DAW''s VST3 plug-in paths and rescan, otherwise the plug-in might not be detected.',
        mbInformation, MB_OK);
      Vst3DirWarningShown := True;
    end;
  end;
end;

// ============================================================
// 通用辅助：解压 ZIP 到目标目录
//   · 优先使用 Windows 10 内置的 tar.exe（原生 C 工具，极快，自 build 17063 起所有 Win10+ 自带）
//   · tar.exe 不可用时回退到 PowerShell Expand-Archive
//   · 返回 True 表示解压成功
// ============================================================
function ExtractZip(const ZipPath, DestPath: String): Boolean;
var
  Cmd, TarExe: String;
  ResultCode: Integer;
begin
  Result := False;
  if not FileExists(ZipPath) then
  begin
    Log('WARNING: ZIP not found: ' + ZipPath);
    Exit;
  end;

  WizardForm.StatusLabel.Caption := 'Extracting ' + ExtractFileName(ZipPath) + ' ...';
  WizardForm.ProgressGauge.Style := npbstMarquee;

  // 确保目标目录存在
  if not DirExists(DestPath) then
    CreateDir(DestPath);

  // 策略 1：tar.exe（Windows 10+ 内置，原生速度最快，9927 文件仅需数秒）
  //   路径：C:\Windows\System32\tar.exe（所有 Win10 build 17063+ 自带）
  TarExe := ExpandConstant('{sys}\tar.exe');
  if FileExists(TarExe) then
  begin
    Cmd := '-xf "' + ZipPath + '" -C "' + DestPath + '"';
    if Exec(TarExe, Cmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    begin
      if ResultCode = 0 then
      begin
        Log('Extracted (tar): ' + ZipPath + ' -> ' + DestPath);
        DeleteFile(ZipPath);
        WizardForm.ProgressGauge.Style := npbstNormal;
        Result := True;
        Exit;
      end
      else
        Log('WARNING: tar.exe exited with code ' + IntToStr(ResultCode) + ', falling back to PowerShell');
    end
    else
      Log('WARNING: Failed to launch tar.exe, falling back to PowerShell');
  end
  else
    Log('tar.exe not found, falling back to PowerShell');

  // 策略 2：PowerShell Expand-Archive（兼容旧版 Windows / 非标环境）
  Cmd := '-NoProfile -Command "Expand-Archive -LiteralPath ''' + ZipPath + ''' -DestinationPath ''' + DestPath + ''' -Force"';
  if Exec('powershell.exe', Cmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    if ResultCode = 0 then
    begin
      Log('Extracted (PS): ' + ZipPath + ' -> ' + DestPath);
      DeleteFile(ZipPath);
      Result := True;
    end
    else
      Log('WARNING: PowerShell Expand-Archive exited with code ' + IntToStr(ResultCode) + ' for ' + ZipPath);
  end
  else
    Log('ERROR: Failed to launch PowerShell for ZIP extraction: ' + ZipPath);

  WizardForm.ProgressGauge.Style := npbstNormal;
  WizardForm.StatusLabel.Caption := 'Finishing installation...';
end;

// ============================================================
// ssPostInstall：文件复制完成后、[Run] 执行前，自动解压三个 ZIP
//   · milkdrop_presets.zip   → %APPDATA%\Y2Kmeter\milkdrop_presets
//   · milkdrop_textures.zip  → %APPDATA%\Y2Kmeter\milkdrop_textures
//   · tamagotchi_assets.zip  → {app}\assets\ (内部含 Tamagotchi/ 前缀)
//   · 注意：[Run] 中的 shell 命令在不可见窗口下可能静默失败；
//     CurStepChanged(ssPostInstall) 由 Inno Setup 内部事件驱动，
//     不依赖窗口消息循环，执行时序更可靠。
// ============================================================
procedure CurStepChanged(CurStep: TSetupStep);
var
  ZipPath, DestPath: String;
  TelemetryValue: Cardinal;
begin
  if CurStep = ssPostInstall then
  begin
    // ---------- 遥测授权：写入注册表 ----------
    // 只有用户在隐私授权页面勾选了"同意"才写入 TelemetryEnabled=1；
    // 否则不写入（或写入 0），客户端默认视为未授权。
    if TelemetryAgreed then
      TelemetryValue := 1
    else
      TelemetryValue := 0;

    RegWriteDWordValue(
      HKEY_CURRENT_USER,
      'Software\iisaacbeats\Y2Kmeter',
      'TelemetryEnabled',
      TelemetryValue);

    if IsComponentSelected('standalone') then
    begin
      // 1) Milkdrop 预设（9927 个 .milk 文件）
      ZipPath  := ExpandConstant('{userappdata}') + '\Y2Kmeter\milkdrop_presets.zip';
      DestPath := ExpandConstant('{userappdata}') + '\Y2Kmeter\milkdrop_presets';
      ExtractZip(ZipPath, DestPath);

      // 2) Milkdrop 纹理（66 个 jpg 文件）
      ZipPath  := ExpandConstant('{userappdata}') + '\Y2Kmeter\milkdrop_textures.zip';
      DestPath := ExpandConstant('{userappdata}') + '\Y2Kmeter\milkdrop_textures';
      ExtractZip(ZipPath, DestPath);

      // 3) Tamagotchi 动画资源（2652 个 PNG 文件）
      ZipPath  := ExpandConstant('{app}') + '\assets\tamagotchi_assets.zip';
      DestPath := ExpandConstant('{app}') + '\assets';
      ExtractZip(ZipPath, DestPath);
    end;
  end;
end;