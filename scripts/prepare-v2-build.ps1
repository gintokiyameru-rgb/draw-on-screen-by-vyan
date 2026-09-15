$path = Join-Path $env:GITHUB_WORKSPACE 'obs-template\src\plugin-main.cpp'
if (-not (Test-Path $path)) { throw "plugin-main.cpp not found: $path" }
$text = Get-Content $path -Raw
if ($text -notmatch '#include <QTcpSocket>') { $text = $text.Replace('#include <QSslSocket>', '#include <QSslSocket>\n#include <QTcpSocket>\n#include <cstring>') }
$old = @'
        m_socket = new QSslSocket(this);
'@
$new = @'
        u.setScheme(scheme);
        u.setPath(QStringLiteral("/room/") + m_room);
        m_url = u;
        m_socket = new QSslSocket(this);
'@
$text = $text.Replace($old, $new)
$text = $text.Replace('        m_url = u;\n        if (scheme == "wss") {', '        if (scheme == "wss") {')
$text = $text.Replace('QDockWidget *g_dock = nullptr;', 'bool g_dockAdded = false;')
$text = $text.Replace('    Q_UNUSED(g_dock);', '    Q_UNUSED(g_dockAdded);')
$text = $text.Replace('    g_dock = obs_frontend_add_dock_by_id("vyanhq_draw_dock", "VyanHQ Draw", g_ui);', '    g_dockAdded = obs_frontend_add_dock_by_id("vyanhq_draw_dock", "VyanHQ Draw", g_ui);')
$text = $text.Replace('    if (g_dock) g_dock = nullptr;', '    if (g_dockAdded) { obs_frontend_remove_dock("vyanhq_draw_dock"); g_dockAdded = false; }')
$text = $text.Replace('        io()->flush();', '        if (m_socket) m_socket->flush(); else if (m_plain) m_plain->flush();')
$text = $text.Replace('        io()->flush();', '        if (m_socket) m_socket->flush(); else if (m_plain) m_plain->flush();')
$text = $text.Replace('    QIODevice *io() const { return m_socket ? static_cast<QIODevice *>(m_socket) : static_cast<QIODevice *>(m_plain); }', '    QIODevice *io() const { return m_socket ? static_cast<QIODevice *>(m_socket) : static_cast<QIODevice *>(m_plain); }')
Set-Content -Path $path -Value $text -Encoding UTF8
Write-Host 'Prepared VyanHQ v2 source.'
