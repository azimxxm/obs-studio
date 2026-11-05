; Windows installer for Narrator Studio (64-bit only).
; Built by .github/workflows/narrator-windows.yaml from the CMake install folder.

#define AppName "Narrator Studio"
#define AppVersion GetEnv("NARRATOR_VERSION")
#define InstallRoot "..\..\build_x64\install"

[Setup]
AppId={{6F3B9D2E-5C1A-4E8B-9A7D-2B4C6E8F1A3D}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=azimxxm
AppPublisherURL=https://github.com/azimxxm/obs-studio
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=Narrator-Studio-{#AppVersion}-Windows-x64-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\bin\64bit\obs64.exe

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#InstallRoot}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
; OBS needs the Visual C++ runtime; installed quietly if it is missing
Source: "vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
; OBS looks for its data next to the exe, so it must start in bin\64bit
Name: "{group}\{#AppName}"; Filename: "{app}\bin\64bit\obs64.exe"; WorkingDir: "{app}\bin\64bit"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\bin\64bit\obs64.exe"; WorkingDir: "{app}\bin\64bit"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Visual C++ runtime..."; Flags: waituntilterminated
Filename: "{app}\bin\64bit\obs64.exe"; WorkingDir: "{app}\bin\64bit"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent
