$path = Join-Path $env:GITHUB_WORKSPACE 'obs-template\src\plugin-main.cpp'
if (-not (Test-Path $path)) { throw "plugin-main.cpp not found: $path" }
$text = Get-Content $path -Raw
$newline = [Environment]::NewLine

if ($text -notmatch '#include <QTcpSocket>') {
  $text = $text.Replace('#include <QSslSocket>', "#include <QSslSocket>$newline#include <QTcpSocket>$newline#include <cstring>")
}

if ($text -notmatch 'obs_source_info g_sourceInfo') {
  $text = $text.Replace('VyanDock *g_ui = nullptr;', "VyanDock *g_ui = nullptr;$newline`nobs_source_info g_sourceInfo{};")
}

if ($text -notmatch 'void requestUiRefresh\(\);') {
  $text = $text.Replace('void clearCanvasLocal()\n{', "void requestUiRefresh();$newline`n$newlinevoid clearCanvasLocal()$newline{")
}

$text = $text.Replace('QMetaObject::invokeMethod(g_ui, "refreshCanvas", Qt::QueuedConnection);', 'requestUiRefresh();')
$text = $text.Replace('CanvasWindow(obs_frontend_get_main_window())', 'CanvasWindow(static_cast<QWidget *>(obs_frontend_get_main_window()))')
$text = $text.Replace('VyanDock(obs_frontend_get_main_window())', 'VyanDock(static_cast<QWidget *>(obs_frontend_get_main_window()))')
$text = $text.Replace('obs_source_draw(g_texture, 0, kCanvasW, kCanvasH, false);', 'obs_source_draw(g_texture, 0, 0, kCanvasW, kCanvasH, false);')

$text = $text.Replace('connect(connectBtn, &QPushButton::clicked, dlg, [dlg, url, room, this]() {', 'connect(connectBtn, &QPushButton::clicked, dlg, [dlg, url, room]() {')
$text = $text.Replace('            s.setValue("relay", url->text().trimmed());', '            QSettings settings("VyanHQ", "Draw");' + $newline + '            settings.setValue("relay", url->text().trimmed());')
$text = $text.Replace('            s.setValue("room", room->text().trimmed());', '            settings.setValue("room", room->text().trimmed());')

if ($text -notmatch 'void requestUiRefresh\(\)\s*\{') {
  $marker = "public:`n    void setStatus(const QString &s) { m_status->setText(\"● \" + s); }"
  $insert = $marker + "$newline}`n`nvoid requestUiRefresh()`n{`n    if (g_ui) QTimer::singleShot(0, [ui = g_ui]() { if (ui) ui->refreshCanvas(); });`n"
  $text = $text.Replace($marker, $insert)
}

Set-Content -Path $path -Value $text -Encoding UTF8
Write-Host 'Prepared VyanHQ v2 source with OBS 32.2.2 compile fixes.'
