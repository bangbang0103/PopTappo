#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceExe
  #define SourceExe "..\dist\PopTappo.exe"
#endif
#ifndef OutputDir
  #define OutputDir "..\release"
#endif
#ifndef OutputBaseFilename
  #define OutputBaseFilename "PopTappo-" + AppVersion + "-Windows-x64-Setup"
#endif

[Setup]
AppId={{8E3C6C6D-5C7E-4D62-9A2B-4E1F2A8C7D10}
AppName=PopTappo
AppVersion={#AppVersion}
AppVerName=PopTappo {#AppVersion}
AppPublisher=bangbang0103
AppPublisherURL=https://github.com/bangbang0103/PopTappo
AppSupportURL=https://github.com/bangbang0103/PopTappo/issues
AppUpdatesURL=https://github.com/bangbang0103/PopTappo/releases
DefaultDirName={autopf}\PopTappo
DefaultGroupName=PopTappo
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupIconFile=..\src\app.ico
UninstallDisplayIcon={app}\PopTappo.exe
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; DestName: "PopTappo.exe"; Flags: ignoreversion

[Icons]
Name: "{group}\PopTappo"; Filename: "{app}\PopTappo.exe"
Name: "{group}\{cm:UninstallProgram,PopTappo}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\PopTappo"; Filename: "{app}\PopTappo.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PopTappo.exe"; Description: "{cm:LaunchProgram,PopTappo}"; Flags: nowait postinstall skipifsilent
