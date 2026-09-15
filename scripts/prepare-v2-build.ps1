$path = Join-Path $env:GITHUB_WORKSPACE 'obs-template\src\plugin-main.cpp'
if (-not (Test-Path $path)) { throw "plugin-main.cpp not found: $path" }

$text = Get-Content $path -Raw
$newline = [Environment]::NewLine

if ($text -notmatch '#include <QTcpSocket>') {
    $includes = '#include <QSslSocket>' + $newline + '#include <QTcpSocket>' + $newline + '#include <cstring>'
    $text = $text.Replace('#include <QSslSocket>', $includes)
}

if ($text -notmatch 'obs_source_info g_sourceInfo') {
    $text = $text.Replace('VyanDock *g_ui = nullptr;', 'VyanDock *g_ui = nullptr;' + $newline + 'obs_source_info g_sourceInfo{};')
}

$text = $text.Replace('QMetaObject::invokeMethod(g_ui, "refreshCanvas", Qt::QueuedConnection);', 'requestUiRefresh();')
if ($text -notmatch 'void requestUiRefresh\(\);') {
    $text = $text.Replace('void clearCanvasLocal()', 'void requestUiRefresh();' + $newline + $newline + 'void clearCanvasLocal()')
}

$text = $text.Replace('CanvasWindow(obs_frontend_get_main_window())', 'CanvasWindow(static_cast<QWidget *>(obs_frontend_get_main_window()))')
$text = $text.Replace('VyanDock(obs_frontend_get_main_window())', 'VyanDock(static_cast<QWidget *>(obs_frontend_get_main_window()))')
$text = $text.Replace('obs_source_draw(g_texture, 0, kCanvasW, kCanvasH, false);', 'obs_source_draw(g_texture, 0, 0, kCanvasW, kCanvasH, false);')

$text = $text.Replace('connect(connectBtn, &QPushButton::clicked, dlg, [dlg, url, room, this]() {', 'connect(connectBtn, &QPushButton::clicked, dlg, [dlg, url, room]() {')
$text = $text.Replace('            s.setValue("relay", url->text().trimmed());', '            QSettings settings("VyanHQ", "Draw");' + $newline + '            settings.setValue("relay", url->text().trimmed());')
$text = $text.Replace('            s.setValue("room", room->text().trimmed());', '            settings.setValue("room", room->text().trimmed());')

# QIODevice has no flush(); flush the concrete Qt socket types.
$text = $text.Replace('        io()->flush();', '        if (m_socket) m_socket->flush(); else if (m_plain) m_plain->flush();')

# obs_frontend_add_dock_by_id returns bool on OBS 32.2.2.
$text = $text.Replace('QDockWidget *g_dock = nullptr;', 'bool g_dock = false;')
$text = $text.Replace('    if (g_dock) g_dock = nullptr;', '    g_dock = false;')

# Insert helper after the VyanDock class, immediately before source registration.
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
Write-Host 'Prepared VyanHQ v2 source with remaining OBS 32.2.2 compile fixes.'
