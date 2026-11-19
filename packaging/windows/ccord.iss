; SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
; Inno Setup installer definition for C-Cord on Windows (x86_64).
; Registers the ccord:// URI scheme for one-click plugin installation from c-cord.org.

#ifndef CCordVersion
  #define CCordVersion "0.1.0"
#endif
#ifndef CCordDistDir
  #define CCordDistDir "dist-win"
#endif
#ifndef CCordOutDir
  #define CCordOutDir "out-win"
#endif
#ifndef CCordIcon
  #define CCordIcon "..\..\assets\windows\ccord.ico"
#endif

[Setup]
AppId={{8E94A8D3-2F49-4D9A-9D8C-CC0RD0000001}
AppName=C-Cord
AppVersion={#CCordVersion}
AppPublisher=C-Cord Contributors
AppPublisherURL=https://c-cord.org
AppSupportURL=https://github.com/C-Cord/C-Cord-Client/issues
AppUpdatesURL=https://c-cord.org
DefaultDirName={autopf}\C-Cord
DefaultGroupName=C-Cord
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
OutputDir={#CCordOutDir}
OutputBaseFilename=C-Cord-{#CCordVersion}-windows-x86_64-setup
CloseApplications=yes
RestartApplications=yes
UninstallDisplayIcon={app}\ccord.exe
#ifexist CCordIcon
SetupIconFile={#CCordIcon}
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#CCordDistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\C-Cord"; Filename: "{app}\ccord.exe"
Name: "{group}\Uninstall C-Cord"; Filename: "{uninstallexe}"
Name: "{autodesktop}\C-Cord"; Filename: "{app}\ccord.exe"; Tasks: desktopicon

[Registry]
; Register ccord:// deep-link handler for one-click plugin installs (works per-user without UAC).
Root: HKCU; Subkey: "Software\Classes\ccord"; ValueType: string; ValueName: ""; ValueData: "URL:C-Cord Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\ccord"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\ccord\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\ccord.exe"",0"
Root: HKCU; Subkey: "Software\Classes\ccord\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\ccord.exe"" --open-url ""%1"""

[Run]
Filename: "{app}\ccord.exe"; Description: "{cm:LaunchProgram,C-Cord}"; Flags: nowait postinstall skipifsilent
