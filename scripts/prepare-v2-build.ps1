$path = Join-Path $env:GITHUB_WORKSPACE 'obs-template\src\plugin-main.cpp'
if (-not (Test-Path $path)) { throw "plugin-main.cpp not found: $path" }

$text = Get-Content $path -Raw
$newline = [Environment]::NewLine

# The v2 source needs Qt TCP support, but the build environment does not ship Qt WebSockets.
if ($text -notmatch '#include <QTcpSocket>') {
    $includes = '#include <QSslSocket>' + $newline + '#include <QTcpSocket>' + $newline + '#include <cstring>'
    $text = $text.Replace('#include <QSslSocket>', $includes)
}

# Globals used by code that is declared before the UI classes.
if ($text -notmatch 'obs_source_info g_sourceInfo') {
    $text = $text.Replace('VyanDock *g_ui = nullptr;', 'VyanDock *g_ui = nullptr;' + $newline + 'obs_source_info g_sourceInfo{};')
}

# Defer UI repaint requests until VyanDock is a complete type.
$text = $text.Replace('QMetaObject::invokeMethod(g_ui, "refreshCanvas", Qt::QueuedConnection);', 'requestUiRefresh();')
if ($text -notmatch 'void requestUiRefresh\(\);') {
    $text = $text.Replace('void clearCanvasLocal()', 'void requestUiRefresh();' + $newline + $newline + 'void clearCanvasLocal()')
}

# obs_frontend_get_main_window returns void* in the OBS frontend API.
$text = $text.Replace('CanvasWindow(obs_frontend_get_main_window())', 'CanvasWindow(static_cast<QWidget *>(obs_frontend_get_main_window()))')
$text = $text.Replace('VyanDock(obs_frontend_get_main_window())', 'VyanDock(static_cast<QWidget *>(obs_frontend_get_main_window()))')

# OBS 32.2.2 uses the six-argument obs_source_draw helper.
$text = $text.Replace('obs_source_draw(g_texture, 0, kCanvasW, kCanvasH, false);', 'obs_source_draw(g_texture, 0, 0, kCanvasW, kCanvasH, false);')

# Capture QSettings safely inside the CONNECT button lambda.
$text = $text.Replace('connect(connectBtn, &QPushButton::clicked, dlg, [dlg, url, room, this]() {', 'connect(connectBtn, &QPushButton::clicked, dlg, [dlg, url, room]() {')
$text = $text.Replace('            s.setValue("relay", url->text().trimmed());', '            QSettings settings("VyanHQ", "Draw");' + $newline + '            settings.setValue("relay", url->text().trimmed());')
$text = $text.Replace('            s.setValue("room", room->text().trimmed());', '            settings.setValue("room", room->text().trimmed());')

# Insert helper after the complete VyanDock class and before source registration.
if ($text -notmatch 'void requestUiRefresh\(\)\s*\{') {
    $marker = '};' + $newline + $newline + 'static const char *sourceGetName'
    $helper = '};' + $newline + $newline + 'void requestUiRefresh()' + $newline + '{' + $newline + '    if (g_ui) QTimer::singleShot(0, [ui = g_ui]() { ui->refreshCanvas(); });' + $newline + '}' + $newline + $newline + 'static const char *sourceGetName'
    if ($text.Contains($marker)) {
        $text = $text.Replace($marker, $helper)
    } else {
        throw 'Could not locate VyanDock end marker while preparing v2 source.'
    }
}

Set-Content -Path $path -Value $text -Encoding UTF8
Write-Host 'Prepared VyanHQ v2 source with OBS 32.2.2 compile fixes.'
