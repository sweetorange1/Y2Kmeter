#define MyAppName      "Y2Kmeter"
#define MyAppVersion   "1.5.0"
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

; -----------------------------------------------------------------------
; 关键：升级安装时自动关闭占用中的 Y2Kmeter.exe，避免"旧文件未被新文件覆盖"
;   · CloseApplications=force     安装/卸载开始前强制关闭匹配到的进程
;   · RestartApplications=no      安装完成后不自动重启（用户可自行在 [Run] 中勾选启动）
; 结合 [InstallDelete] 强制先删除旧 EXE，确保 1.1.x → 1.5.0 升级一定覆盖成功。
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
; VST3：删除旧 bundle 目录（若存在），用户选择的目录由 [Code] 段 GetVst3Dir 决定
Type: filesandordirs; Name: "{code:GetVst3Dir}\{#MyPluginBundle}"; Components: vst3

; -----------------------------------------------------------------------
; 文件：按组件分别复制
; -----------------------------------------------------------------------
[Files]
; Standalone EXE
Source: "cmake-build-release\Y2Kmeter_artefacts\Release\Standalone\{#MyAppExeName}"; \
    DestDir: "{app}"; \
    Flags: ignoreversion; \
    Components: standalone

; VST3（整个 .vst3 bundle 目录递归复制）
; DestDir 走 [Code] 段 GetVst3Dir —— 用户在独立向导页里选择的路径
Source: "cmake-build-release\Y2Kmeter_artefacts\Release\VST3\{#MyPluginBundle}\*"; \
    DestDir: "{code:GetVst3Dir}\{#MyPluginBundle}"; \
    Flags: ignoreversion recursesubdirs createallsubdirs; \
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
  Vst3DirPage:        TInputDirWizardPage;
  Vst3DirWarningShown: Boolean;

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

// 只有勾选了 vst3 组件才显示独立目录页；未勾选时直接跳过
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (Vst3DirPage <> nil) and (PageID = Vst3DirPage.ID) then
    Result := not WizardIsComponentSelected('vst3');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ChosenPath: string;
begin
  Result := True;

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