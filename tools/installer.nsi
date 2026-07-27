; 云驿 Beta 安装程序 — NSIS 脚本
Unicode True

!define PRODUCT_NAME "云驿"
!define PRODUCT_VERSION "0.5-beta"
!define PRODUCT_PUBLISHER "云驿"
!define PRODUCT_WEB_SITE "https://github.com/qin-nai/yunyi"

; ---------- 路径 ----------
!define BINDIR "..\x64\Release"
!define SDK_DLL "..\webview2-sdk\build\native\x64\WebView2Loader.dll"
!define APP_HTML "..\app\relay\webview\yunyi.html"

; ---------- 输出 ----------
OutFile "${BINDIR}\云驿-Setup.exe"
InstallDir "$PROGRAMFILES64\${PRODUCT_NAME}"
InstallDirRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" "InstallLocation"

RequestExecutionLevel admin

; ---------- 界面 ----------
BrandingText "云驿 Beta"

Page components
Page directory
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles

; ---------- 段: 主程序 ----------
Section "云驿 应用程序 (必需)" SecApp
  SectionIn RO

  SetOutPath "$INSTDIR"

  File "${BINDIR}\云驿GUI.exe"
  File "${BINDIR}\云驿.exe"
  File "${SDK_DLL}"
  File "${BINDIR}\libcrypto-4-x64.dll"
  File "${BINDIR}\libssl-4-x64.dll"

  SetOutPath "$INSTDIR\webview"
  File "${APP_HTML}"
  SetOutPath "$INSTDIR"

  WriteUninstaller "$INSTDIR\uninst.exe"

  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
    "DisplayName" "${PRODUCT_NAME} ${PRODUCT_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
    "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
    "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
    "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
    "DisplayIcon" "$INSTDIR\云驿GUI.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
    "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
    "NoRepair" 1
SectionEnd

; ---------- 段: 开始菜单 ----------
Section "开始菜单快捷方式" SecShortcut
  CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
  CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" \
    "$INSTDIR\云驿GUI.exe" "" "$INSTDIR\云驿GUI.exe" 0
  CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\卸载 ${PRODUCT_NAME}.lnk" \
    "$INSTDIR\uninst.exe" "" "$INSTDIR\uninst.exe" 0
SectionEnd

; ---------- 段: 桌面快捷方式 ----------
Section "桌面快捷方式" SecDesktop
  CreateShortCut "$DESKTOP\${PRODUCT_NAME}.lnk" \
    "$INSTDIR\云驿GUI.exe" "" "$INSTDIR\云驿GUI.exe" 0
SectionEnd

; ---------- 段: WebView2 自动下载安装 ----------
Section "-WebView2 Runtime" SecWebView2
  SectionIn RO

  ReadRegStr $0 HKLM "SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00FB3A9A6E2E}" "pv"
  StrCmp $0 "" check_per_user done

  check_per_user:
    ReadRegStr $0 HKCU "SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00FB3A9A6E2E}" "pv"
    StrCmp $0 "" download_webview2 done

  download_webview2:
    DetailPrint "正在下载 Microsoft Edge WebView2 Runtime..."
    NSISdl::download "https://go.microsoft.com/fwlink/p/?LinkId=2124702" "$TEMP\WebView2Setup.exe"
    Pop $1
    StrCmp $1 "success" install_webview2
    DetailPrint "下载失败，跳过 WebView2 安装"
    Goto done

  install_webview2:
    DetailPrint "正在静默安装 WebView2 Runtime（请耐心等待）..."
    ExecWait '"$TEMP\WebView2Setup.exe" /silent /install'
    Delete "$TEMP\WebView2Setup.exe"
    DetailPrint "WebView2 Runtime 安装完成"

  done:
SectionEnd

; ---------- 卸载 ----------
Section "Uninstall"
  Delete "$INSTDIR\云驿GUI.exe"
  Delete "$INSTDIR\云驿.exe"
  Delete "$INSTDIR\WebView2Loader.dll"
  Delete "$INSTDIR\libcrypto-4-x64.dll"
  Delete "$INSTDIR\libssl-4-x64.dll"
  Delete "$INSTDIR\webview\yunyi.html"
  RMDir "$INSTDIR\webview"
  Delete "$INSTDIR\uninst.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT_NAME}\卸载 ${PRODUCT_NAME}.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT_NAME}"

  Delete "$DESKTOP\${PRODUCT_NAME}.lnk"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
SectionEnd
