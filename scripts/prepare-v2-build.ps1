$path = Join-Path $env:GITHUB_WORKSPACE 'obs-template\src\plugin-main.cpp'
if (-not (Test-Path $path)) { throw "plugin-main.cpp not found: $path" }
$text = Get-Content $path -Raw
if ($text -notmatch '#include <QTcpSocket>') { $text = $text.Replace('#include <QSslSocket>', '#include <QSslSocket>\n#include <QTcpSocket>\n#include <cstring>') }
$text = $text.Replace('        m_room = room.trimmed();\n        if (m_room.isEmpty()) {', '        m_room = room.trimmed();\n        if (m_room.isEmpty()) {')
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
Set-Content -Path $path -Value $text -Encoding UTF8
Write-Host 'Prepared VyanHQ v2 source.'
