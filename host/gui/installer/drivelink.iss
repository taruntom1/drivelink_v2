; DriveLink Windows installer (Inno Setup 6).
;
; Built by tools/package_win.py, which stages the release build and runs
;   ISCC /DAppVersion=<full version> /DStageDir=<stage> /DRepoRoot=<repo> /O<out> drivelink.iss
; Per-user by default; the wizard offers an all-users install. No PATH change
; and no registry entries other than Inno's own uninstall entry.

#ifndef AppVersion
  #error AppVersion is not defined; build with tools/package_win.py
#endif
#ifndef StageDir
  #error StageDir is not defined; build with tools/package_win.py
#endif
#ifndef RepoRoot
  #error RepoRoot is not defined; build with tools/package_win.py
#endif

[Setup]
; The product's permanent id: never change it, or upgrades install side by side.
AppId={{205D84A2-43DA-4D1C-A6C1-849C055E0090}
AppName=DriveLink
AppVersion={#AppVersion}
AppPublisher=Tarun Tom
DefaultDirName={autopf}\DriveLink
DefaultGroupName=DriveLink
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=DriveLink-{#AppVersion}-setup
LicenseFile={#RepoRoot}\LICENSE
InfoBeforeFile={#RepoRoot}\host\gui\notices\THIRD_PARTY_NOTICES.txt
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\drivelink-gui.exe
; No SetupIconFile: the project has no application icon yet (Inno's default is used).

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\DriveLink"; Filename: "{app}\drivelink-gui.exe"
Name: "{autodesktop}\DriveLink"; Filename: "{app}\drivelink-gui.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\drivelink-gui.exe"; Description: "{cm:LaunchProgram,DriveLink}"; Flags: nowait postinstall skipifsilent
