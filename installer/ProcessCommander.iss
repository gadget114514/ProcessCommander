; ProcessCommander installer (Inno Setup 6)
;
; Prerequisites:
;   1. Build the Release binaries and assemble the payload into
;      dist\ProcessCommander-v1.2.0-win64\ :
;        ProcessCommander.exe, pcder.exe, conpty.dll, OpenConsole.exe,
;        LICENSE, README.md, README-ja.md, docs\images\*
;   2. Compile with:  ISCC.exe installer\ProcessCommander.iss
;      or run installer\build_installer.cmd
;
; The install is per-user ({localappdata}\Programs) so the dashboard can write
; logs/state/task queues next to the executable without administrator rights.

#define MyAppName "ProcessCommander"
#define MyAppVersion "1.2.0"
#define MyAppPublisher "gadget114514"
#define MyAppURL "https://github.com/gadget114514/ProcessCommander"
#define MyAppExeName "ProcessCommander.exe"
#define PayloadDir "..\dist\ProcessCommander-v1.2.0-win64"

[Setup]
AppId={{fb224954-9950-4106-a925-41423c19c7cc}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion=1.2.0.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=ProcessCommander Setup
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile={#PayloadDir}\LICENSE
OutputDir=..\dist
OutputBaseFilename=ProcessCommander-v{#MyAppVersion}-win64-setup
SetupIconFile=..\images\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
CloseApplications=yes
RestartApplications=no
ChangesEnvironment=yes
SetupLogging=yes
AppMutex=ProcessCommander_GUI_Mutex_v1

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "addtopath"; Description: "Add pcder.exe to the user PATH (recommended for CLI / LLM agents)"; GroupDescription: "Integration:"; Flags: checkedonce
Name: "autostartgui"; Description: "Start the ProcessCommander dashboard at sign-in (tray)"; GroupDescription: "Integration:"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\ProcessCommander.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\pcder.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\conpty.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\OpenConsole.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\README-ja.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\README (English)"; Filename: "{app}\README.md"
Name: "{group}\README (日本語)"; Filename: "{app}\README-ja.md"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addtopath; Check: NeedsAddPath('{app}'); Flags: preservestringtype
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ProcessCommander"; ValueData: """{app}\{#MyAppExeName}"" gui"; Tasks: autostartgui; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"

[Code]
function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    Exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

procedure RemoveFromUserPath(Path: string);
var
  OrigPath: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', OrigPath) then Exit;
  P := Pos(';' + Uppercase(Path) + ';', ';' + Uppercase(OrigPath) + ';');
  if P = 0 then Exit;
  Delete(OrigPath, P, Length(Path) + 1);
  RegWriteExpandStringValue(HKCU, 'Environment', 'Path', OrigPath);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromUserPath(ExpandConstant('{app}'));
end;
