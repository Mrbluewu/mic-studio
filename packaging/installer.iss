; Mic Studio 安装包脚本（Inno Setup 6）
; 编译：ISCC.exe packaging\installer.iss

#define AppName "Mic Studio"
#define AppVersion "0.1.13"
#define AppPublisher "Mic Studio"

[Setup]
AppId={{B0F4A3C2-7D51-4E8A-9C33-2A6B7F1E5D10}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\Mic Studio
DefaultGroupName={#AppName}
; 显示「选择安装位置」页，允许用户自定义目录
DisableDirPage=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=MicStudio-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\assets\icons\mic-studio.ico
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 按用户安装，无需管理员权限
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\micstudio.exe

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"

[Files]
Source: "..\build\apps\micstudio\Release\micstudio.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\apps\engine-cli\Release\engine-cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\apps\micstudio\Release\DeepFilterNet.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\micstudio.exe"
Name: "{group}\卸载 {#AppName}"; Filename: "{uninstallexe}"
; desktopicon 任务勾选时才创建桌面快捷方式
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\micstudio.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\micstudio.exe"; Description: "立即启动 {#AppName}"; Flags: nowait postinstall skipifsilent
