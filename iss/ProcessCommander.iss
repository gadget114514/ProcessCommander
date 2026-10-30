; ProcessCommander installer script (Inno Setup 6/7)
; Compile: ISCC.exe iss\ProcessCommander.iss  (or run iss\build.ps1)

#define MyAppName "ProcessCommander"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "gadget114514"
#define MyAppExeName "ProcessCommander.exe"
#define MyAppId "{{3F264840-A30F-41D1-9644-99EF1A2477DC}"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/gadget114514/ProcessCommander
AppSupportURL=https://github.com/gadget114514/ProcessCommander
AppUpdatesURL=https://github.com/gadget114514/ProcessCommander
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=ProcessCommander-v{#MyAppVersion}-win64-setup
SetupIconFile=..\images\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
ChangesEnvironment=yes
MinVersion=10.0.17763

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[CustomMessages]
english.AdditionalTasks=Additional tasks:
japanese.AdditionalTasks=追加タスク:
english.AddToPath=Add {#MyAppName} to the system PATH
japanese.AddToPath={#MyAppName} をシステム PATH に追加
english.LaunchApp=Launch {#MyAppName}
japanese.LaunchApp={#MyAppName} を起動

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "addtopath"; Description: "{cm:AddToPath}"; GroupDescription: "{cm:AdditionalTasks}"

[Files]
Source: "..\build\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\pcder.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\conpty.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\OpenConsole.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "redist\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "redist\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "redist\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README-ja.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\doc\pros-cons-comparison.md"; DestDir: "{app}\doc"; Flags: ignoreversion
Source: "..\docs\images\*"; DestDir: "{app}\docs\images"; Flags: ignoreversion recursesubdirs createallsubdirs
; tools\ww: optional task-card runner template (copied into projects by an LLM agent; see tools\ww\README-ww.md)
Source: "..\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
Name: "{app}\logs"; Permissions: users-modify

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

[Registry]
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addtopath; Flags: preservestringtype; Check: NeedsAddPath('{app}')

[Code]
const
  SMTO_ABORTIFHUNG = $0002;
  WM_SETTINGCHANGE = $001A;

function SendMessageTimeout(hWnd: HWND; Msg: UINT; wParam: Longint; lParam: string;
  fuFlags: UINT; uTimeout: UINT; var lpdwResult: DWORD): Longint;
  external 'SendMessageTimeoutW@user32.dll stdcall';

function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', OrigPath) then
  begin
    Result := True;
    Exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

procedure RemoveFromPath(const Dir: string);
var
  PathValue: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', PathValue) then
    Exit;

  PathValue := ';' + PathValue + ';';
  P := Pos(';' + Uppercase(Dir) + ';', Uppercase(PathValue));
  if P = 0 then
    Exit;

  Delete(PathValue, P, Length(Dir) + 1);
  if Length(PathValue) >= 2 then
    PathValue := Copy(PathValue, 2, Length(PathValue) - 2)
  else
    PathValue := '';

  RegWriteExpandStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', PathValue);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  MsgResult: DWORD;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveFromPath(ExpandConstant('{app}'));
    SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 'Environment',
      SMTO_ABORTIFHUNG, 5000, MsgResult);
  end;
end;
