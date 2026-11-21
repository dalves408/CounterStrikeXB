; Counter-Strike hi-res texture add-on: an extraction-only Setup for an installed game.
; Built by scripts/build_texture_addon.py from installer/Output/texture-addon-<version>/texture-payload.json
; (scripts/build_texture_payload.py), which generates texture_payload.iss and texture_parts_code.iss.
; The texture-NN.bin parts are 7z archives beside this EXE or downloaded from the pinned release
; URLs; each is checked (size, SHA-256) before it is used. The pack goes to {app}\texture_pack,
; where the game looks for it (src/cs_texture_pack.c). No uninstaller: delete texture_pack to remove it.
#include "texture_payload.iss"
#define GameGuid "D7CEE68B-0EA8-48F7-884A-EF8B2BE16E1A"
#define TextureMarker "cs-addon-complete.txt"

[Setup]
AppId={{DC589BBF-5D91-47CD-A341-3736A1DF9AFC}
AppName=Counter-Strike Hi-Res Textures
AppVersion={#TextureVersion}
AppPublisher=dalves408/CounterStrikeXB
AppPublisherURL=https://github.com/dalves408/CounterStrikeXB
DefaultDirName={code:GameDir}
UsePreviousAppDir=no
AppendDefaultDirName=no
DisableDirPage=no
DisableProgramGroupPage=yes
DirExistsWarning=no
Uninstallable=no
CreateUninstallRegKey=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=no
SetupIconFile=..\resources\cs.ico
WizardSmallImageFile=wizard_small_55.bmp,wizard_small_64.bmp,wizard_small_80.bmp,wizard_small_96.bmp,wizard_small_128.bmp
OutputBaseFilename=CounterStrike-HiRes-Textures-{#TextureVersion}-Setup
ArchiveExtraction=enhanced/nopassword
Compression=lzma2/max
SolidCompression=yes
LZMANumBlockThreads=1
; In the look of the game's manual, as the game's own menus (src/cs_ui.c): the dark style over the manual's navy
; blueprint page, its title picture on the Welcome and Finished pages (all made by scripts/build_installer.py)
WizardStyle=modern dark hidebevels
WizardImageFile=wizard_image_164.bmp,wizard_image_192.bmp,wizard_image_246.bmp,wizard_image_273.bmp,wizard_image_328.bmp
WizardBackImageFile=wizard_back_640.png,wizard_back_960.png,wizard_back_1280.png
WizardBackColor=#122C52

[Messages]
#ifdef TextureHasDownload
WelcomeLabel2=Add the recreated 4x hi-res textures to Counter-Strike (Xbox recompilation).%n%nSetup uses the texture-NN.bin parts beside this EXE when they are there and downloads the others ({#TextureDownloadText}). An interrupted download resumes when Setup runs again.%n%nClose the game before you continue. Needs a game build with hi-res texture support (F1 menu: Hi-res textures).
#else
WelcomeLabel2=Add the recreated 4x hi-res textures to Counter-Strike (Xbox recompilation).%n%nKeep the {#TexturePartCount} texture-NN.bin parts beside this Setup EXE.%n%nClose the game before you continue. Needs a game build with hi-res texture support (F1 menu: Hi-res textures).
#endif
SelectDirLabel3=Choose your installed Counter-Strike game folder (the one with cs2_xbox_recomp.exe).
ButtonInstall=&Extract
ReadyLabel1=Ready to extract the hi-res texture pack.
ReadyLabel2a=Click Extract to continue, or Back to choose another game folder.
FinishedHeadingLabel=Texture pack installed
FinishedLabel=Start the game. Hi-res textures are on by default; F1 > Hi-res textures turns them off and on while you play. A Render scale of Medium or High shows the extra detail.

[Files]
Source: "cs_texture_download.exe"; Flags: dontcopy

[Code]
#include "texture_download_code.iss"

const
  GameKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#GameGuid}}_is1';

function GameExists(Dir: String): Boolean;
begin
  Result := FileExists(AddBackslash(Dir) + 'cs2_xbox_recomp.exe');
end;

function GameDir(Param: String): String;
var Dir: String;
begin
  if RegQueryStringValue(HKCU, GameKey, 'Inno Setup: App Path', Dir) then
    if GameExists(Dir) then begin Result := RemoveBackslashUnlessRoot(Dir); Exit; end;
  if RegQueryStringValue(HKLM, GameKey, 'Inno Setup: App Path', Dir) then
    if GameExists(Dir) then begin Result := RemoveBackslashUnlessRoot(Dir); Exit; end;
  Dir := ExpandConstant('{src}');
  if GameExists(Dir) then Result := Dir
  else Result := 'C:\Games\Counter-Strike';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not WizardSilent and not GameExists(WizardDirValue) then begin
    MsgBox('Choose the installed Counter-Strike game folder (it has cs2_xbox_recomp.exe).', mbError, MB_OK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if not GameExists(ExpandConstant('{app}')) then
    Result := 'Choose the installed Counter-Strike game folder (it has cs2_xbox_recomp.exe).'
  else
    Result := PrepareTexturePayload;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpReady then begin
    if TexturePackNeedsInstall then begin
      WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
      WizardForm.PageNameLabel.Caption := SetupMessage(msgWizardReady);
      WizardForm.PageDescriptionLabel.Caption := SetupMessage(msgReadyLabel1);
      WizardForm.ReadyLabel.Caption := SetupMessage(msgReadyLabel2a);
    end else begin
      WizardForm.NextButton.Caption := '&Continue';
      WizardForm.PageNameLabel.Caption := 'Texture pack already installed';
      WizardForm.PageDescriptionLabel.Caption := 'This release is already in this game folder.';
      WizardForm.ReadyLabel.Caption := 'Click Continue to finish.';
    end;
  end;
  if (CurPageID = wpFinished) and TexturePackAlreadyCurrent then begin
    WizardForm.FinishedHeadingLabel.Caption := 'Texture pack already installed';
    WizardForm.FinishedLabel.Caption := 'The current 4x texture pack is already installed.';
  end;
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  TextureCancelButtonClick(Cancel, Confirm);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then InstallTexturePack;
  if CurStep = ssDone then TextureFinishCacheChoice;
end;

procedure DeinitializeSetup;
begin
  TextureCleanupOnExit;
end;

function GetCustomSetupExitCode: Integer;
begin
  if TexturePackInstalled then Result := 0 else Result := 1;
end;
