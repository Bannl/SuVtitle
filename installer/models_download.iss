// =====================================================================================================================
// SuVtitle - installer/models_download.iss: the LIGHT installer's model download, a part of the [Code] section.
// ---------------------------------------------------------------------------------------------------------------------
// #included by installer\SuVtitle_Setup.iss (in [Code], under #ifndef Full) AND by the installer test kit's throwaway
// setup (tests\installer\dltest.iss): one source, so the kit tests the code that ships. Before the #include, the including
// script defines the generated models block (ModelCount, ModelFile[], ModelSize[], ModelSha256[], ModelUrl[]: from
// installer\models.json) and, for tests only, MdlSilentAnswer. Everything here is named Mdl... and every event function
// is an event attribute, so the including script keeps its own event functions.
//
// What it does (the director's decision, 03/11/2026, with the fixes of that day's adversarial review):
//  * When the user clicks Install (NextButtonClick on wpReady: BEFORE anything is installed), each model that {app}\models
//    lacks, or holds with another size (with /VERIFYMODELS: or with another SHA-256), is downloaded by Inno's download
//    page into Setup's temporary folder {tmp}, ONE AT A TIME, and Inno checks its SHA-256 (a mismatch is an error, never
//    an install). A model in place with its exact size is kept and its content is not read, unless /VERIFYMODELS (about
//    30 s for the 2.9 GB model, measured): an update whose models are all there downloads nothing and needs no network.
//  * A model gets its final name only when it is complete. It first becomes {app}\models\<file>.part (size checked,
//    flushed to disk, permissions reset to the folder's), then replaces the final file in ONE rename (MoveFileEx with
//    REPLACE_EXISTING, same folder), after the installation (ssPostInstall). The previous file stays until that rename,
//    and stays if it fails. A *.part in {app}\models that this run did not make (a Setup killed earlier) is deleted.
//      - {tmp} on the installation drive: the downloads wait in {tmp} and are moved into place after the installation.
//      - {tmp} on another drive: each model is moved to the installation drive (a copy, flushed) right after its own
//        download, so {tmp}'s drive needs room for the largest model only. {app}\models can then be created before the
//        installation: removed again (when empty) if Setup stops; [Dirs] lets the uninstaller remove {app}.
//  * The free space is checked before the first download (MdlRoomProblem).
//  * An error (offline, a server error, a SHA-256 mismatch, no room) says what failed and offers Retry / Cancel; Cancel goes
//    back to the Ready page, nothing installed. Silent installs: the same without the page; with /SUPPRESSMSGBOXES the
//    answer is Cancel and Setup stops at its Preparing step: EXIT CODE 7 (Inno's "cannot proceed"), nothing installed.
//  * After the installation, a model that could not be put in place is named in a last message and Setup's exit code is
//    10 (MdlExitMissing); a model whose permissions could not be reset (other accounts may not read it) gives 11.
//  * The models about to be replaced are registered for Setup's in-use check (CloseApplications, the Restart Manager).
// =====================================================================================================================
#ifndef MdlSilentAnswer
  #define MdlSilentAnswer "IDCANCEL"
#endif
#define MdlI 0
#sub MdlEmitModel
  MdlAdd('{#ModelFile[MdlI]}', '{#ModelUrl[MdlI]}', '{#ModelSha256[MdlI]}', '{#ModelSize[MdlI]}');
#endsub

const
  MdlRoomApp = 536870912;      // 512 MiB more on the installation drive: the program files (about 0.3 GB) and some headroom
  MdlRoomTmp = 268435456;      // 256 MiB more on the drive of {tmp}, when it is another drive
  MdlSilentTries = 5;          // a silent install never answers Retry forever (MdlSilentAnswer = IDRETRY in tests)
  MdlExitMissing = 10;         // installed, but a model could not be put in place (GetCustomSetupExitCode)
  MdlExitAcl = 11;             // installed, every model in place, but the permissions of one could not be reset
  MdlMoveReplace = $1;         // MOVEFILE_REPLACE_EXISTING
  MdlMoveCopy = $2;            // MOVEFILE_COPY_ALLOWED: on another volume, a copy and then the source is deleted
  MdlMoveWriteThrough = $8;    // MOVEFILE_WRITE_THROUGH: such a copy is flushed to disk before the call returns
  MdlGenericWrite = $40000000;
  MdlShareRead = $1;
  MdlOpenExisting = 3;
  MdlBadHandle = $FFFFFFFF;

var
  MdlFile, MdlUrl, MdlSha256: array of String;
  MdlSize: array of Int64;
  MdlNeeded: array of Boolean;     // to fetch: absent, another size, or (/VERIFYMODELS) another SHA-256
  MdlChecked: array of Boolean;    // /VERIFYMODELS: the file in place was hashed in this run ...
  MdlCheckedOk: array of Boolean;  // ... and has the pinned SHA-256
  MdlFetched: array of Boolean;    // in {tmp}, downloaded and SHA-256-checked by Inno
  MdlStaged: array of Boolean;     // in MdlStageDir as <file>.part, with its size
  MdlStepDir: String;              // the {app}\models of the last attempt (Back to another folder resets what depends on it)
  MdlStageDir: String;             // the folder of this run's .part files
  MdlMadeDirs: array of String;    // the folders this run created for them, deepest first (removed at the end when empty)
  MdlVerify: Boolean;              // /VERIFYMODELS on the command line
  MdlStepError: String;            // why the step before the installation failed (a silent install: PrepareToInstall, code 7)
  MdlMissing: String;              // the models not put in place (the last message; exit code 10)
  MdlAclFailed: String;            // the models whose permissions could not be reset (exit code 11)
  MdlPage: TDownloadWizardPage;

function MdlMoveFileEx(const ExistingName, NewName: String; Flags: Cardinal): Integer;
  external 'MoveFileExW@kernel32.dll stdcall';
function MdlCreateFile(const Name: String; Access, Share, Security, Disposition, Flags, Template: Cardinal): Cardinal;
  external 'CreateFileW@kernel32.dll stdcall';
function MdlFlushFileBuffers(H: Cardinal): Integer;
  external 'FlushFileBuffers@kernel32.dll stdcall';
function MdlCloseHandle(H: Cardinal): Integer;
  external 'CloseHandle@kernel32.dll stdcall';

procedure MdlAdd(const AFile, AUrl, ASha256, ASize: String);
var n: Integer;
begin
  n := GetArrayLength(MdlFile);
  SetArrayLength(MdlFile, n + 1);
  SetArrayLength(MdlUrl, n + 1);
  SetArrayLength(MdlSha256, n + 1);
  SetArrayLength(MdlSize, n + 1);
  SetArrayLength(MdlNeeded, n + 1);
  SetArrayLength(MdlChecked, n + 1);
  SetArrayLength(MdlCheckedOk, n + 1);
  SetArrayLength(MdlFetched, n + 1);
  SetArrayLength(MdlStaged, n + 1);
  MdlFile[n] := AFile;
  MdlUrl[n] := AUrl;
  MdlSha256[n] := ASha256;
  MdlSize[n] := StrToInt64(ASize);
  MdlNeeded[n] := False;
  MdlChecked[n] := False;
  MdlCheckedOk[n] := False;
  MdlFetched[n] := False;
  MdlStaged[n] := False;
end;

// "6.0 GB", "147 MB" (1 GB = 1024^3 bytes, as Windows counts a drive; 1023.6 MiB and more reads "1.0 GB", never "1024 MB")
function MdlBytesText(const Bytes: Int64): String;
var MB, Tenths: Int64;
begin
  MB := (Bytes + 524288) div 1048576;
  if MB < 1024 then
    Result := IntToStr(MB) + ' MB'
  else begin
    Tenths := (Bytes * 10 + 536870912) div 1073741824;
    Result := IntToStr(Tenths div 10) + '.' + IntToStr(Tenths mod 10) + ' GB';
  end;
end;

procedure MdlAppend(var List: String; const Item: String);
begin
  if List <> '' then
    List := List + #13#10;
  List := List + Item;
end;

function MdlDir: String;
begin
  Result := ExpandConstant('{app}\models');
end;

function MdlFinal(const i: Integer): String;
begin
  Result := MdlDir + '\' + MdlFile[i];
end;

function MdlTmp(const i: Integer): String;
begin
  Result := ExpandConstant('{tmp}\') + MdlFile[i];
end;

function MdlPart(const i: Integer): String;
begin
  Result := MdlStageDir + '\' + MdlFile[i] + '.part';
end;

function MdlHasSize(const Path: String; const Size: Int64): Boolean;
var Have: Int64;
begin
  Result := FileSize64(Path, Have) and (Have = Size);
end;

// True when {app}\models holds model i with its exact size (its content is read only with /VERIFYMODELS).
function MdlInPlace(const i: Integer): Boolean;
begin
  Result := MdlHasSize(MdlFinal(i), MdlSize[i]);
end;

function MdlSameDrive(const A, B: String): Boolean;
begin
  Result := CompareText(ExtractFileDrive(A), ExtractFileDrive(B)) = 0;
end;

// The free bytes of the drive of Path; False when Windows does not say (then nothing is refused).
function MdlFreeBytes(const Path: String; var Free: Int64): Boolean;
var Total: Int64;
begin
  Result := GetSpaceOnDisk64(AddBackslash(ExtractFileDrive(Path)), Free, Total);
  if not Result then
    Log('Models: the free space of ' + ExtractFileDrive(Path) + ' could not be read: not checked.');
end;

// Creates Dir and its missing parents; records each folder it created, deepest first, in MdlMadeDirs.
function MdlForceDir(const Dir: String): Boolean;
var D: String; Missing: array of String; n, k: Integer;
begin
  D := RemoveBackslashUnlessRoot(Dir);
  n := 0;
  while (D <> '') and not DirExists(D) do begin
    SetArrayLength(Missing, n + 1);
    Missing[n] := D;
    n := n + 1;
    if ExtractFileDir(D) = D then
      Break;
    D := ExtractFileDir(D);
  end;
  Result := ForceDirectories(Dir);
  for k := 0 to n - 1 do
    if DirExists(Missing[k]) then begin
      SetArrayLength(MdlMadeDirs, GetArrayLength(MdlMadeDirs) + 1);
      MdlMadeDirs[GetArrayLength(MdlMadeDirs) - 1] := Missing[k];
      Log('Models: created the folder ' + Missing[k] + '.');
    end;
end;

// The folders this run created for its .part files go, when they are empty (Setup stopped, or a Back to another folder).
procedure MdlRemoveMadeDirs;
var k: Integer;
begin
  for k := 0 to GetArrayLength(MdlMadeDirs) - 1 do
    if DirExists(MdlMadeDirs[k]) and RemoveDir(MdlMadeDirs[k]) then
      Log('Models: removed the empty folder ' + MdlMadeDirs[k] + '.');
  SetArrayLength(MdlMadeDirs, 0);
end;

// This run's .part files that were not put in place go.
procedure MdlDropStaged;
var i: Integer;
begin
  for i := 0 to GetArrayLength(MdlFile) - 1 do
    if MdlStaged[i] then begin
      if DeleteFile(MdlPart(i)) then
        Log('Models: removed ' + MdlPart(i) + ' (not put in place).');
      MdlStaged[i] := False;
    end;
end;

// Every *.part in Dir that this run did not stage there: a copy left by a Setup that did not finish. Never trusted: deleted.
procedure MdlDeleteLeftovers(const Dir: String);
var R: TFindRec; Names: array of String; i, k, n: Integer; Mine: Boolean;
begin
  if not DirExists(Dir) then
    Exit;
  n := 0;
  if FindFirst(Dir + '\*.part', R) then begin
    try
      repeat
        if ((R.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) and (CompareText(Copy(R.Name, Length(R.Name) - 4, 5), '.part') = 0) then begin
          SetArrayLength(Names, n + 1);
          Names[n] := R.Name;
          n := n + 1;
        end;
      until not FindNext(R);
    finally
      FindClose(R);
    end;
  end;
  for k := 0 to n - 1 do begin
    Mine := False;
    if CompareText(Dir, MdlStageDir) = 0 then
      for i := 0 to GetArrayLength(MdlFile) - 1 do
        if MdlStaged[i] and (CompareText(Names[k], MdlFile[i] + '.part') = 0) then
          Mine := True;
    if not Mine then begin
      if DeleteFile(Dir + '\' + Names[k]) then
        Log('Models: removed ' + Dir + '\' + Names[k] + ', left by a Setup that did not finish.')
      else
        Log('Models: WARNING: ' + Dir + '\' + Names[k] + ', left by a Setup that did not finish, cannot be removed.');
    end;
  end;
end;

// /VERIFYMODELS: model i in place (with its size) has the pinned SHA-256? Hashed once per run and folder.
function MdlVerifiedOk(const i: Integer): Boolean;
var H: String;
begin
  if not MdlChecked[i] then begin
    MdlChecked[i] := True;
    MdlCheckedOk[i] := False;
    try
      H := GetSHA256OfFile(MdlFinal(i));
      MdlCheckedOk[i] := CompareText(H, MdlSha256[i]) = 0;
      if MdlCheckedOk[i] then
        Log('Models: ' + MdlFile[i] + ' in place: SHA-256 checked (/VERIFYMODELS), kept.')
      else
        Log('Models: ' + MdlFile[i] + ' in place has another SHA-256 (' + H + '): damaged, downloaded again (/VERIFYMODELS).');
    except
      Log('Models: ' + MdlFile[i] + ' in place cannot be read (' + GetExceptionMessage + '): downloaded again (/VERIFYMODELS).');
    end;
  end;
  Result := MdlCheckedOk[i];
end;

// '' when there is room for what is still to fetch, else what is missing where (Total: the bytes still to fetch). Same
// drive: every download lands there and stays. Another drive: {tmp} holds ONE download at a time (the largest counts) and
// the installation drive receives every model not moved there yet.
function MdlRoomProblem(var Total: Int64): String;
var i: Integer; Tmp, App: String; Free, TmpNeed, AppNeed: Int64; Same: Boolean;
begin
  Result := '';
  Tmp := ExpandConstant('{tmp}');
  App := ExpandConstant('{app}');
  Same := MdlSameDrive(Tmp, App);
  TmpNeed := 0;
  AppNeed := 0;
  Total := 0;
  for i := 0 to GetArrayLength(MdlFile) - 1 do
    if MdlNeeded[i] and not MdlStaged[i] then begin
      Total := Total + MdlSize[i];
      if not MdlFetched[i] then begin
        if Same then
          TmpNeed := TmpNeed + MdlSize[i]
        else if MdlSize[i] > TmpNeed then
          TmpNeed := MdlSize[i];
      end;
      if not Same then
        AppNeed := AppNeed + MdlSize[i];
    end;
  if Same then begin
    if (TmpNeed > 0) and MdlFreeBytes(App, Free) and (Free < TmpNeed + MdlRoomApp) then
      Result := 'Drive ' + ExtractFileDrive(App) + ' needs ' + MdlBytesText(TmpNeed + MdlRoomApp) + ' free (the models, '
        + MdlBytesText(TmpNeed) + ', and the program files); it has ' + MdlBytesText(Free) + '.';
  end else begin
    if (TmpNeed > 0) and MdlFreeBytes(Tmp, Free) and (Free < TmpNeed + MdlRoomTmp) then
      Result := 'Drive ' + ExtractFileDrive(Tmp) + ', where Setup downloads each model before moving it to the installation '
        + 'drive, needs ' + MdlBytesText(TmpNeed + MdlRoomTmp) + ' free (the largest model to download, ' + MdlBytesText(TmpNeed)
        + '); it has ' + MdlBytesText(Free) + '.';
    if (AppNeed > 0) and MdlFreeBytes(App, Free) and (Free < AppNeed + MdlRoomApp) then
      MdlAppend(Result, 'Drive ' + ExtractFileDrive(App) + ', the installation drive, needs ' + MdlBytesText(AppNeed + MdlRoomApp)
        + ' free (the models, ' + MdlBytesText(AppNeed) + ', and the program files); it has ' + MdlBytesText(Free) + '.');
  end;
end;

// Flushes F to disk, so that a power cut after the rename cannot leave the final name on unwritten data.
procedure MdlFlush(const F: String);
var H: Cardinal;
begin
  H := MdlCreateFile(F, MdlGenericWrite, MdlShareRead, 0, MdlOpenExisting, FILE_ATTRIBUTE_NORMAL, 0);
  if H = MdlBadHandle then
    Log(Format('Models: WARNING: %s cannot be opened to flush it to disk (error %d).', [F, DLLGetLastError]))
  else begin
    if MdlFlushFileBuffers(H) = 0 then
      Log(Format('Models: WARNING: flushing %s to disk failed (error %d).', [F, DLLGetLastError]));
    MdlCloseHandle(H);
  end;
end;

// Resets the permissions of F to the ones its folder gives (a file renamed in from {tmp} keeps those of Setup's temporary
// folder, which only the account that ran Setup may read).
function MdlResetAcl(const F: String): Boolean;
var Tries, Rc: Integer;
begin
  Result := False;
  for Tries := 1 to 5 do begin
    if Exec(ExpandConstant('{sys}\icacls.exe'), '"' + F + '" /reset /Q', '', SW_HIDE, ewWaitUntilTerminated, Rc) and (Rc = 0) then begin
      Result := True;
      Exit;
    end;
    Log(Format('Models: icacls /reset of %s: code %d (attempt %d).', [F, Rc, Tries]));
    Sleep(50 * Tries);
  end;
end;

// Moves the downloaded model i from {tmp} to <models folder>\<file>.part: a rename on the same volume, else a copy flushed
// to disk and the {tmp} file deleted (MoveFileEx). '' when done, size checked; else what went wrong.
function MdlStage(const i: Integer): String;
var Tries: Integer; Moved: Boolean;
begin
  Result := '';
  if not MdlForceDir(MdlDir) then begin
    Result := 'the folder ' + MdlDir + ' cannot be created.';
    Exit;
  end;
  MdlStageDir := MdlDir;
  if not MdlHasSize(MdlTmp(i), MdlSize[i]) then begin
    MdlFetched[i] := False;
    Result := 'the downloaded file is missing or incomplete (' + MdlTmp(i) + ').';
    Exit;
  end;
  MdlPage.SetText('Moving the model to the installation drive...', MdlFile[i]);
  if FileExists(MdlPart(i)) then
    DeleteFile(MdlPart(i));
  Moved := False;
  for Tries := 1 to 6 do begin
    Moved := MdlMoveFileEx(MdlTmp(i), MdlPart(i), MdlMoveCopy or MdlMoveWriteThrough) <> 0;
    if Moved then
      Break;
    Log(Format('Models: moving %s to %s: error %d (attempt %d).', [MdlTmp(i), MdlPart(i), DLLGetLastError, Tries]));
    Sleep(20 * Tries);
  end;
  if not (Moved and MdlHasSize(MdlPart(i), MdlSize[i])) then begin
    DeleteFile(MdlPart(i));
    if not FileExists(MdlTmp(i)) then
      MdlFetched[i] := False;
    Result := MdlFile[i] + ' cannot be moved to ' + MdlStageDir + ' (is the drive full?).';
    Exit;
  end;
  MdlStaged[i] := True;
  MdlFetched[i] := False;
  Log('Models: ' + MdlFile[i] + ' staged as ' + MdlPart(i) + '.');
end;

// Puts model i in place (ssPostInstall): its .part (staged now if it still is in {tmp}) is checked, flushed, given the
// folder's permissions, then replaces the final file in ONE rename. '' when done; else what went wrong (the previous file,
// if any, is untouched).
function MdlPlace(const i: Integer): String;
var Tries: Integer; Done: Boolean;
begin
  Result := '';
  if not MdlStaged[i] then begin
    Result := MdlStage(i);
    if Result <> '' then
      Exit;
  end;
  if not MdlHasSize(MdlPart(i), MdlSize[i]) then begin
    DeleteFile(MdlPart(i));
    MdlStaged[i] := False;
    Result := 'its downloaded copy ' + MdlPart(i) + ' is missing or incomplete.';
    Exit;
  end;
  MdlFlush(MdlPart(i));
  if not MdlResetAcl(MdlPart(i)) then begin
    Log('Models: WARNING: the permissions of ' + MdlPart(i) + ' could not be reset.');
    MdlAppend(MdlAclFailed, MdlFile[i]);
  end;
  Done := False;
  for Tries := 1 to 6 do begin
    Done := MdlMoveFileEx(MdlPart(i), MdlFinal(i), MdlMoveReplace or MdlMoveWriteThrough) <> 0;
    if Done then
      Break;
    Log(Format('Models: putting %s in place: error %d (attempt %d).', [MdlFinal(i), DLLGetLastError, Tries]));
    Sleep(20 * Tries);
  end;
  if not Done then begin
    Result := MdlFinal(i) + ' cannot be replaced (is SuVtitle, or one of its AI servers, still running?).';
    Exit;
  end;
  MdlStaged[i] := False;
  Log('Models: ' + MdlFile[i] + ' installed in ' + MdlDir + '.');
end;

// Fetches every needed model, one at a time: download into {tmp} (Inno checks the SHA-256), and on another drive the move
// to its .part right away. True when they all are; else MdlStepError says why.
function MdlDownloadAll(const Count: Integer): Boolean;
var i, k, Answer, Tries: Integer; Err: String; Aborted, Cross: Boolean;
begin
  Result := False;
  Cross := not MdlSameDrive(ExpandConstant('{tmp}'), ExpandConstant('{app}'));
  k := 0;
  for i := 0 to GetArrayLength(MdlFile) - 1 do begin
    if not MdlNeeded[i] then
      Continue;
    k := k + 1;
    Tries := 0;
    repeat
      Tries := Tries + 1;
      Err := '';
      Aborted := False;
      if MdlStaged[i] and not MdlHasSize(MdlPart(i), MdlSize[i]) then
        MdlStaged[i] := False;
      if MdlFetched[i] and not MdlHasSize(MdlTmp(i), MdlSize[i]) then
        MdlFetched[i] := False;
      if not (MdlStaged[i] or MdlFetched[i]) then begin
        // (no [Code] line may start with '[': Inno would read a section tag)
        MdlPage.Description := Format('Model %d of %d: %s (%s). Only the models that are not installed yet are downloaded.', [k,
          Count, MdlFile[i], MdlBytesText(MdlSize[i])]);
        MdlPage.Clear;
        MdlPage.Add(MdlUrl[i], MdlFile[i], MdlSha256[i]);
        try
          MdlPage.Download;
          MdlFetched[i] := True;
          Log('Models: ' + MdlFile[i] + ' downloaded, SHA-256 checked.');
        except
          Aborted := MdlPage.AbortedByUser;
          Err := MdlFile[i] + ': ' + GetExceptionMessage;
        end;
      end;
      // another drive: off {tmp} at once, so that {tmp}'s drive holds one model at a time
      if (Err = '') and Cross and not MdlStaged[i] then
        Err := MdlStage(i);
      if Err = '' then
        Answer := IDOK
      else if Aborted then begin
        Log('Models: the download was cancelled by the user.');
        Answer := IDCANCEL;
      end else begin
        Log('Models: failed: ' + Err);
        if WizardSilent and (Tries >= MdlSilentTries) then
          Answer := IDCANCEL
        else
          Answer := SuppressibleMsgBox('Setup could not download an AI model that SuVtitle needs:' + #13#10#13#10 + Err + #13#10#13#10
            + 'Check the internet connection (a proxy or a firewall may block huggingface.co), then click Retry. '
            + 'Cancel goes back to Setup: nothing has been installed.', mbError, MB_RETRYCANCEL, {#MdlSilentAnswer});
      end;
    until Answer <> IDRETRY;
    if Answer <> IDOK then begin
      if Aborted then
        MdlStepError := 'The download of the AI models was cancelled; nothing was installed.'
      else
        MdlStepError := 'An AI model could not be downloaded, so nothing was installed: ' + Err;
      Exit;
    end;
  end;
  Result := True;
end;

// NextButtonClick on wpReady: makes every needed model available (in {tmp}, or staged on the installation drive) BEFORE
// anything is installed. True when they all are; else MdlStepError says why.
function MdlFetchMissing: Boolean;
var i, Count, Answer, Tries: Integer; Total: Int64; Problem: String;
begin
  Result := False;
  MdlStepError := '';
  // a Back to another folder: what this run staged or created for the former one goes, and its checks no longer hold
  if CompareText(MdlStepDir, MdlDir) <> 0 then begin
    MdlDropStaged;
    MdlStageDir := '';
    MdlRemoveMadeDirs;
    for i := 0 to GetArrayLength(MdlFile) - 1 do
      MdlChecked[i] := False;
    MdlStepDir := MdlDir;
  end;
  MdlDeleteLeftovers(MdlDir);
  Count := 0;
  for i := 0 to GetArrayLength(MdlFile) - 1 do begin
    MdlNeeded[i] := not MdlInPlace(i);
    if MdlNeeded[i] then
      Count := Count + 1;
  end;
  if (Count = 0) and not MdlVerify then begin
    Log('Models: all in place in ' + MdlDir + ' with their sizes: nothing to download.');
    Result := True;
    Exit;
  end;
  MdlPage.Show;
  try
    if MdlVerify then
      for i := 0 to GetArrayLength(MdlFile) - 1 do
        if not MdlNeeded[i] then begin
          MdlPage.SetText('Checking the AI models already installed (SHA-256)...', MdlFile[i]);
          if not MdlVerifiedOk(i) then begin
            MdlNeeded[i] := True;
            Count := Count + 1;
          end;
        end;
    for i := 0 to GetArrayLength(MdlFile) - 1 do
      if MdlNeeded[i] then
        Log('Models: ' + MdlFile[i] + ' to download (' + IntToStr(MdlSize[i]) + ' bytes) from ' + MdlUrl[i])
      else
        Log('Models: ' + MdlFile[i] + ' is in place with its size (' + IntToStr(MdlSize[i]) + ' bytes): kept.');
    if Count = 0 then
      Result := True
    else begin
      Tries := 0;
      repeat
        Tries := Tries + 1;
        Problem := MdlRoomProblem(Total);
        if Problem = '' then
          Answer := IDOK
        else begin
          Log('Models: not enough free space: ' + Problem);
          if WizardSilent and (Tries >= MdlSilentTries) then
            Answer := IDCANCEL
          else
            Answer := SuppressibleMsgBox('There is not enough free disk space for the AI models SuVtitle needs ('
              + MdlBytesText(Total) + ').' + #13#10#13#10 + Problem + #13#10#13#10
              + 'Free some space, then click Retry. Cancel goes back to Setup: nothing has been installed.',
              mbError, MB_RETRYCANCEL, {#MdlSilentAnswer});
          if Answer <> IDRETRY then
            MdlStepError := 'There is not enough free disk space for the AI models (' + MdlBytesText(Total)
              + '), so nothing was installed. ' + Problem;
        end;
      until (Answer = IDOK) or (MdlStepError <> '');
      if MdlStepError = '' then
        Result := MdlDownloadAll(Count);
    end;
  finally
    MdlPage.Hide;
  end;
end;

// ssPostInstall: every fetched model goes in place; then the last word when one could not.
procedure MdlPlaceAll;
var i, Answer, Tries: Integer; Err: String;
begin
  for i := 0 to GetArrayLength(MdlFile) - 1 do begin
    if not MdlNeeded[i] then
      Continue;
    WizardForm.StatusLabel.Caption := 'Putting the AI models in place...';
    WizardForm.FilenameLabel.Caption := MdlFile[i];
    Tries := 0;
    repeat
      Tries := Tries + 1;
      if MdlStaged[i] or MdlFetched[i] then
        Err := MdlPlace(i)
      else
        Err := 'it was not downloaded.';
      if Err = '' then
        Answer := IDOK
      else begin
        Log('Models: ' + MdlFile[i] + ' NOT put in place: ' + Err);
        if WizardSilent and (Tries >= MdlSilentTries) then
          Answer := IDCANCEL
        else
          Answer := SuppressibleMsgBox('Setup could not put the AI model ' + MdlFile[i] + ' in ' + MdlDir + ':' + #13#10#13#10 + Err
            + #13#10#13#10 + 'Close SuVtitle if it runs, then click Retry. Cancel finishes the installation without this model.',
            mbError, MB_RETRYCANCEL, {#MdlSilentAnswer});
      end;
    until Answer <> IDRETRY;
    if Answer <> IDOK then begin
      if FileExists(MdlFinal(i)) then
        MdlAppend(MdlMissing, MdlFile[i] + ' (the previous file, of another size, is kept)')
      else
        MdlAppend(MdlMissing, MdlFile[i] + ' (missing)');
    end;
  end;
  if MdlMissing <> '' then begin
    Log('Models: NOT put in place: ' + MdlMissing);
    SuppressibleMsgBox('SuVtitle is installed, but these AI models could not be put in place:' + #13#10#13#10 + MdlMissing
      + #13#10#13#10 + 'SuVtitle runs without them, with less (without the 4B translation model it translates with the '
      + 'smaller one, without a word). Close SuVtitle and run this installer again: it downloads only what is missing.',
      mbError, MB_OK, IDOK);
  end else if MdlAclFailed <> '' then begin
    Log('Models: permissions not reset: ' + MdlAclFailed);
    SuppressibleMsgBox('SuVtitle is installed, but the permissions of these AI models could not be set (icacls /reset '
      + 'failed); other Windows accounts of this PC may not be able to use them:' + #13#10#13#10 + MdlAclFailed + #13#10#13#10
      + 'An administrator can fix it with: icacls "' + MdlDir + '" /reset /T', mbInformation, MB_OK, IDOK);
  end;
end;

<event('InitializeWizard')>
procedure MdlInitializeWizard;
var i: Integer;
begin
#for {MdlI = 0; MdlI < ModelCount; MdlI++} MdlEmitModel
  for i := 1 to ParamCount do
    if CompareText(ParamStr(i), '/VERIFYMODELS') = 0 then
      MdlVerify := True;
  if MdlVerify then
    Log('Models: /VERIFYMODELS: the models in place will be checked against their SHA-256.');
  MdlPage := CreateDownloadPage('Downloading the AI models',
    'Setup downloads the AI models SuVtitle needs from Hugging Face (only the ones not installed yet).', nil);
  MdlPage.ShowBaseNameInsteadOfUrl := True;
end;

// The Ready page says what will be downloaded, and how much, before the user clicks Install.
<event('UpdateReadyMemo')>
function MdlUpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var i, Count: Integer; Bytes: Int64; Lines: String;
begin
  Result := '';
  if MemoUserInfoInfo <> '' then
    Result := Result + MemoUserInfoInfo + NewLine + NewLine;
  if MemoDirInfo <> '' then
    Result := Result + MemoDirInfo + NewLine + NewLine;
  if MemoTypeInfo <> '' then
    Result := Result + MemoTypeInfo + NewLine + NewLine;
  if MemoComponentsInfo <> '' then
    Result := Result + MemoComponentsInfo + NewLine + NewLine;
  if MemoGroupInfo <> '' then
    Result := Result + MemoGroupInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + MemoTasksInfo + NewLine + NewLine;
  Count := 0;
  Bytes := 0;
  Lines := '';
  for i := 0 to GetArrayLength(MdlFile) - 1 do
    if not MdlInPlace(i) then begin
      Count := Count + 1;
      Bytes := Bytes + MdlSize[i];
      Lines := Lines + NewLine + Space + MdlFile[i] + ' (' + MdlBytesText(MdlSize[i]) + ')';
    end;
  if Count = 0 then
    Result := Result + 'AI models:' + NewLine + Space + 'already installed: nothing to download'
  else
    Result := Result + 'AI models to download from Hugging Face (' + MdlBytesText(Bytes) + '; an internet connection is needed):' + Lines;
  if MdlVerify then
    Result := Result + NewLine + Space + 'The installed ones are checked first (SHA-256, /VERIFYMODELS): a damaged one is downloaded again.';
end;

<event('NextButtonClick')>
function MdlNextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpReady then begin
    Result := MdlFetchMissing;
    // silent: stop at the Preparing step (exit code 7, "cannot proceed"), not here (exit code 1, "failed to initialize")
    if (not Result) and WizardSilent then begin
      if MdlStepError = '' then
        MdlStepError := 'The AI models could not be downloaded, so nothing was installed.';
      Result := True;
    end;
  end;
end;

<event('PrepareToInstall')>
function MdlPrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := MdlStepError;
  if Result <> '' then
    Log('Models: Setup stops before installing anything: ' + Result);
end;

// The models about to be replaced are checked for being in use, like the program files (CloseApplications).
<event('RegisterExtraCloseApplicationsResources')>
procedure MdlRegisterExtraCloseApplicationsResources;
var i: Integer;
begin
  for i := 0 to GetArrayLength(MdlFile) - 1 do
    if MdlNeeded[i] and FileExists(MdlFinal(i)) then begin
      RegisterExtraCloseApplicationsResource(False, MdlFinal(i));
      Log('Models: ' + MdlFinal(i) + ' registered for the in-use check (it will be replaced).');
    end;
end;

<event('CurStepChanged')>
procedure MdlCurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    MdlPlaceAll;
end;

<event('GetCustomSetupExitCode')>
function MdlGetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if MdlMissing <> '' then
    Result := MdlExitMissing
  else if MdlAclFailed <> '' then
    Result := MdlExitAcl;
  if Result <> 0 then
    Log(Format('Models: exit code %d.', [Result]));
end;

// Whatever happened: no .part of this run stays, nor a folder it created that is still empty.
<event('DeinitializeSetup')>
procedure MdlDeinitializeSetup;
begin
  MdlDropStaged;
  MdlRemoveMadeDirs;
end;
