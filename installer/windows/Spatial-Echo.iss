; Spatial Echo Windows installer. Unsigned; built by the manual
; release workflow from the windows-release CMake preset:
;   iscc /DMyAppVersion=1.0.0 installer\windows\Spatial-Echo.iss

#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#ifndef ArtefactDir
  #define ArtefactDir "..\..\build\cmake\windows-release\SpatialEcho_artefacts\Release"
#endif

#define MyAppName "Spatial Echo"
#define MyAppPublisher "Echoes Lab"
#define MyAppURL "https://echoeslabmusic.com/lab"

[Setup]
AppId={{F63DCC36-5AE3-4BD1-89C4-B81B6C361589}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\Echoes Lab\Spatial Echo
DefaultGroupName=Echoes Lab
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputBaseFilename=Spatial-Echo-v{#MyAppVersion}-windows
OutputDir=Output
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
PrivilegesRequired=admin
LicenseFile=license.txt
InfoBeforeFile=README.txt
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\Spatial-Echo.exe
WizardStyle=modern

[Types]
Name: "full"; Description: "VST3 plug-in and standalone app"
Name: "custom"; Description: "Choose components"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plug-in"; Types: full custom
Name: "standalone"; Description: "Standalone app"; Types: full custom

[Files]
Source: "{#ArtefactDir}\VST3\Spatial-Echo.vst3\*"; DestDir: "{commoncf64}\VST3\Spatial-Echo.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#ArtefactDir}\Standalone\Spatial-Echo.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#ArtefactDir}\Standalone\Resources\*"; DestDir: "{app}\Resources"; Components: standalone; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{group}\Spatial Echo"; Filename: "{app}\Spatial-Echo.exe"; Components: standalone
Name: "{group}\Uninstall Spatial Echo"; Filename: "{uninstallexe}"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Spatial-Echo.vst3"
