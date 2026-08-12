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

!insertmacro RequireSourceFile "obs-plugins\64bit\obs-zoom-plugin.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\CoreVideoOAuthCallback.exe"
!insertmacro RequireSourceFile "obs-plugins\64bit\Qt6Core.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\Qt6Gui.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\Qt6Network.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\Qt6Widgets.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\plugins\platforms\qwindows.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\plugins\tls\qcertonlybackend.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\plugins\tls\qschannelbackend.dll"
!insertmacro RequireSourceFile "data\obs-plugins\obs-zoom-plugin\locale\en-US.ini"
; The Tiles wall's shader, shipped by the same install(DIRECTORY data/ ...) rule
; as the locale file above. This check only proves the file is present in the
; package being built: a package missing it (or missing the data/ tree
; entirely) produces a Tiles source that cannot draw at all (see
; src/zoom-tiles-effect-policy.h). It says nothing about whether the file's
; CONTENTS match this build of the DLL — an existing-but-stale effect file
; passes this check and would only show up as a degraded wall at runtime.
!insertmacro RequireSourceFile "data\obs-plugins\obs-zoom-plugin\effects\corevideo-tiles.effect"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\ZoomObsEngine.exe"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\sdk.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\sdkExt.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\zSDK.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\zVideoApp.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\zVideoUI.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\zNet.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\libcrypto-3-zm.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\libssl-3-zm.dll"
!insertmacro RequireSourceFile "obs-plugins\64bit\zoom-runtime\zoom.manifest"

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
  MessageBox MB_ICONSTOP "Select the OBS Studio install folder, usually C:\Program Files\obs-studio. Do not select a profile folder or a CoreVideo package folder."
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
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\obs-zoom-plugin.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\CoreVideoOAuthCallback.exe"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\Qt6Core.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\Qt6Gui.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\Qt6Network.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\Qt6Widgets.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\plugins\platforms\qwindows.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\plugins\tls\qcertonlybackend.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\plugins\tls\qschannelbackend.dll"
  !insertmacro VerifyInstalledFile "data\obs-plugins\obs-zoom-plugin\locale\en-US.ini"
  ; Checked after the copy as well as before it: the partial install this guards
  ; against is one where the DLL landed and data\ did not.
  !insertmacro VerifyInstalledFile "data\obs-plugins\obs-zoom-plugin\effects\corevideo-tiles.effect"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\ZoomObsEngine.exe"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\sdk.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\sdkExt.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\zSDK.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\zVideoApp.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\zVideoUI.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\zNet.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\libcrypto-3-zm.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\libssl-3-zm.dll"
  !insertmacro VerifyInstalledFile "obs-plugins\64bit\zoom-runtime\zoom.manifest"
FunctionEnd

Section "CoreVideo for OBS" SecCoreVideo
  Call CheckObsClosed

  ; v0.1.29 and earlier left the FFmpeg runtime loose in obs-plugins\64bit,
  ; where it collides with the same-named FFmpeg 8 DLLs OBS 32.2+ keeps in
  ; bin\64bit and stops OBS's own obs-ffmpeg module from loading. The runtime
  ; now lives in obs-plugins\64bit\corevideo-ffmpeg\; sweep any leftovers.
  Delete "$INSTDIR\obs-plugins\64bit\avcodec-*.dll"
  Delete "$INSTDIR\obs-plugins\64bit\avdevice-*.dll"
  Delete "$INSTDIR\obs-plugins\64bit\avfilter-*.dll"
  Delete "$INSTDIR\obs-plugins\64bit\avformat-*.dll"
  Delete "$INSTDIR\obs-plugins\64bit\avutil-*.dll"
  Delete "$INSTDIR\obs-plugins\64bit\swresample-*.dll"
  Delete "$INSTDIR\obs-plugins\64bit\swscale-*.dll"

  SetOutPath "$INSTDIR"
  File /r "${SOURCE_DIR}\*.*"
  Call VerifyInstalledRuntime

  WriteUninstaller "$INSTDIR\Uninstall-CoreVideo.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "DisplayName" "CoreVideo for OBS"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "Publisher" "CoreVideo"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "UninstallString" "$INSTDIR\Uninstall-CoreVideo.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Call un.CheckObsClosed
  !include "${FILE_LIST}"
  Delete "$INSTDIR\Uninstall-CoreVideo.exe"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideo for OBS"
SectionEnd

Function un.CheckObsClosed
  !insertmacro RequireProcessClosed "obs64.exe" "OBS Studio" "OBS Studio"
  !insertmacro RequireProcessClosed "CoreVideoSidecar.exe" "CoreVideo Sidecar" "CoreVideo Sidecar"
  !insertmacro RequireProcessClosed "ZoomObsEngine.exe" "CoreVideo Zoom engine" "CoreVideo Zoom engine"
  !insertmacro RequireProcessClosed "CoreVideoOAuthCallback.exe" "CoreVideo OAuth callback helper" "the browser sign-in callback helper"
  !insertmacro RequireProcessClosed "ZoomSdkAuthProbe.exe" "CoreVideo Zoom SDK auth probe" "the Zoom SDK auth probe"
  !insertmacro RequireProcessClosed "ffmpeg.exe" "FFmpeg" "FFmpeg"
FunctionEnd
