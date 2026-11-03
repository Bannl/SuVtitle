; ==============================================================================
; SuVtitle - tests/installer/dltest.iss: the installer test kit's THROWAWAY setup. Compiled and run by
; tests\installer\run_installer_kit.js, once per case; never by hand.
; ------------------------------------------------------------------------------
; It is NOT SuVtitle's installer: its own AppId, the current user's rights only (PrivilegesRequired=lowest), no
; uninstaller, no registry, no shortcut. It installs ONE text file into the folder the kit names (/DIR=) and runs the
; light installer's model download code -- installer\models_download.iss, #included below exactly as SuVtitle_Setup.iss
; #includes it (a mutant of it when the kit passes /DRegionFile=) -- against the kit's loopback server.
; The case file (/DCaseFile=, written by the kit) gives the models block (test models: their files, sizes, SHA-256 and
; loopback URLs, made by gen_iss_registry.js buildModelsBlock like the real one), the output names and the case's knobs.
; ==============================================================================
#ifndef CaseFile
  #error Compile this through tests\installer\run_installer_kit.js (it passes /DCaseFile=<the case file>).
#endif
#include CaseFile
#ifndef RegionFile
  #define RegionFile AddBackslash(SourcePath) + "..\..\installer\models_download.iss"
#endif
#ifndef CloseApps
  #define CloseApps "no"
#endif

[Setup]
AppId=SuVtitleInstallerKitThrowaway
AppName=SuVtitle installer kit (throwaway test setup)
AppVersion=0.0.1
DefaultDirName={localappdata}\SuVtitleInstallerKitUnused
PrivilegesRequired=lowest
Uninstallable=no
CloseApplications={#CloseApps}
RestartApplications=no
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=yes
DisableFinishedPage=yes
OutputDir={#OutDir}
OutputBaseFilename={#OutBase}
Compression=lzma2/fast
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
SetupLogging=yes

[Files]
Source: "{#ReadmeFile}"; DestDir: "{app}"; Flags: ignoreversion

[Code]
#include RegionFile

// ---- the kit's own hooks, NOT in the real installer: they log what the kit checks, and one damages a download on purpose.
// They are defined after the #include, so at each event they run after the download code's own implementation.
procedure KitList(const Tag, Dir: String);
var R: TFindRec; S: Int64;
begin
  if FindFirst(AddBackslash(Dir) + '*', R) then begin
    try
      repeat
        if (R.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then begin
          S := R.SizeHigh;
          S := S * 4294967296 + R.SizeLow;
          Log('KIT ' + Tag + ' file ' + R.Name + ' ' + IntToStr(S));
        end;
      until not FindNext(R);
    finally
      FindClose(R);
    end;
  end;
end;

<event('CurPageChanged')>
procedure KitCurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpReady then
    Log('KIT memo: ' + MdlUpdateReadyMemo('  ', ' | ', '', '', '', '', '', ''));
end;

<event('CurStepChanged')>
procedure KitCurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then begin
    Log('KIT tmp=' + ExpandConstant('{tmp}'));
    KitList('install-tmp', ExpandConstant('{tmp}'));
    KitList('install-models', ExpandConstant('{app}\models'));
#ifdef KitTruncateTmp
    // a download damaged between its SHA-256 check and its placement: one byte left
    SaveStringToFile(ExpandConstant('{tmp}\{#KitTruncateTmp}'), 'x', False);
    Log('KIT truncated {tmp}\{#KitTruncateTmp}');
#endif
#ifdef KitTruncatePart
    SaveStringToFile(ExpandConstant('{app}\models\{#KitTruncatePart}.part'), 'x', False);
    Log('KIT truncated {app}\models\{#KitTruncatePart}.part');
#endif
  end;
  if CurStep = ssDone then
    KitList('done-models', ExpandConstant('{app}\models'));
end;

<event('DeinitializeSetup')>
procedure KitDeinitializeSetup;
begin
  try
    KitList('deinit-models', ExpandConstant('{app}\models'));
  except
    Log('KIT deinit: ' + GetExceptionMessage);
  end;
end;
