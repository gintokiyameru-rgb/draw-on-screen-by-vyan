#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <graphics/graphics.h>

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QColorDialog>
#include <QCryptographicHash>
#include <QDialog>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAbstractSocket>
#include <QSslSocket>

#include <algorithm>
#include <cmath>
#include <cstdint>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("vyanhq-draw", "en-US")

namespace {
constexpr int kCanvasW = 1920;
constexpr int kCanvasH = 1080;
constexpr const char *kDefaultRelay = "wss://YOUR-WORKER.workers.dev";

QDockWidget *g_dock = nullptr;
class VyanDock;
VyanDock *g_ui = nullptr;
QDialog *g_canvasWindow = nullptr;
obs_hotkey_id g_clearHotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id g_toggleHotkey = OBS_INVALID_HOTKEY_ID;

QMutex g_docMutex;
QImage g_canvas(kCanvasW, kCanvasH, QImage::Format_RGBA8888);
uint64_t g_revision = 1;
bool g_visible = true;
QColor g_color(255, 255, 255, 255);
int g_brush = 8;
int g_eraser = 32;

gs_texture_t *g_texture = nullptr;
uint64_t g_textureRevision = 0;

enum class Tool { Pen, Eraser, Line, Rect, Ellipse, Arrow, Text };

QString toolName(Tool t)
{
    switch (t) {
    case Tool::Pen: return "pen";
    case Tool::Eraser: return "eraser";
    case Tool::Line: return "line";
    case Tool::Rect: return "rect";
    case Tool::Ellipse: return "circle";
    case Tool::Arrow: return "arrow";
    case Tool::Text: return "text";
    }
    return "pen";
}

Tool toolFromName(const QString &s)
{
    if (s == "eraser") return Tool::Eraser;
    if (s == "line") return Tool::Line;
    if (s == "rect") return Tool::Rect;
    if (s == "circle") return Tool::Ellipse;
    if (s == "arrow") return Tool::Arrow;
    if (s == "text") return Tool::Text;
    return Tool::Pen;
}

QRectF canvasRect(const QWidget *w)
{
    if (!w || w->width() <= 0 || w->height() <= 0) return {};
    const double sx = double(w->width()) / kCanvasW;
    const double sy = double(w->height()) / kCanvasH;
    const double s = qMin(sx, sy);
    const double ww = kCanvasW * s;
    const double hh = kCanvasH * s;
    return QRectF((w->width() - ww) * 0.5, (w->height() - hh) * 0.5, ww, hh);
}

QPoint canvasPoint(const QWidget *w, const QPointF &p)
{
    const QRectF r = canvasRect(w);
    if (r.isEmpty()) return {};
    return QPoint(qBound(0, qRound((p.x() - r.left()) / r.width() * (kCanvasW - 1)), kCanvasW - 1),
                  qBound(0, qRound((p.y() - r.top()) / r.height() * (kCanvasH - 1)), kCanvasH - 1));
}

QPen drawPen(bool erase)
{
    return QPen(erase ? QColor(Qt::transparent) : g_color,
                erase ? g_eraser : g_brush,
                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

void touch()
{
    ++g_revision;
}

void ensureCanvas()
{
    if (g_canvas.isNull()) {
        g_canvas = QImage(kCanvasW, kCanvasH, QImage::Format_RGBA8888);
        g_canvas.fill(Qt::transparent);
    }
}

void drawFreehand(const QPoint &a, const QPoint &b, bool erase)
{
    QMutexLocker lock(&g_docMutex);
    ensureCanvas();
    QPainter p(&g_canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(erase ? QPainter::CompositionMode_Clear : QPainter::CompositionMode_SourceOver);
    p.setPen(drawPen(erase));
    p.drawLine(a, b);
    touch();
}

void drawShape(const QPoint &a, const QPoint &b, Tool tool)
{
    QMutexLocker lock(&g_docMutex);
    ensureCanvas();
    QPainter p(&g_canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(drawPen(false));
    p.setBrush(Qt::NoBrush);
    const QRectF rect(QPointF(qMin(a.x(), b.x()), qMin(a.y(), b.y())),
                      QPointF(qMax(a.x(), b.x()), qMax(a.y(), b.y())));
    if (tool == Tool::Line) p.drawLine(a, b);
    else if (tool == Tool::Rect) p.drawRect(rect);
    else if (tool == Tool::Ellipse) p.drawEllipse(rect);
    else if (tool == Tool::Arrow) {
        p.drawLine(a, b);
        const QLineF line(a, b);
        const double angle = std::atan2(-line.dy(), line.dx());
        const double size = qMax(12.0, double(g_brush) * 2.5);
        const QPointF end = line.p2();
        const QPointF p1 = end - QPointF(std::cos(angle + M_PI / 6.0) * size, -std::sin(angle + M_PI / 6.0) * size);
        const QPointF p2 = end - QPointF(std::cos(angle - M_PI / 6.0) * size, -std::sin(angle - M_PI / 6.0) * size);
        p.drawLine(end, p1);
        p.drawLine(end, p2);
    }
    touch();
}

void drawText(const QPoint &pt, const QString &text)
{
    QMutexLocker lock(&g_docMutex);
    ensureCanvas();
    QPainter p(&g_canvas);
    QFont f;
    f.setPointSize(qMax(14, g_brush * 2));
    p.setFont(f);
    p.setPen(g_color);
    p.drawText(pt, text);
    touch();
}

void clearCanvasLocal()
{
    QMutexLocker lock(&g_docMutex);
    ensureCanvas();
    g_canvas.fill(Qt::transparent);
    touch();
}

QString colorHex()
{
    return g_color.name(QColor::HexArgb);
}

void applyRemoteObject(const QJsonObject &o)
{
    const QString type = o.value("type").toString();
    if (type == "clear") {
        clearCanvasLocal();
        if (g_ui) QMetaObject::invokeMethod(g_ui, "refreshCanvas", Qt::QueuedConnection);
        return;
    }
    if (type != "draw") return;
    const Tool t = toolFromName(o.value("tool").toString());
    const QPoint a(o.value("x1").toInt(), o.value("y1").toInt());
    const QPoint b(o.value("x2").toInt(), o.value("y2").toInt());
    const QColor c(o.value("color").toString());
    const QColor old = g_color;
    const int oldBrush = g_brush;
    const int oldErase = g_eraser;
    g_color = c.isValid() ? c : g_color;
    g_brush = qMax(1, o.value("size").toInt(g_brush));
    g_eraser = g_brush;
    if (t == Tool::Pen || t == Tool::Eraser) drawFreehand(a, b, t == Tool::Eraser);
    else if (t == Tool::Text) drawText(a, o.value("text").toString());
    else drawShape(a, b, t);
    g_color = old;
    g_brush = oldBrush;
    g_eraser = oldErase;
    if (g_ui) QMetaObject::invokeMethod(g_ui, "refreshCanvas", Qt::QueuedConnection);
}

class RelayClient final : public QObject
{
    Q_OBJECT
public:
    explicit RelayClient(QObject *parent = nullptr) : QObject(parent)
    {
        m_clientId = QString::number(QRandomGenerator::global()->generate64(), 16);
    }

    bool connected() const { return m_open; }
    QString relayUrl() const { return m_baseUrl; }
    QString room() const { return m_room; }
    QString memberLink() const
    {
        if (m_baseUrl.isEmpty() || m_room.isEmpty()) return {};
        QUrl u(m_baseUrl);
        u.setScheme(u.scheme() == "wss" || u.scheme() == "https" ? "https" : "http");
        u.setPath("/room/" + m_room);
        return u.toString();
    }

public slots:
    void connectRoom(const QString &base, const QString &room)
    {
        disconnectRoom();
        QUrl u(base.trimmed());
        if (!u.isValid() || u.host().isEmpty()) {
            emit status("Invalid server URL");
            return;
        }
        QString scheme = u.scheme().toLower();
        if (scheme == "https") scheme = "wss";
        if (scheme == "http") scheme = "ws";
        if (scheme != "wss" && scheme != "ws") {
            emit status("Use https:// or wss:// relay URL");
            return;
        }
        m_baseUrl = u.toString(QUrl::FullyEncoded);
        m_baseUrl = m_baseUrl.left(m_baseUrl.indexOf("://") + 3) + u.host(QUrl::FullyEncoded);
        if (!u.port(defaultPortFor(scheme)) && u.port() == -1) {
            // default port is applied by the socket below
        }
        m_room = room.trimmed();
        if (m_room.isEmpty()) {
            emit status("Room name is empty");
            return;
        }
        m_socket = new QSslSocket(this);
        connect(m_socket, &QAbstractSocket::disconnected, this, &RelayClient::onDisconnected);
        connect(m_socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            emit status(m_socket ? m_socket->errorString() : QStringLiteral("Socket error"));
        });
        connect(m_socket, &QSslSocket::encrypted, this, &RelayClient::onEncrypted);
        connect(m_socket, &QIODevice::readyRead, this, &RelayClient::onReadyRead);
        m_url = u;
        if (scheme == "wss") {
            emit status("Connecting…");
            m_socket->connectToHostEncrypted(u.host(), u.port(443));
        } else {
            delete m_socket;
            m_socket = nullptr;
            m_plain = new QTcpSocket(this);
            connect(m_plain, &QAbstractSocket::disconnected, this, &RelayClient::onDisconnected);
            connect(m_plain, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
                emit status(m_plain ? m_plain->errorString() : QStringLiteral("Socket error"));
            });
            connect(m_plain, &QAbstractSocket::connected, this, &RelayClient::onConnected);
            connect(m_plain, &QIODevice::readyRead, this, &RelayClient::onReadyRead);
            emit status("Connecting…");
            m_plain->connectToHost(u.host(), u.port(80));
        }
    }

    void disconnectRoom()
    {
        m_open = false;
        m_handshake = false;
        m_rx.clear();
        if (m_socket) { m_socket->abort(); m_socket->deleteLater(); m_socket = nullptr; }
        if (m_plain) { m_plain->abort(); m_plain->deleteLater(); m_plain = nullptr; }
        if (!m_room.isEmpty()) emit status("Disconnected");
    }

    void sendDraw(Tool tool, const QPoint &a, const QPoint &b, const QString &text = QString())
    {
        if (!m_open) return;
        QJsonObject o{{"type", "draw"}, {"tool", toolName(tool)}, {"x1", a.x()}, {"y1", a.y()},
                      {"x2", b.x()}, {"y2", b.y()}, {"color", colorHex()}, {"size", tool == Tool::Eraser ? g_eraser : g_brush}};
        if (!text.isEmpty()) o.insert("text", text);
        sendText(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    }

    void sendClear()
    {
        if (!m_open) return;
        sendText(QStringLiteral("{\"type\":\"clear\"}"));
    }

signals:
    void status(const QString &text);
    void remoteJson(const QJsonObject &object);
    void connectedChanged(bool online);

private:
    static quint16 defaultPortFor(const QString &scheme) { return scheme == "wss" ? 443 : 80; }
    QIODevice *io() const { return m_socket ? static_cast<QIODevice *>(m_socket) : static_cast<QIODevice *>(m_plain); }

    void onConnected() { beginHandshake(); }
    void onEncrypted() { beginHandshake(); }

    void beginHandshake()
    {
        if (!io() || m_handshake) return;
        m_handshake = true;
        QByteArray raw;
        raw.resize(16);
        for (int i = 0; i < raw.size(); i += 4) {
            quint32 r = QRandomGenerator::global()->generate();
            std::memcpy(raw.data() + i, &r, qMin(4, raw.size() - i));
        }
        m_wsKey = raw.toBase64();
        QString path = m_url.path(QUrl::FullyEncoded);
        if (path.isEmpty()) path = "/";
        if (!m_url.query(QUrl::FullyEncoded).isEmpty()) path += "?" + m_url.query(QUrl::FullyEncoded);
        const quint16 p = m_url.port(defaultPortFor(m_url.scheme()));
        QString host = m_url.host(QUrl::FullyEncoded);
        if ((m_url.scheme() == "wss" && p != 443) || (m_url.scheme() == "ws" && p != 80)) host += ":" + QString::number(p);
        const QByteArray request = QString("GET %1 HTTP/1.1\r\nHost: %2\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %3\r\nSec-WebSocket-Version: 13\r\nOrigin: https://vyanhq.local\r\n\r\n").arg(path, host, QString::fromLatin1(m_wsKey)).toUtf8();
        io()->write(request);
        io()->flush();
    }

    void onReadyRead()
    {
        if (!io()) return;
        m_rx.append(io()->readAll());
        if (!m_open) {
            const int headerEnd = m_rx.indexOf("\r\n\r\n");
            if (headerEnd < 0) return;
            const QByteArray header = m_rx.left(headerEnd);
            m_rx.remove(0, headerEnd + 4);
            if (!header.startsWith("HTTP/1.1 101")) {
                emit status("Relay handshake failed");
                disconnectRoom();
                return;
            }
            m_open = true;
            emit status("Connected");
            emit connectedChanged(true);
            QJsonObject join{{"type", "join"}, {"room", m_room}, {"client", m_clientId}};
            sendText(QString::fromUtf8(QJsonDocument(join).toJson(QJsonDocument::Compact)));
        }
        parseFrames();
    }

    void parseFrames()
    {
        while (m_rx.size() >= 2) {
            const quint8 b1 = quint8(m_rx[0]);
            const quint8 b2 = quint8(m_rx[1]);
            const bool masked = (b2 & 0x80) != 0;
            quint64 len = b2 & 0x7f;
            int pos = 2;
            if (len == 126) {
                if (m_rx.size() < pos + 2) return;
                len = (quint64(quint8(m_rx[pos])) << 8) | quint64(quint8(m_rx[pos + 1]));
                pos += 2;
            } else if (len == 127) {
                if (m_rx.size() < pos + 8) return;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | quint64(quint8(m_rx[pos + i]));
                pos += 8;
            }
            if (len > 32ULL * 1024ULL * 1024ULL) { disconnectRoom(); return; }
            if (masked) pos += 4;
            if (m_rx.size() < pos + qint64(len)) return;
            QByteArray payload = m_rx.mid(masked ? pos + 4 : pos, qint64(len));
            if (masked) {
                const QByteArray mask = m_rx.mid(pos - 4, 4);
                for (qint64 i = 0; i < payload.size(); ++i) payload[i] = payload[i] ^ mask[i % 4];
            }
            m_rx.remove(0, pos + qint64(len));
            const quint8 opcode = b1 & 0x0f;
            if (opcode == 0x8) { disconnectRoom(); return; }
            if (opcode == 0x9) { sendFrame(0xA, payload); continue; }
            if (opcode != 0x1) continue;
            QJsonParseError err{};
            const QJsonDocument d = QJsonDocument::fromJson(payload, &err);
            if (!d.isObject()) continue;
            const QJsonObject o = d.object();
            if (o.value("type").toString() == "ops") {
                const QJsonArray ops = o.value("ops").toArray();
                for (const QJsonValue &v : ops) if (v.isObject()) emit remoteJson(v.toObject());
            } else {
                emit remoteJson(o);
            }
        }
    }

    void sendText(const QString &text) { sendFrame(0x1, text.toUtf8()); }

    void sendFrame(quint8 opcode, const QByteArray &payload)
    {
        if (!io()) return;
        QByteArray frame;
        frame.append(char(0x80 | (opcode & 0x0f)));
        const quint64 len = quint64(payload.size());
        if (len < 126) frame.append(char(0x80 | quint8(len)));
        else if (len <= 0xffff) {
            frame.append(char(0x80 | 126));
            frame.append(char((len >> 8) & 0xff));
            frame.append(char(len & 0xff));
        } else {
            frame.append(char(0x80 | 127));
            for (int i = 7; i >= 0; --i) frame.append(char((len >> (8 * i)) & 0xff));
        }
        QByteArray mask(4, Qt::Uninitialized);
        quint32 r = QRandomGenerator::global()->generate();
        std::memcpy(mask.data(), &r, 4);
        frame.append(mask);
        for (qint64 i = 0; i < payload.size(); ++i) frame.append(char(quint8(payload[i]) ^ quint8(mask[i % 4])));
        io()->write(frame);
        io()->flush();
    }

    void onDisconnected()
    {
        const bool wasOpen = m_open;
        m_open = false;
        m_handshake = false;
        if (wasOpen) emit connectedChanged(false);
        emit status("Disconnected");
    }

    QSslSocket *m_socket = nullptr;
    QTcpSocket *m_plain = nullptr;
    QUrl m_url;
    QString m_baseUrl;
    QString m_room;
    QString m_clientId;
    QByteArray m_rx;
    QByteArray m_wsKey;
    bool m_handshake = false;
    bool m_open = false;
};

RelayClient *g_relay = nullptr;

class CanvasWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit CanvasWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMouseTracking(true);
        setMinimumSize(720, 420);
        setCursor(Qt::CrossCursor);
    }
    void setTool(Tool t) { m_tool = t; setCursor(t == Tool::Text ? Qt::IBeamCursor : Qt::CrossCursor); update(); }
    Tool tool() const { return m_tool; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(24, 24, 28));
        const QRectF dst = canvasRect(this);
        p.fillRect(dst, Qt::black);
        QImage img;
        { QMutexLocker lock(&g_docMutex); ensureCanvas(); img = g_canvas; }
        p.drawImage(dst, img);
        if (m_drawing && m_tool != Tool::Pen && m_tool != Tool::Eraser && m_tool != Tool::Text) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(drawPen(false));
            const QPointF a = dst.topLeft() + QPointF(double(m_start.x()) / kCanvasW * dst.width(), double(m_start.y()) / kCanvasH * dst.height());
            const QPointF b = dst.topLeft() + QPointF(double(m_current.x()) / kCanvasW * dst.width(), double(m_current.y()) / kCanvasH * dst.height());
            const QRectF rr(QPointF(qMin(a.x(), b.x()), qMin(a.y(), b.y())), QPointF(qMax(a.x(), b.x()), qMax(a.y(), b.y())));
            if (m_tool == Tool::Line) p.drawLine(a, b);
            else if (m_tool == Tool::Rect) p.drawRect(rr);
            else if (m_tool == Tool::Ellipse) p.drawEllipse(rr);
            else if (m_tool == Tool::Arrow) {
                p.drawLine(a, b);
                const QLineF line(a, b);
                const double angle = std::atan2(-line.dy(), line.dx());
                const double size = qMax(10.0, double(g_brush) * 2.0 * dst.width() / kCanvasW);
                const QPointF end = line.p2();
                p.drawLine(end, end - QPointF(std::cos(angle + M_PI / 6.0) * size, -std::sin(angle + M_PI / 6.0) * size));
                p.drawLine(end, end - QPointF(std::cos(angle - M_PI / 6.0) * size, -std::sin(angle - M_PI / 6.0) * size));
            }
        }
        p.setPen(QPen(QColor(105,105,115), 1));
        p.drawRect(dst);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPoint p = canvasPoint(this, e->position());
        m_start = m_current = p;
        m_drawing = true;
        if (m_tool == Tool::Text) {
            m_drawing = false;
            bool ok = false;
            const QString text = QInputDialog::getText(this, "Insert Text", "Text:", QLineEdit::Normal, {}, &ok);
            if (ok && !text.isEmpty()) { drawText(p, text); if (g_relay) g_relay->sendDraw(Tool::Text, p, p, text); update(); }
            return;
        }
        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) { drawFreehand(p, p, m_tool == Tool::Eraser); if (g_relay) g_relay->sendDraw(m_tool, p, p); }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!m_drawing) return;
        const QPoint p = canvasPoint(this, e->position());
        m_current = p;
        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) {
            drawFreehand(m_last, p, m_tool == Tool::Eraser);
            if (g_relay) g_relay->sendDraw(m_tool, m_last, p);
            m_last = p;
        }
        update();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !m_drawing) return;
        const QPoint p = canvasPoint(this, e->position());
        m_current = p;
        if (m_tool != Tool::Pen && m_tool != Tool::Eraser && m_tool != Tool::Text) {
            drawShape(m_start, p, m_tool);
            if (g_relay) g_relay->sendDraw(m_tool, m_start, p);
        }
        m_drawing = false;
        update();
    }

private:
    Tool m_tool = Tool::Pen;
    QPoint m_start;
    QPoint m_last;
    QPoint m_current;
    bool m_drawing = false;
};

class CanvasWindow final : public QDialog
{
    Q_OBJECT
public:
    explicit CanvasWindow(QWidget *parent = nullptr) : QDialog(parent)
    {
        setWindowTitle("VyanHQ Draw — Private Canvas");
        resize(1180, 760);
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(8,8,8,8);
        root->setSpacing(6);

        auto *tools = new QHBoxLayout();
        m_group = new QButtonGroup(this);
        m_group->setExclusive(true);
        const QList<QPair<QString, Tool>> list = {
            {"Pen", Tool::Pen}, {"Eraser", Tool::Eraser}, {"Line", Tool::Line},
            {"Rect", Tool::Rect}, {"Circle", Tool::Ellipse}, {"Arrow", Tool::Arrow}, {"Text", Tool::Text}
        };
        for (const auto &it : list) {
            auto *b = new QPushButton(it.first);
            b->setCheckable(true);
            b->setMinimumHeight(32);
            b->setProperty("toolButton", true);
            m_group->addButton(b, int(it.second));
            tools->addWidget(b, 1);
            if (it.second == Tool::Pen) b->setChecked(true);
        }
        connect(m_group, &QButtonGroup::idClicked, this, [this](int id){
            m_canvas->setTool(static_cast<Tool>(id));
        });

        auto *color = new QPushButton("Color");
        connect(color, &QPushButton::clicked, this, [this]() {
            const QColor c = QColorDialog::getColor(g_color, this, "Draw Color");
            if (c.isValid()) g_color = c;
        });
        tools->addWidget(color);

        auto *clear = new QPushButton("Clear");
        connect(clear, &QPushButton::clicked, this, [](){ clearCanvasLocal(); if (g_relay) g_relay->sendClear(); if (g_ui) g_ui->refreshCanvas(); });
        tools->addWidget(clear);
        root->addLayout(tools);

        m_canvas = new CanvasWidget(this);
        root->addWidget(m_canvas, 1);

        auto *sl = new QHBoxLayout();
        auto *brushLabel = new QLabel("Brush");
        auto *brush = new QSlider(Qt::Horizontal);
        brush->setRange(1, 64); brush->setValue(g_brush);
        connect(brush, &QSlider::valueChanged, [](int v){ g_brush = v; });
        sl->addWidget(brushLabel); sl->addWidget(brush, 1);
        auto *eraserLabel = new QLabel("Eraser");
        auto *eraser = new QSlider(Qt::Horizontal);
        eraser->setRange(4, 128); eraser->setValue(g_eraser);
        connect(eraser, &QSlider::valueChanged, [](int v){ g_eraser = v; });
        sl->addWidget(eraserLabel); sl->addWidget(eraser, 1);
        root->addLayout(sl);

        setStyleSheet("QPushButton[toolButton=\"true\"]:checked{background:#6d4aff;color:white;border:1px solid #9a84ff;} QDialog{background:#1a1a1e;} QLabel{color:#ddd;}");
    }
    CanvasWidget *canvas() const { return m_canvas; }
private:
    CanvasWidget *m_canvas = nullptr;
    QButtonGroup *m_group = nullptr;
};

class VyanDock final : public QWidget
{
    Q_OBJECT
public:
    explicit VyanDock(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumWidth(250);
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(10,10,10,10);
        root->setSpacing(7);
        auto *title = new QLabel("VyanHQ Draw");
        title->setStyleSheet("font-weight:700;font-size:16px;");
        root->addWidget(title);
        m_status = new QLabel("● Disconnected");
        m_status->setStyleSheet("color:#9a9aa3;");
        root->addWidget(m_status);

        auto *row1 = new QHBoxLayout();
        auto *draw = new QPushButton("DRAW");
        draw->setMinimumHeight(34);
        connect(draw, &QPushButton::clicked, this, &VyanDock::openDraw);
        row1->addWidget(draw, 1);
        auto *copy = new QPushButton("◯");
        copy->setToolTip("Copy member link");
        copy->setFixedWidth(38);
        connect(copy, &QPushButton::clicked, this, &VyanDock::copyMemberLink);
        row1->addWidget(copy);
        root->addLayout(row1);

        auto *row2 = new QHBoxLayout();
        auto *connectBtn = new QPushButton("CONNECT");
        connectBtn->setMinimumHeight(34);
        connect(connectBtn, &QPushButton::clicked, this, &VyanDock::openConnect);
        row2->addWidget(connectBtn, 1);
        auto *forget = new QPushButton("FORGET");
        connect(forget, &QPushButton::clicked, this, &VyanDock::forget);
        row2->addWidget(forget, 1);
        root->addLayout(row2);
        root->addStretch();
    }

public slots:
    void refreshCanvas() { if (g_canvasWindow) qobject_cast<CanvasWindow*>(g_canvasWindow)->canvas()->update(); }

private slots:
    void openDraw()
    {
        if (!g_canvasWindow) {
            g_canvasWindow = new CanvasWindow(obs_frontend_get_main_window());
            g_canvasWindow->setAttribute(Qt::WA_DeleteOnClose, true);
            connect(g_canvasWindow, &QObject::destroyed, [](){ g_canvasWindow = nullptr; });
        }
        g_canvasWindow->show();
        g_canvasWindow->raise();
        g_canvasWindow->activateWindow();
    }

    void openConnect()
    {
        QSettings s("VyanHQ", "Draw");
        auto *dlg = new QDialog(this);
        dlg->setWindowTitle("VyanHQ Draw — Connect Room");
        dlg->setModal(true);
        dlg->resize(520, 220);
        auto *root = new QVBoxLayout(dlg);
        auto *url = new QLineEdit(s.value("relay", QString::fromLatin1(kDefaultRelay)).toString());
        auto *room = new QLineEdit(s.value("room", QString()).toString());
        if (room->text().isEmpty()) room->setText(QString::number(QRandomGenerator::global()->generate() % 900000 + 100000));
        root->addWidget(new QLabel("Cloudflare relay URL"));
        root->addWidget(url);
        root->addWidget(new QLabel("Room name"));
        root->addWidget(room);
        auto *buttons = new QHBoxLayout();
        auto *connectBtn = new QPushButton("Connect");
        auto *cancel = new QPushButton("Cancel");
        buttons->addWidget(connectBtn); buttons->addWidget(cancel);
        root->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);
        connect(connectBtn, &QPushButton::clicked, dlg, [dlg, url, room, this]() {
            s.setValue("relay", url->text().trimmed());
            s.setValue("room", room->text().trimmed());
            g_relay->connectRoom(url->text(), room->text());
            dlg->accept();
        });
        dlg->exec();
        dlg->deleteLater();
    }

    void copyMemberLink()
    {
        if (!g_relay || g_relay->room().isEmpty()) return;
        QApplication::clipboard()->setText(g_relay->memberLink());
        m_status->setText("● Member link copied");
    }

    void forget()
    {
        if (g_relay) g_relay->disconnectRoom();
        QSettings s("VyanHQ", "Draw");
        s.remove("relay"); s.remove("room");
        m_status->setText("● Disconnected");
    }

public:
    void setStatus(const QString &s) { m_status->setText("● " + s); }
private:
    QLabel *m_status = nullptr;
};

static const char *sourceGetName(void *) { return "VyanHQ Draw"; }
struct SourceState { obs_source_t *source = nullptr; };
static void *sourceCreate(obs_data_t *, obs_source_t *source) { ensureCanvas(); auto *s = new SourceState; s->source = source; return s; }
static void sourceDestroy(void *d) { delete static_cast<SourceState *>(d); }
static uint32_t sourceWidth(void *) { return kCanvasW; }
static uint32_t sourceHeight(void *) { return kCanvasH; }
static obs_properties_t *sourceProperties(void *) {
    auto *p = obs_properties_create();
    obs_properties_add_text(p, "info", "Native transparent 1920×1080 VyanHQ Draw canvas.", OBS_TEXT_INFO);
    return p;
}
static void sourceVideoRender(void *data, gs_effect_t *effect)
{
    Q_UNUSED(data); Q_UNUSED(effect);
    if (!g_visible) return;
    QImage composite; uint64_t rev;
    { QMutexLocker lock(&g_docMutex); ensureCanvas(); composite = g_canvas; rev = g_revision; }
    if (!g_texture) {
        g_texture = gs_texture_create(kCanvasW, kCanvasH, GS_RGBA, 1, nullptr, GS_DYNAMIC);
        if (!g_texture) return;
        g_textureRevision = 0;
    }
    if (rev != g_textureRevision) {
        QImage rgba = composite.convertToFormat(QImage::Format_RGBA8888);
        gs_texture_set_image(g_texture, rgba.constBits(), uint32_t(rgba.bytesPerLine()), false);
        g_textureRevision = rev;
    }
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    obs_source_draw(g_texture, 0, kCanvasW, kCanvasH, false);
    gs_blend_state_pop();
}
static obs_source_info makeSourceInfo()
{
    obs_source_info i{};
    i.id = "vyanhq_draw";
    i.type = OBS_SOURCE_TYPE_INPUT;
    i.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    i.get_name = sourceGetName;
    i.create = sourceCreate;
    i.destroy = sourceDestroy;
    i.get_width = sourceWidth;
    i.get_height = sourceHeight;
    i.get_properties = sourceProperties;
    i.video_render = sourceVideoRender;
    return i;
}

void clearHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed) return;
    clearCanvasLocal();
    if (g_relay) g_relay->sendClear();
    if (g_ui) QMetaObject::invokeMethod(g_ui, "refreshCanvas", Qt::QueuedConnection);
}

void toggleHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed) return;
    g_visible = !g_visible;
}

} // namespace

bool obs_module_load(void)
{
    g_canvas.fill(Qt::transparent);
    g_sourceInfo = makeSourceInfo();
    obs_register_source(&g_sourceInfo);

    g_relay = new RelayClient(qApp);
    g_ui = new VyanDock(obs_frontend_get_main_window());
    g_dock = obs_frontend_add_dock_by_id("vyanhq_draw_dock", "VyanHQ Draw", g_ui);
    Q_UNUSED(g_dock);
    QObject::connect(g_relay, &RelayClient::status, g_ui, &VyanDock::setStatus);
    QObject::connect(g_relay, &RelayClient::remoteJson, [](const QJsonObject &o){ applyRemoteObject(o); });
    QObject::connect(g_relay, &RelayClient::connectedChanged, [](bool online){ if (g_ui) g_ui->setStatus(online ? "Connected / Room Active" : "Disconnected"); });

    g_clearHotkey = obs_hotkey_register_frontend("vyanhq_draw.clear", "VyanHQ Draw: Clear Canvas", clearHotkey, nullptr);
    g_toggleHotkey = obs_hotkey_register_frontend("vyanhq_draw.toggle", "VyanHQ Draw: Toggle Canvas", toggleHotkey, nullptr);
    blog(LOG_INFO, "[VyanHQ Draw] loaded v2 coop/ui");
    return true;
}

void obs_module_unload(void)
{
    if (g_relay) { g_relay->disconnectRoom(); g_relay->deleteLater(); g_relay = nullptr; }
    if (g_dock) g_dock = nullptr;
    g_ui = nullptr;
}

#include "plugin-main.moc"
