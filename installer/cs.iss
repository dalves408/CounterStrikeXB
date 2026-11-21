; Inno Setup script for the Counter-Strike (Xbox) recompilation (Windows 10/11, 64-bit).
; The same Setup as the Marvel vs. Capcom 2 recompilation's, for this game.
; Built by:
;   python scripts/build_installer.py --version <version> (it runs the steps below)
; Output: installer\Output\CounterStrike-Setup-<version>.exe
; If the game is already installed (its game files are in place), Setup only updates the program files:
; it does not ask for the disc and does not copy the 1.6 GB of game files again.
#ifndef AppVersion
#define AppVersion "0.9.0"
#endif
#ifndef BuildDir
#define BuildDir "..\build_release\RelWithDebInfo"
#endif
; The optional hi-res texture pack (task "hires"): texture_payload.iss and texture_parts_code.iss pin
; every texture-NN.bin part's URL, size and SHA-256 (scripts/build_installer.py writes them from
; installer/texture-payload-<version>.json); installer/texture_download_code.iss does the work.
#include "texture_payload.iss"
#define TextureOptional
#define TextureMarker "cs-addon-complete.txt"

[Setup]
AppId={{D7CEE68B-0EA8-48F7-884A-EF8B2BE16E1A}
AppName=Counter-Strike
AppVersion={#AppVersion}
AppPublisher=dalves408/CounterStrikeXB
AppPublisherURL=https://github.com/dalves408/CounterStrikeXB
DefaultDirName=C:\Games\Counter-Strike
DefaultGroupName=Counter-Strike
DisableProgramGroupPage=yes
; fresh install defaults to C:\Games\Counter-Strike; an existing install is found by AppId and its folder is pre-filled; the folder page is always shown so it can be changed
DisableDirPage=no
UsePreviousAppDir=yes
; a running game or settings program is closed so its exe can be replaced
CloseApplications=yes
CloseApplicationsFilter=cs2_xbox_recomp.exe,config.exe
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile=..\resources\cs.ico
; the picture at the top right of every wizard page is the same icon (the sizes are for different screen scalings)
WizardSmallImageFile=wizard_small_55.bmp,wizard_small_64.bmp,wizard_small_80.bmp,wizard_small_96.bmp,wizard_small_128.bmp
UninstallDisplayIcon={app}\cs2_xbox_recomp.exe
OutputBaseFilename=CounterStrike-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
; In the look of the game's manual, as the game's own menus (src/cs_ui.c): the dark style over the manual's navy
; blueprint page, its title picture on the Welcome and Finished pages (all made by scripts/build_installer.py)
WizardStyle=modern dark hidebevels
WizardImageFile=wizard_image_164.bmp,wizard_image_192.bmp,wizard_image_246.bmp,wizard_image_273.bmp,wizard_image_328.bmp
WizardBackImageFile=wizard_back_640.png,wizard_back_960.png,wizard_back_1280.png
WizardBackColor=#122C52
; the Welcome page shows the title picture first, as the Finished page does last
DisableWelcomePage=no
ExtraDiskSpaceRequired=1700000000
; the texture pack's parts are 7z archives (extracted by Setup itself)
ArchiveExtraction=enhanced/nopassword

[Messages]
WelcomeLabel2=This installs the Counter-Strike (Xbox) PC recompilation.%n%nThe game's own files are not included. You need your own copy of the Xbox disc: a disc image (.iso or .xiso) or a folder with the game's files. Setup will ask for it and copy the files it needs.

[Files]
Source: "{#BuildDir}\cs2_xbox_recomp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\config.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\insignia_setup.exe"; DestDir: "{app}"; Flags: ignoreversion
; SDL3 (zlib license): PlayStation and other non-Xbox controllers
Source: "{#BuildDir}\SDL3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\SDL3-LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
; the import tool only runs during setup; it is not installed
Source: "{#BuildDir}\cs_import.exe"; DestDir: "{tmp}"; Flags: ignoreversion deleteafterinstall
; the title update, as a patch to the disc's default.xbe: used by cs_import, and by the game when an
; install made before it still has the disc's file
Source: "{#BuildDir}\cs_tu.xbepatch"; DestDir: "{tmp}"; Flags: ignoreversion deleteafterinstall
Source: "{#BuildDir}\cs_tu.xbepatch"; DestDir: "{app}"; Flags: ignoreversion
; downloads and checks the texture pack's parts (installer/texture_download.c); never installed
Source: "{#BuildDir}\cs_texture_download.exe"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\Counter-Strike"; Filename: "{app}\cs2_xbox_recomp.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\Counter-Strike Settings"; Filename: "{app}\config.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\Counter-Strike Insignia Setup"; Filename: "{app}\insignia_setup.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Counter-Strike"; Filename: "{app}\cs2_xbox_recomp.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
; hidden when this pack is already installed and complete (an update or repair does not fetch it again);
; silent installs: /TASKS="hires" (with /MERGETASKS to keep the others)
Name: "hires"; Description: "Download and install the hi-res texture pack ({#TextureDownloadText}, {#TextureInstalledText} on disk; uses texture-NN.bin files beside Setup if present)"; Flags: unchecked; Check: TexturePackNeedsInstall

[UninstallDelete]
Type: filesandordirs; Name: "{app}\game_files"
; the toolkit's kernel log, written beside the game
Type: files; Name: "{app}\xbox_kernel.log"
Type: filesandordirs; Name: "{app}\texture_pack"

[Code]
#include "texture_download_code.iss"

var
  SrcPage, UpdatePage: TWizardPage;
  SrcEdit: TNewEdit;
  UpdateOnly, UpdateMode: Boolean;
  OldDir, OldVer, NewVer: String;
  VerRel: Integer;          { -1: the installed version is older, 0: same, 1: newer than this Setup }
  RepairRadio, QuitRadio: TNewRadioButton;
  QuitNow: Boolean;
  TexturesReady: Boolean;   { every texture part is verified and waiting to be extracted }
  TextureSkipNote: String;  { why the texture pack was not installed, for the last page }

{ The game files are in place: default.xbe and a few of the largest files, so an install that was
  interrupted halfway is not mistaken for a finished one. }
function GameFilesPresent(Dir: String): Boolean;
begin
  Result := FileExists(Dir + '\game_files\default.xbe') and
            FileExists(Dir + '\game_files\FrontEnd.SXWad') and
            FileExists(Dir + '\game_files\de_dust2.SXWad') and
            FileExists(Dir + '\game_files\de_vostok.SXWad');
end;

{ Where a previous Setup put the game, or '' }
function PreviousAppDir: String;
var
  Key, Dir: String;
begin
  Result := '';
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{D7CEE68B-0EA8-48F7-884A-EF8B2BE16E1A}_is1';
  if RegQueryStringValue(HKCU, Key, 'Inno Setup: App Path', Dir) or
     RegQueryStringValue(HKLM, Key, 'Inno Setup: App Path', Dir) then
    Result := RemoveBackslashUnlessRoot(Dir);
end;

{ The version a previous Setup recorded, or '' }
function PreviousVersion: String;
var
  Key, V: String;
begin
  Result := '';
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{D7CEE68B-0EA8-48F7-884A-EF8B2BE16E1A}_is1';
  if RegQueryStringValue(HKCU, Key, 'DisplayVersion', V) or
     RegQueryStringValue(HKLM, Key, 'DisplayVersion', V) then
    Result := V;
end;

{ The n-th dot-separated number of a version such as 1.0.2 (0 when missing) }
function VerPart(V: String; N: Integer): Integer;
var
  I, P: Integer;
begin
  for I := 1 to N do
  begin
    P := Pos('.', V);
    if P = 0 then begin V := ''; Break; end;
    V := Copy(V, P + 1, Length(V));
  end;
  P := Pos('.', V);
  if P > 0 then V := Copy(V, 1, P - 1);
  Result := StrToIntDef(V, 0);
end;

{ -1 if A is older than B, 0 if the same, 1 if newer }
function CompareVer(A, B: String): Integer;
var
  I: Integer;
begin
  Result := 0;
  for I := 0 to 3 do
    if VerPart(A, I) <> VerPart(B, I) then
    begin
      if VerPart(A, I) < VerPart(B, I) then Result := -1 else Result := 1;
      Exit;
    end;
end;

{ The page that says what was found and asks the player to go ahead }
var
  UpdY: Integer;

procedure AddLine(Text: String; Bold: Boolean);
var
  L: TNewStaticText;
begin
  L := TNewStaticText.Create(UpdatePage);
  L.Parent := UpdatePage.Surface;
  L.AutoSize := False;
  L.WordWrap := True;
  L.Left := 0;
  L.Top := UpdY;
  L.Width := UpdatePage.SurfaceWidth;
  if Bold then L.Font.Style := [fsBold];
  L.Caption := Text;
  L.Height := ScaleY(18) * (1 + Length(Text) div 70);
  UpdY := UpdY + L.Height + ScaleY(6);
end;

procedure RadioClick(Sender: TObject);
begin
  if QuitRadio.Checked then
    WizardForm.NextButton.Caption := 'Quit'
  else
    WizardForm.NextButton.Caption := 'Repair';
end;

procedure AddChoice(var R: TNewRadioButton; Text: String; Checked: Boolean);
begin
  R := TNewRadioButton.Create(UpdatePage);
  R.Parent := UpdatePage.Surface;
  R.Left := 0;
  R.Top := UpdY;
  R.Width := UpdatePage.SurfaceWidth;
  R.Height := ScaleY(20);
  R.Caption := Text;
  R.Checked := Checked;
  R.OnClick := @RadioClick;
  UpdY := UpdY + R.Height + ScaleY(6);
end;

procedure CreateUpdatePage;
begin
  if VerRel = 0 then
    UpdatePage := CreateCustomPage(wpWelcome, 'This version is already installed',
      'Setup found the same version of Counter-Strike on this computer.')
  else
    UpdatePage := CreateCustomPage(wpWelcome, 'Counter-Strike is already installed',
      'Setup found an existing installation of the game on this computer.');
  UpdY := 0;
  AddLine('Installed in:  ' + OldDir, False);
  if OldVer <> '' then
    AddLine('Installed version:  ' + OldVer, False)
  else
    AddLine('Installed version:  (not recorded)', False);
  AddLine('This Setup:  version ' + NewVer, True);
  UpdY := UpdY + ScaleY(10);
  if VerRel = 0 then
  begin
    AddLine('The installed version matches this Setup. You can repair the installation, which reinstalls the ' +
            'program files and shortcuts. Your game files, saves and settings are kept, and you do not need your disc.', False);
    UpdY := UpdY + ScaleY(10);
    AddChoice(RepairRadio, 'Repair the installation', True);
    AddChoice(QuitRadio, 'Quit (leave everything as it is)', False);
    Exit;
  end;
  if VerRel < 0 then
    AddLine('Setup will update the program files from version ' + OldVer + ' to version ' + NewVer +
            '. Your game files, saves and settings are kept, and you do not need your disc.', False)
  else
    AddLine('A NEWER version than this Setup is installed. Continuing will replace it with the older version ' +
            NewVer + '. Your game files, saves and settings are kept.', False);
  UpdY := UpdY + ScaleY(10);
  AddLine('Click Update to continue, or Cancel to leave the installed version as it is.', False);
end;

procedure BrowseFile(Sender: TObject);
var
  F: String;
begin
  F := SrcEdit.Text;
  if GetOpenFileName('Select the disc image', F, '', 'Xbox disc images (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*', 'iso') then
    SrcEdit.Text := F;
end;

procedure BrowseFolder(Sender: TObject);
var
  D: String;
begin
  D := SrcEdit.Text;
  if BrowseForFolder('Select the folder that holds the game''s files (the one with default.xbe)', D, False) then
    SrcEdit.Text := D;
end;

{ The manual's look on the wizard's header: its type on the headings, and the gold double rule under the title. }
procedure ManualRule(Top, Height: Integer; Color: TColor);
var
  R: TBitmapImage;
begin
  R := TBitmapImage.Create(WizardForm);
  R.Parent := WizardForm.MainPanel;
  R.Left := 0;
  R.Top := Top;
  R.Width := WizardForm.MainPanel.Width;
  R.Height := Height;
  R.BackColor := Color;
end;

procedure ManualLook;
begin
  WizardForm.PageNameLabel.Font.Name := 'Bahnschrift';
  WizardForm.PageNameLabel.Font.Style := [fsBold];
  WizardForm.PageDescriptionLabel.Font.Name := 'Bahnschrift';
  WizardForm.WelcomeLabel1.Font.Name := 'Bahnschrift';
  WizardForm.WelcomeLabel1.Font.Style := [fsBold];
  WizardForm.FinishedHeadingLabel.Font.Name := 'Bahnschrift';
  WizardForm.FinishedHeadingLabel.Font.Style := [fsBold];
  ManualRule(WizardForm.MainPanel.Height - ScaleY(6), ScaleY(3), $2AC6E2);   { gold, as RGB 226,198,42 }
  ManualRule(WizardForm.MainPanel.Height - ScaleY(2), 1, $1E8496);
end;

procedure InitializeWizard;
var
  L: TNewStaticText;
  B1, B2: TNewButton;
begin
  ManualLook;
  NewVer := '{#AppVersion}';
  OldDir := PreviousAppDir;
  UpdateMode := (OldDir <> '') and GameFilesPresent(OldDir);
  if UpdateMode then
  begin
    OldVer := PreviousVersion;
    if OldVer <> '' then VerRel := CompareVer(OldVer, NewVer) else VerRel := -1;
    WizardForm.WelcomeLabel2.Caption :=
      'Counter-Strike is already installed on this computer.' + #13#10#13#10 +
      'Setup will show the installed version and this Setup''s version on the next page and ask you to confirm before it changes anything.';
    CreateUpdatePage;
  end;
  SrcPage := CreateCustomPage(wpSelectDir, 'Your copy of the game',
    'Select the disc image or folder that Setup should copy the game files from.');
  L := TNewStaticText.Create(SrcPage);
  L.Parent := SrcPage.Surface;
  L.Caption := 'Disc image (.iso, .xiso) or a folder containing default.xbe:';
  L.Left := 0; L.Top := 0;
  SrcEdit := TNewEdit.Create(SrcPage);
  SrcEdit.Parent := SrcPage.Surface;
  SrcEdit.Left := 0; SrcEdit.Top := L.Height + 8;
  SrcEdit.Width := SrcPage.SurfaceWidth;
  SrcEdit.Text := ExpandConstant('{param:SOURCE|}');   { setup /SOURCE=<image or folder> for silent installs }
  B1 := TNewButton.Create(SrcPage);
  B1.Parent := SrcPage.Surface;
  B1.Caption := 'Disc image...';
  B1.Left := 0; B1.Top := SrcEdit.Top + SrcEdit.Height + 8;
  B1.Width := ScaleX(110);
  B1.OnClick := @BrowseFile;
  B2 := TNewButton.Create(SrcPage);
  B2.Parent := SrcPage.Surface;
  B2.Caption := 'Folder...';
  B2.Left := B1.Left + B1.Width + 8; B2.Top := B1.Top;
  B2.Width := ScaleX(110);
  B2.OnClick := @BrowseFolder;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and (TextureSkipNote <> '') then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 + TextureSkipNote;
  if UpdateMode and (CurPageID = UpdatePage.ID) then
  begin
    if VerRel = 0 then RadioClick(nil) else WizardForm.NextButton.Caption := 'Update';
  end
  else if UpdateMode and UpdateOnly and (CurPageID = wpFinished) then
  begin
    if VerRel = 0 then
      WizardForm.FinishedLabel.Caption := 'Counter-Strike ' + NewVer + ' was repaired.' + #13#10#13#10 +
        'Your game files, saves and settings were kept.'
    else
      WizardForm.FinishedLabel.Caption := 'Counter-Strike was updated to version ' + NewVer + '.' + #13#10#13#10 +
        'Your game files, saves and settings were kept.';
  end;
end;

{ The "Ready to install" page says what is about to happen. }
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  S: String;
begin
  if UpdateMode and GameFilesPresent(WizardDirValue) then
  begin
    if VerRel = 0 then
      S := 'Repair Counter-Strike ' + NewVer + ' in:'
    else
    begin
      S := 'Update Counter-Strike';
      if OldVer <> '' then S := S + ' from version ' + OldVer;
      S := S + ' to version ' + NewVer + ' in:';
    end;
    S := S + NewLine + Space + WizardDirValue + NewLine + NewLine +
         'Only the program files are replaced. Your game files, saves and settings are kept.';
    if MemoTasksInfo <> '' then S := S + NewLine + NewLine + MemoTasksInfo;
    Result := S;
  end else
  begin
    S := '';
    if MemoDirInfo <> '' then S := S + MemoDirInfo + NewLine + NewLine;
    if MemoTasksInfo <> '' then S := S + MemoTasksInfo + NewLine;
    Result := S;
  end;
end;

{ No disc is needed when the game files are already in the chosen folder. }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = SrcPage.ID) and GameFilesPresent(WizardDirValue);
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if QuitNow then Confirm := False;      { Quit on the matching-version page: no "Exit Setup?" question }
  TextureCancelButtonClick(Cancel, Confirm);
end;

{ The texture parts are found or downloaded and checked before anything is installed. A failure
  never stops the game install: Retry, or install the game without the pack (silent installs
  skip it and end with exit code 2). }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Err: String;
begin
  Result := '';
  TexturesReady := False;
  if not WantTexturePack then Exit;
  repeat
    Err := PrepareTexturePayload;
    if Err = '' then begin
      TexturesReady := not TexturePackAlreadyCurrent;
      Exit;
    end;
    Log('Texture pack not prepared: ' + Err);
    if WizardSilent then Break;
  until MsgBox('The hi-res texture pack could not be prepared:' + #13#10#13#10 + Err + #13#10#13#10 +
               'Click Retry to try again, or Cancel to install the game without the hi-res textures ' +
               '(run Setup again later to add them).', mbError, MB_RETRYCANCEL) <> IDRETRY;
  TextureSkipNote := 'The hi-res texture pack was not installed: ' + Err;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if UpdateMode and (VerRel = 0) and (CurPageID = UpdatePage.ID) and QuitRadio.Checked then
  begin
    QuitNow := True;
    WizardForm.Close;
    Result := False;
    Exit;
  end;
  if CurPageID = SrcPage.ID then
    if (Trim(SrcEdit.Text) = '') or
       not (FileExists(Trim(SrcEdit.Text)) or DirExists(Trim(SrcEdit.Text))) then
    begin
      MsgBox('Select an existing disc image or folder.', mbError, MB_OK);
      Result := False;
    end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Msg: String;
begin
  if CurStep = ssInstall then
    UpdateOnly := GameFilesPresent(ExpandConstant('{app}'));
  if (CurStep = ssPostInstall) and not UpdateOnly then
  begin
    WizardForm.StatusLabel.Caption := 'Copying the game files from your disc (about 1.6 GB, a minute or two)...';
    WizardForm.FilenameLabel.Caption := '';
    if not Exec(ExpandConstant('{tmp}\cs_import.exe'),
        '"' + Trim(SrcEdit.Text) + '" "' + ExpandConstant('{app}\game_files') + '"',
        '', SW_HIDE, ewWaitUntilTerminated, Code) then
      Code := 99;
    if Code <> 0 then
    begin
      case Code of
        2: Msg := 'The disc image or folder could not be found.';
        3: Msg := 'That file is not an Xbox disc image.';
        4: Msg := 'That is not the supported version of Counter-Strike (the USA Xbox release).';
        5: Msg := 'Some game files are missing or damaged. The dump may be incomplete.';
        6: Msg := 'The game files could not be written to the install folder.';
        7: Msg := 'The game update could not be applied to the game files.';
      else
        Msg := 'The game files could not be copied.';
      end;
      MsgBox(Msg + #13#10#13#10 + 'Counter-Strike was installed but cannot start yet. ' +
        'Run Setup again and select a correct disc image or folder.', mbError, MB_OK);
    end;
  end;
  if (CurStep = ssPostInstall) and TexturesReady and GameFilesPresent(ExpandConstant('{app}')) then
    try
      InstallTexturePack;
    except
      TextureSkipNote := 'The hi-res texture pack was not installed: ' + GetExceptionMessage;
      Log(TextureSkipNote);
      if not WizardSilent then MsgBox(TextureSkipNote + #13#10#13#10 + 'The game itself is installed.', mbError, MB_OK);
    end;
  if CurStep = ssDone then TextureFinishCacheChoice;
end;

procedure DeinitializeSetup;
begin
  TextureCleanupOnExit;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if not GameFilesPresent(ExpandConstant('{app}')) then
    Result := 1
  else if WantTexturePack and not TexturePackInstalled then
    Result := 2;   { the game is installed, the texture pack is not }
end;
