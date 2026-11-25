Unicode true
ManifestSupportedOS all
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "v0.0.0-dev"
!endif

!ifndef SOURCE_DIR
  !error "SOURCE_DIR must point to the staged CoreVideo install folder"
!endif

!ifndef OUT_FILE
  !define OUT_FILE "CoreVideo-Setup-${VERSION}.exe"
!endif

!ifndef FILE_LIST
  !define FILE_LIST "corevideo-uninstall-files.nsh"
!endif

!macro RequireSourceFile RELPATH
  !if /FileExists "${SOURCE_DIR}\${RELPATH}"
  !else
    !error "SOURCE_DIR is missing required CoreVideo runtime file: ${RELPATH}"
  !endif
!macroend

!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\obs-zoom-plugin.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\bin\64bit\obs-zoom-plugin.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\obs-zoom-plugin.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\CoreVideoOAuthCallback.exe"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\Qt6Core.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\Qt6Gui.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\Qt6Network.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\Qt6Widgets.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\plugins\platforms\qwindows.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\plugins\tls\qcertonlybackend.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\plugins\tls\qschannelbackend.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\data\locale\en-US.ini"
; The Tiles wall's shader, shipped by the same install(DIRECTORY data/ ...) rule
; as the locale file above. This check only proves the file is present in the
; package being built: a package missing it (or missing the data/ tree
; entirely) produces a Tiles source that cannot draw at all (see
; src/zoom-tiles-effect-policy.h). It says nothing about whether the file's
; CONTENTS match this build of the DLL — an existing-but-stale effect file
; passes this check and would only show up as a degraded wall at runtime.
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\data\effects\corevideo-tiles.effect"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\ZoomObsEngine.exe"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\sdk.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\sdkExt.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\zSDK.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\zVideoApp.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\zVideoUI.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\zNet.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\libcrypto-3-zm.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\libssl-3-zm.dll"
!insertmacro RequireSourceFile "plugins\obs-zoom-plugin\zoom-runtime\zoom.manifest"

Var ObsRoot
Var PortableInstall

Name "CoreVideo for OBS"
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\obs-studio"
InstallDirRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio" "InstallLocation"
BrandingText "CoreVideo ${VERSION}"

!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

!macro SetInstallDirIfObsRoot CANDIDATE
  IfFileExists "${CANDIDATE}\bin\64bit\obs64.exe" 0 +2
    StrCpy $INSTDIR "${CANDIDATE}"
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "CoreVideo requires 64-bit Windows and 64-bit OBS Studio."
    Abort
  ${EndIf}

  ; Preserve /D for unattended portable installs; validate it before copying.
  IfSilent init_done
  IfFileExists "$INSTDIR\bin\64bit\obs64.exe" init_done
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio" "InstallLocation"
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${Else}
    ReadRegStr $0 HKLM "Software\OBS Studio" "Install_Dir"
    ${If} $0 != ""
      StrCpy $INSTDIR $0
    ${EndIf}
  ${EndIf}
  IfFileExists "$INSTDIR\bin\64bit\obs64.exe" init_done
  !insertmacro SetInstallDirIfObsRoot "$PROGRAMFILES64\obs-studio"
  IfFileExists "$INSTDIR\bin\64bit\obs64.exe" init_done
  !insertmacro SetInstallDirIfObsRoot "$LOCALAPPDATA\Programs\obs-studio"
init_done:
FunctionEnd

Function .onVerifyInstDir
  IfFileExists "$INSTDIR\bin\64bit\obs64.exe" done
  IfFileExists "$INSTDIR\obs64.exe" 0 invalid
  IfFileExists "$INSTDIR\..\..\bin\64bit\obs64.exe" 0 invalid
  StrCpy $INSTDIR "$INSTDIR\..\.."
  Goto done
invalid:
  MessageBox MB_ICONSTOP "Select the OBS Studio install folder, usually C:\Program Files\obs-studio. Do not select a profile folder or a CoreVideo package folder." /SD IDOK
  Abort
done:
FunctionEnd

!macro RequireProcessClosed PROCESS_NAME DISPLAY_NAME ACTION_NAME
  nsExec::ExecToStack 'cmd /C tasklist /FI "IMAGENAME eq ${PROCESS_NAME}" /NH ^| findstr /I "${PROCESS_NAME}" >NUL'
  Pop $0
  Pop $1
  ${If} $0 == 0
    MessageBox MB_ICONSTOP "${DISPLAY_NAME} is currently running. Close ${ACTION_NAME} before installing CoreVideo."
    Abort
  ${EndIf}
!macroend

Function CheckObsClosed
  !insertmacro RequireProcessClosed "obs64.exe" "OBS Studio" "OBS Studio"
  !insertmacro RequireProcessClosed "CoreVideoSidecar.exe" "CoreVideo Sidecar" "CoreVideo Sidecar"
  !insertmacro RequireProcessClosed "ZoomObsEngine.exe" "CoreVideo Zoom engine" "CoreVideo Zoom engine"
  !insertmacro RequireProcessClosed "CoreVideoOAuthCallback.exe" "CoreVideo OAuth callback helper" "the browser sign-in callback helper"
  !insertmacro RequireProcessClosed "ZoomSdkAuthProbe.exe" "CoreVideo Zoom SDK auth probe" "the Zoom SDK auth probe"
  !insertmacro RequireProcessClosed "ffmpeg.exe" "FFmpeg" "FFmpeg"
FunctionEnd

!macro VerifyInstalledFile RELPATH
  IfFileExists "$INSTDIR\${RELPATH}" +3
  MessageBox MB_ICONSTOP "CoreVideo installation is incomplete. Missing: ${RELPATH}"
  Abort
!macroend

Function VerifyInstalledRuntime
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\obs-zoom-plugin.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\bin\64bit\obs-zoom-plugin.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\obs-zoom-plugin.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\CoreVideoOAuthCallback.exe"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\Qt6Core.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\Qt6Gui.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\Qt6Network.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\Qt6Widgets.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\plugins\platforms\qwindows.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\plugins\tls\qcertonlybackend.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\plugins\tls\qschannelbackend.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\data\locale\en-US.ini"
  ; Checked after the copy as well as before it: the partial install this guards
  ; against is one where the DLL landed and data\ did not.
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\data\effects\corevideo-tiles.effect"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\ZoomObsEngine.exe"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\sdk.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\sdkExt.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\zSDK.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\zVideoApp.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\zVideoUI.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\zNet.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\libcrypto-3-zm.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\libssl-3-zm.dll"
  !insertmacro VerifyInstalledFile "plugins\obs-zoom-plugin\zoom-runtime\zoom.manifest"
FunctionEnd

Section "CoreVideo for OBS" SecCoreVideo
  Call CheckObsClosed

  ; The directory page selects an OBS installation, not the package destination.
  Call .onVerifyInstDir
  StrCpy $ObsRoot $INSTDIR
  ClearErrors
  GetDLLVersion "$ObsRoot\bin\64bit\obs64.exe" $0 $1
  IfErrors unsupported_obs
  IntCmp $0 2097154 check_patch unsupported_obs version_ok
check_patch:
  IntCmp $1 131072 version_ok unsupported_obs version_ok
unsupported_obs:
  MessageBox MB_ICONSTOP "CoreVideo requires OBS Studio 32.2.2 or newer. Update OBS before installing." /SD IDOK
  SetErrorLevel 2
  Abort
version_ok:
  ; Standard installs use ProgramData, portable installs stay self-contained.
  StrCpy $PortableInstall 1
  IfFileExists "$ObsRoot\portable_mode.txt" destination_ready
  IfFileExists "$ObsRoot\portable_mode" destination_ready
  StrCpy $PortableInstall 0
  SetShellVarContext all
  StrCpy $INSTDIR "$APPDATA\obs-studio"
destination_ready:
  SetOutPath "$INSTDIR"
  File /r "${SOURCE_DIR}\*.*"
  Call VerifyInstalledRuntime
  ; Retire only the old entry DLL after the new package is verified. Preserve
  ; its runtime/data and a rollback copy; never sweep shared DLLs by wildcard.
  ${If} $INSTDIR != $ObsRoot
    IfFileExists "$ObsRoot\obs-plugins\64bit\obs-zoom-plugin.dll" 0 migration_done
    IfFileExists "$ObsRoot\obs-plugins\64bit\obs-zoom-plugin.dll.pre-modern" migration_backup_exists
    Rename "$ObsRoot\obs-plugins\64bit\obs-zoom-plugin.dll" "$ObsRoot\obs-plugins\64bit\obs-zoom-plugin.dll.pre-modern"
    Goto migration_done
migration_backup_exists:
    Delete "$ObsRoot\obs-plugins\64bit\obs-zoom-plugin.dll"
migration_done:
  ${EndIf}

  WriteUninstaller "$INSTDIR\Uninstall-CoreVideo.exe"
  ${If} $PortableInstall == 0
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "DisplayName" "CoreVideo for OBS"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "Publisher" "CoreVideo"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "UninstallString" "$INSTDIR\Uninstall-CoreVideo.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "NoRepair" 1
  ${EndIf}
SectionEnd

Section "Uninstall"
  Call un.CheckObsClosed
  !include "${FILE_LIST}"
  Delete "$INSTDIR\Uninstall-CoreVideo.exe"
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "InstallLocation"
  ${If} $0 == $INSTDIR
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS"
  ${EndIf}
SectionEnd

Function un.CheckObsClosed
  !insertmacro RequireProcessClosed "obs64.exe" "OBS Studio" "OBS Studio"
  !insertmacro RequireProcessClosed "CoreVideoSidecar.exe" "CoreVideo Sidecar" "CoreVideo Sidecar"
  !insertmacro RequireProcessClosed "ZoomObsEngine.exe" "CoreVideo Zoom engine" "CoreVideo Zoom engine"
  !insertmacro RequireProcessClosed "CoreVideoOAuthCallback.exe" "CoreVideo OAuth callback helper" "the browser sign-in callback helper"
  !insertmacro RequireProcessClosed "ZoomSdkAuthProbe.exe" "CoreVideo Zoom SDK auth probe" "the Zoom SDK auth probe"
  !insertmacro RequireProcessClosed "ffmpeg.exe" "FFmpeg" "FFmpeg"
FunctionEnd
