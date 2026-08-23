#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #error SourceDir must point to the prepared portable directory
#endif
#ifndef OutputDir
  #error OutputDir must point to the installer output directory
#endif
#ifndef RepoRoot
  #error RepoRoot must point to the repository root
#endif
#ifndef LicenseFile
  #error LicenseFile must point to the reflowed installer agreement
#endif
#ifndef SetupBaseName
  #define SetupBaseName "HDRBridge-v" + AppVersion + "-Windows-x64-Setup"
#endif

[Setup]
AppId={{9CE72A91-8875-47E1-A03A-643ED99AD78B}
AppName=HDR Bridge
AppVersion={#AppVersion}
AppVerName=HDR Bridge {#AppVersion}
AppPublisher=HDR Bridge
AppPublisherURL=https://github.com/RockLithium/hdrbridge
AppSupportURL=https://github.com/RockLithium/hdrbridge/issues
AppUpdatesURL=https://github.com/RockLithium/hdrbridge/releases
DefaultDirName={autopf}\HDR Bridge
DefaultGroupName=HDR Bridge
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#SetupBaseName}
SetupIconFile={#RepoRoot}\desktop\assets\hdrbridge.ico
UninstallDisplayIcon={app}\hdrbridge.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
UsePreviousAppDir=yes
UsePreviousGroup=yes
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany=HDR Bridge
VersionInfoDescription=HDR Bridge Windows x64 Installer
VersionInfoProductName=HDR Bridge
VersionInfoProductVersion={#AppVersion}
VersionInfoTextVersion={#AppVersion}
LicenseFile={#LicenseFile}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\HDR Bridge"; Filename: "{app}\hdrbridge.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\HDR Bridge"; Filename: "{app}\hdrbridge.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\hdrbridge.exe"; Description: "{cm:LaunchProgram,HDR Bridge}"; Flags: nowait postinstall skipifsilent

[Code]
const
  WM_CLOSE = $0010;

function FindWindow(lpClassName, lpWindowName: string): HWND;
  external 'FindWindowW@user32.dll stdcall';
function PostMessage(hWnd: HWND; Msg: UINT; wParam, lParam: Longint): Boolean;
  external 'PostMessageW@user32.dll stdcall';

function InitializeUninstall(): Boolean;
var
  AppWindow: HWND;
  Attempts: Integer;
begin
  Result := True;
  AppWindow := FindWindow('', 'HDR Bridge');
  if AppWindow = 0 then
    Exit;

  if MsgBox(
       'HDR Bridge is currently running.' + #13#10 + #13#10 +
       'Close it now and continue uninstalling?',
       mbConfirmation, MB_YESNO) <> IDYES then
  begin
    Result := False;
    Exit;
  end;

  PostMessage(AppWindow, WM_CLOSE, 0, 0);
  for Attempts := 1 to 50 do
  begin
    Sleep(100);
    if FindWindow('', 'HDR Bridge') = 0 then
      Exit;
  end;

  MsgBox(
    'HDR Bridge could not be closed. Finish or cancel any active conversion, '
    + 'close the application, and run the uninstaller again.',
    mbError, MB_OK);
  Result := False;
end;
