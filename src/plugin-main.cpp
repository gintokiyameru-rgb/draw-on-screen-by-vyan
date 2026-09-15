#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <graphics/graphics.h>

#include <QApplication>
#include <QColorDialog>
#include <QClipboard>
#include <QDialog>
#include <QDockWidget>
#include <QFontDialog>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSizePolicy>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QList>
#include <QPair>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("vyanhq-draw", "en-US")

namespace {
constexpr int kCanvasW = 1920;
constexpr int kCanvasH = 1080;
constexpr int kMaxUndo = 40;

QDockWidget *g_dock = nullptr;
class VyanDock;
VyanDock *g_ui = nullptr;
obs_source_info g_sourceInfo{};
obs_hotkey_id g_clearMine = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id g_toggleCanvas = OBS_INVALID_HOTKEY_ID;

QMutex g_docMutex;

enum class Tool { Pen, Eraser, Line, Rect, Ellipse, Arrow, Text };

struct Layer {
    QString name;
    QImage image;
};

struct Snapshot {
    std::vector<Layer> layers;
    int active = 0;
};

std::vector<Layer> g_layers;
int g_activeLayer = 0;
std::vector<Snapshot> g_undo;
std::vector<Snapshot> g_redo;
uint64_t g_revision = 1;

QColor g_color(255, 255, 255, 255);
int g_brush = 8;
int g_eraser = 32;
bool g_visible = true;

// OBS GPU texture for the native source.
gs_texture_t *g_texture = nullptr;
uint64_t g_textureRevision = 0;

QImage compositeDocument()
{
    QImage result(kCanvasW, kCanvasH, QImage::Format_RGBA8888);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (const Layer &layer : g_layers)
        p.drawImage(0, 0, layer.image);
    p.end();
    return result;
}

void ensureDocument()
{
    if (g_layers.empty()) {
        Layer l;
        l.name = QStringLiteral("My Drawing");
        l.image = QImage(kCanvasW, kCanvasH, QImage::Format_RGBA8888);
        l.image.fill(Qt::transparent);
        g_layers.push_back(std::move(l));
        g_activeLayer = 0;
    }
}

void snapshotPush()
{
    QMutexLocker lock(&g_docMutex);
    Snapshot s;
    s.active = g_activeLayer;
    s.layers = g_layers;
    g_undo.push_back(std::move(s));
    if ((int)g_undo.size() > kMaxUndo)
        g_undo.erase(g_undo.begin());
    g_redo.clear();
}

void touchDocument()
{
    ++g_revision;
}

void clearMyCanvas()
{
    QMutexLocker lock(&g_docMutex);
    ensureDocument();
    g_layers[g_activeLayer].image.fill(Qt::transparent);
    touchDocument();
}

void restoreSnapshot(const Snapshot &s)
{
    QMutexLocker lock(&g_docMutex);
    g_layers = s.layers;
    g_activeLayer = qBound(0, s.active, qMax(0, (int)g_layers.size() - 1));
    touchDocument();
}

QRectF canvasDisplayRect(const QWidget *widget, double zoom)
{
    const double baseW = kCanvasW * zoom;
    const double baseH = kCanvasH * zoom;
    const double scale = qMin(widget->width() / baseW, widget->height() / baseH);
    const double w = baseW * scale;
    const double h = baseH * scale;
    const double x = (widget->width() - w) * 0.5;
    const double y = (widget->height() - h) * 0.5;
    return QRectF(x, y, w, h);
}

QPoint canvasPointFromWidget(const QWidget *widget, const QPointF &p, double zoom)
{
    QRectF r = canvasDisplayRect(widget, zoom);
    if (r.width() <= 0 || r.height() <= 0)
        return {};
    const double nx = qBound(0.0, (p.x() - r.left()) / r.width(), 1.0);
    const double ny = qBound(0.0, (p.y() - r.top()) / r.height(), 1.0);
    return QPoint(qBound(0, qRound(nx * (kCanvasW - 1)), kCanvasW - 1),
                  qBound(0, qRound(ny * (kCanvasH - 1)), kCanvasH - 1));
}

QPen makePen(bool erase)
{
    const int width = erase ? g_eraser : g_brush;
    const QColor color = erase ? QColor(Qt::transparent) : g_color;
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

void drawFreehand(const QPoint &a, const QPoint &b, bool erase)
{
    QMutexLocker lock(&g_docMutex);
    ensureDocument();
    QPainter p(&g_layers[g_activeLayer].image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(erase ? QPainter::CompositionMode_Clear : QPainter::CompositionMode_SourceOver);
    p.setPen(makePen(erase));
    p.drawLine(a, b);
    p.end();
    touchDocument();
}

void drawShape(const QPoint &a, const QPoint &b, Tool tool)
{
    QMutexLocker lock(&g_docMutex);
    ensureDocument();
    QPainter p(&g_layers[g_activeLayer].image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(makePen(false));
    p.setBrush(Qt::NoBrush);
    const QRectF rect(QPointF(qMin(a.x(), b.x()), qMin(a.y(), b.y())),
                      QPointF(qMax(a.x(), b.x()), qMax(a.y(), b.y())));
    switch (tool) {
    case Tool::Line: p.drawLine(a, b); break;
    case Tool::Rect: p.drawRect(rect); break;
    case Tool::Ellipse: p.drawEllipse(rect); break;
    case Tool::Arrow: {
        p.drawLine(a, b);
        const QLineF line(a, b);
        const double angle = std::atan2(-line.dy(), line.dx());
        const double sz = qMax(10.0, (double)g_brush * 2.2);
        const QPointF end = line.p2();
        const QPointF p1 = end - QPointF(std::cos(angle + M_PI / 6.0) * sz,
                                          -std::sin(angle + M_PI / 6.0) * sz);
        const QPointF p2 = end - QPointF(std::cos(angle - M_PI / 6.0) * sz,
                                          -std::sin(angle - M_PI / 6.0) * sz);
        p.drawLine(end, p1);
        p.drawLine(end, p2);
        break;
    }
    default: break;
    }
    p.end();
    touchDocument();
}

void drawTextAt(const QPoint &pt, const QString &text)
{
    QMutexLocker lock(&g_docMutex);
    ensureDocument();
    QPainter p(&g_layers[g_activeLayer].image);
    QFont f;
    f.setPointSize(qMax(14, g_brush * 2));
    p.setFont(f);
    p.setPen(g_color);
    p.drawText(pt, text);
    p.end();
    touchDocument();
}

static const char *sourceGetName(void *)
{
    return "VyanHQ Draw";
}

struct SourceState {
    obs_source_t *source = nullptr;
};

static void *sourceCreate(obs_data_t *, obs_source_t *source)
{
    ensureDocument();
    auto *state = new SourceState;
    state->source = source;
    return state;
}

static void sourceDestroy(void *data)
{
    delete static_cast<SourceState *>(data);
}

static void sourceShow(void *data)
{
    Q_UNUSED(data);
    blog(LOG_INFO, "[VyanHQ Draw] native source shown");
}

static void sourceHide(void *data)
{
    Q_UNUSED(data);
    blog(LOG_INFO, "[VyanHQ Draw] native source hidden");
}
static uint32_t sourceWidth(void *) { return kCanvasW; }
static uint32_t sourceHeight(void *) { return kCanvasH; }

static obs_properties_t *sourceProperties(void *)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_text(props, "info", "VyanHQ Draw is a fixed 1920×1080 transparent canvas.", OBS_TEXT_INFO);
    return props;
}

static void sourceVideoRender(void *data, gs_effect_t *effect)
{
    Q_UNUSED(data);
    Q_UNUSED(effect);

    if (!g_visible)
        return;

    QImage composite;
    uint64_t revision = 0;
    {
        QMutexLocker lock(&g_docMutex);
        ensureDocument();
        composite = compositeDocument();
        revision = g_revision;
    }

    if (!g_texture) {
        g_texture = gs_texture_create(kCanvasW, kCanvasH, GS_RGBA, 1, nullptr, GS_DYNAMIC);
        if (!g_texture) {
            blog(LOG_WARNING, "[VyanHQ Draw] failed to create native canvas texture");
            return;
        }
        g_textureRevision = 0;
    }

    if (revision != g_textureRevision) {
        QImage rgba = composite.convertToFormat(QImage::Format_RGBA8888);
        gs_texture_set_image(g_texture, rgba.constBits(), (uint32_t)rgba.bytesPerLine(), false);
        g_textureRevision = revision;
    }

    // Use the same native OBS effect pipeline used by synchronous sources.
    // This avoids relying on the caller-provided effect and makes the source
    // behave like a normal native transparent texture source.
    gs_effect_t *drawEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    if (!drawEffect)
        return;

    gs_eparam_t *imageParam = gs_effect_get_param_by_name(drawEffect, "image");
    if (!imageParam)
        return;

    gs_effect_set_texture(imageParam, g_texture);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    while (gs_effect_loop(drawEffect, "Draw")) {
        gs_draw_sprite(g_texture, 0, kCanvasW, kCanvasH);
    }
    gs_blend_state_pop();
}

static obs_source_info g_sourceInfoInit()
{
    obs_source_info info{};
    info.id = "vyanhq_draw";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = sourceGetName;
    info.create = sourceCreate;
    info.destroy = sourceDestroy;
    info.get_width = sourceWidth;
    info.get_height = sourceHeight;
    info.get_properties = sourceProperties;
    info.video_render = sourceVideoRender;
    info.show = sourceShow;
    info.hide = sourceHide;
    return info;
}

class CanvasWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit CanvasWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        setMinimumSize(420, 280);
    }

    void setTool(Tool tool) { m_tool = tool; setCursor(tool == Tool::Text ? Qt::IBeamCursor : Qt::CrossCursor); }
    void setZoom(double z) { m_zoom = qBound(0.25, z, 2.5); update(); }
    double zoom() const { return m_zoom; }
    void setColor(const QColor &c) { Q_UNUSED(c); }
    void refresh() { update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(26, 26, 30));
        QRectF dst = canvasDisplayRect(this, m_zoom);
        p.fillRect(dst, Qt::black);
        QMutexLocker lock(&g_docMutex);
        ensureDocument();
        p.drawImage(dst, compositeDocument());
        p.setPen(QPen(QColor(100, 100, 110), 1));
        p.drawRect(dst);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        const QPoint p = canvasPointFromWidget(this, e->position(), m_zoom);
        m_drawing = true;
        m_start = p;
        m_last = p;
        if (m_tool == Tool::Text) {
            m_drawing = false;
            bool ok = false;
            const QString text = QInputDialog::getText(this, "Insert Text", "Text:", QLineEdit::Normal, {}, &ok);
            if (ok && !text.isEmpty()) {
                snapshotPush();
                drawTextAt(p, text);
                update();
            }
            return;
        }
        snapshotPush();
        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) {
            drawFreehand(p, p, m_tool == Tool::Eraser);
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!m_drawing)
            return;
        const QPoint p = canvasPointFromWidget(this, e->position(), m_zoom);
        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) {
            drawFreehand(m_last, p, m_tool == Tool::Eraser);
            m_last = p;
            update();
        } else {
            // Shape preview is kept simple for this first V2 test build.
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !m_drawing)
            return;
        const QPoint p = canvasPointFromWidget(this, e->position(), m_zoom);
        if (m_tool != Tool::Pen && m_tool != Tool::Eraser)
            drawShape(m_start, p, m_tool);
        m_drawing = false;
        update();
    }

private:
    Tool m_tool = Tool::Pen;
    double m_zoom = 1.0;
    QPoint m_start;
    QPoint m_last;
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
        setSizeGripEnabled(true);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(6);

        auto *tools = new QHBoxLayout();
        const QList<QPair<QString, Tool>> buttons = {
            {"Pen", Tool::Pen}, {"Eraser", Tool::Eraser}, {"Line", Tool::Line},
            {"Rect", Tool::Rect}, {"Circle", Tool::Ellipse}, {"Arrow", Tool::Arrow}, {"Text", Tool::Text}
        };
        for (const auto &pair : buttons) {
            auto *b = new QPushButton(pair.first);
            b->setCheckable(true);
            if (pair.second == Tool::Pen) b->setChecked(true);
            tools->addWidget(b, 1);
            connect(b, &QPushButton::clicked, this, [this, tool = pair.second, b, tools]() {
                canvas->setTool(tool);
                for (QObject *obj : tools->parent()->children()) Q_UNUSED(obj);
            });
        }

        auto *color = new QPushButton("Color");
        tools->addWidget(color, 1);
        connect(color, &QPushButton::clicked, this, [this]() {
            const QColor c = QColorDialog::getColor(g_color, this, "Drawing Color");
            if (c.isValid()) g_color = c;
        });

        auto *brushText = new QLabel("Brush"); tools->addWidget(brushText);
        auto *brush = new QSlider(Qt::Horizontal); brush->setRange(1, 80); brush->setValue(g_brush); tools->addWidget(brush, 2);
        auto *eraserText = new QLabel("Eraser"); tools->addWidget(eraserText);
        auto *eraser = new QSlider(Qt::Horizontal); eraser->setRange(4, 160); eraser->setValue(g_eraser); tools->addWidget(eraser, 2);
        auto *undo = new QPushButton("Undo"); tools->addWidget(undo, 1);
        auto *redo = new QPushButton("Redo"); tools->addWidget(redo, 1);
        auto *clear = new QPushButton("Clear"); tools->addWidget(clear, 1);
        root->addLayout(tools);

        auto *zoomRow = new QHBoxLayout();
        zoomRow->addWidget(new QLabel("Zoom"));
        zoom = new QSlider(Qt::Horizontal); zoom->setRange(25, 250); zoom->setValue(100); zoomRow->addWidget(zoom, 2);
        zoomLabel = new QLabel("100%"); zoomRow->addWidget(zoomLabel);
        root->addLayout(zoomRow);

        auto *body = new QHBoxLayout();
        canvas = new CanvasWidget(this);
        body->addWidget(canvas, 1);

        auto *layers = new QVBoxLayout();
        layers->addWidget(new QLabel("Layers"));
        layerList = new QListWidget();
        layers->addWidget(layerList, 1);
        auto *layerBtns = new QHBoxLayout();
        auto *add = new QPushButton("+"); auto *remove = new QPushButton("-");
        layerBtns->addWidget(add); layerBtns->addWidget(remove); layers->addLayout(layerBtns);
        QWidget *layerPanel = new QWidget(); layerPanel->setLayout(layers); layerPanel->setMinimumWidth(190);
        body->addWidget(layerPanel);
        root->addLayout(body, 1);

        connect(brush, &QSlider::valueChanged, this, [](int v) { g_brush = v; });
        connect(eraser, &QSlider::valueChanged, this, [](int v) { g_eraser = v; });
        connect(zoom, &QSlider::valueChanged, this, [this](int v) { zoomLabel->setText(QString::number(v) + "%"); canvas->setZoom(v / 100.0); });
        connect(clear, &QPushButton::clicked, this, [this] { snapshotPush(); clearMyCanvas(); canvas->refresh(); syncLayers(); });
        connect(undo, &QPushButton::clicked, this, [this] { performUndo(); });
        connect(redo, &QPushButton::clicked, this, [this] { performRedo(); });
        connect(add, &QPushButton::clicked, this, [this] { addLayer(); });
        connect(remove, &QPushButton::clicked, this, [this] { removeLayer(); });
        connect(layerList, &QListWidget::currentRowChanged, this, [this](int row) {
            if (row >= 0 && row < (int)g_layers.size()) { g_activeLayer = row; canvas->refresh(); }
        });

        syncLayers();
    }

private:
    CanvasWidget *canvas = nullptr;
    QListWidget *layerList = nullptr;
    QSlider *zoom = nullptr;
    QLabel *zoomLabel = nullptr;

    void syncLayers()
    {
        QMutexLocker lock(&g_docMutex);
        ensureDocument();
        layerList->clear();
        for (const Layer &l : g_layers) layerList->addItem(l.name);
        if (!g_layers.empty()) layerList->setCurrentRow(g_activeLayer);
    }

    void addLayer()
    {
        snapshotPush();
        QMutexLocker lock(&g_docMutex);
        Layer l;
        l.name = QString("Layer %1").arg(g_layers.size() + 1);
        l.image = QImage(kCanvasW, kCanvasH, QImage::Format_RGBA8888);
        l.image.fill(Qt::transparent);
        g_layers.push_back(std::move(l));
        g_activeLayer = (int)g_layers.size() - 1;
        touchDocument();
        syncLayers();
        canvas->refresh();
    }

    void removeLayer()
    {
        QMutexLocker lock(&g_docMutex);
        if (g_layers.size() <= 1) return;
        if (g_activeLayer < 0 || g_activeLayer >= (int)g_layers.size()) return;
        lock.unlock();
        snapshotPush();
        lock.relock();
        g_layers.erase(g_layers.begin() + g_activeLayer);
        g_activeLayer = qBound(0, g_activeLayer, (int)g_layers.size() - 1);
        touchDocument();
        lock.unlock();
        syncLayers();
        canvas->refresh();
    }

    void performUndo()
    {
        if (g_undo.empty()) return;
        Snapshot current;
        {
            QMutexLocker lock(&g_docMutex);
            current.active = g_activeLayer;
            current.layers = g_layers;
            g_redo.push_back(current);
            Snapshot target = g_undo.back();
            g_undo.pop_back();
            g_layers = target.layers;
            g_activeLayer = target.active;
            touchDocument();
        }
        syncLayers();
        canvas->refresh();
    }

    void performRedo()
    {
        if (g_redo.empty()) return;
        Snapshot current;
        {
            QMutexLocker lock(&g_docMutex);
            current.active = g_activeLayer;
            current.layers = g_layers;
            g_undo.push_back(current);
            Snapshot target = g_redo.back();
            g_redo.pop_back();
            g_layers = target.layers;
            g_activeLayer = target.active;
            touchDocument();
        }
        syncLayers();
        canvas->refresh();
    }
};

static QString trimBase(QString s)
{
    s = s.trimmed();
    while (s.endsWith('/')) s.chop(1);
    return s;
}

class VyanDock final : public QWidget
{
    Q_OBJECT
public:
    explicit VyanDock(QWidget *parent = nullptr) : QWidget(parent), settings("VyanHQ", "VyanHQ Draw V2")
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(180, 150);
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(6);
        auto *title = new QLabel("<b style='font-size:14px'>VyanHQ Draw</b>"); root->addWidget(title);
        status = new QLabel("Disconnected"); status->setStyleSheet("font-size:10px;color:#a0a0a0;"); root->addWidget(status);

        auto *r1 = new QHBoxLayout();
        draw = new QPushButton("DRAW"); profile = new QPushButton(QString::fromUtf8("◯")); profile->setToolTip("Copy member link");
        draw->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        profile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        r1->addWidget(draw, 4); r1->addWidget(profile, 1); root->addLayout(r1, 1);

        auto *r2 = new QHBoxLayout();
        connectBtn = new QPushButton("CONNECT"); forget = new QPushButton("FORGET");
        connectBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        forget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        r2->addWidget(connectBtn); r2->addWidget(forget); root->addLayout(r2, 1);

        connect(draw, &QPushButton::clicked, this, &VyanDock::drawClicked);
        connect(profile, &QPushButton::clicked, this, &VyanDock::copyMemberLink);
        connect(connectBtn, &QPushButton::clicked, this, &VyanDock::connectClicked);
        connect(forget, &QPushButton::clicked, this, &VyanDock::forgetRoom);
        refreshStatus();
    }

    void doControl(const QString &action)
    {
        if (action == "clear-mine") {
            clearMyCanvas();
            return;
        }
        if (action == "toggle") {
            g_visible = !g_visible;
            refreshStatus();
        }
    }

private:
    QSettings settings;
    QNetworkAccessManager net;
    QLabel *status = nullptr;
    QPushButton *draw = nullptr;
    QPushButton *profile = nullptr;
    QPushButton *connectBtn = nullptr;
    QPushButton *forget = nullptr;
    QString backend, room, memberToken, hostToken, roomName;

    void setStatus(const QString &s, bool good = true)
    {
        status->setText(s);
        status->setStyleSheet(good ? "font-size:10px;color:#65d995;" : "font-size:10px;color:#f0ad4e;");
    }

    void refreshStatus()
    {
        if (!g_visible) { setStatus("Locked", true); return; }
        if (settings.value("room").toString().isEmpty() && !settings.value("backend").toString().isEmpty()) { setStatus("Connected", true); return; }
        if (!settings.value("room").toString().isEmpty()) { setStatus("Room Active", true); return; }
        setStatus("Disconnected", false);
    }

    bool backendConfigured()
    {
        backend = trimBase(settings.value("backend").toString());
        if (backend.isEmpty()) {
            QDialog dlg(this); dlg.setWindowTitle("Cloud Setup");
            auto *l = new QVBoxLayout(&dlg);
            l->addWidget(new QLabel("Cloudflare Worker URL"));
            auto *e = new QLineEdit(); e->setPlaceholderText("https://your-worker.workers.dev"); l->addWidget(e);
            auto *buttons = new QHBoxLayout(); auto *cancel = new QPushButton("Cancel"); auto *save = new QPushButton("Save"); buttons->addWidget(cancel); buttons->addWidget(save); l->addLayout(buttons);
            connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
            connect(save, &QPushButton::clicked, &dlg, [&dlg, e, this]() { const QString v = trimBase(e->text()); if (v.isEmpty()) return; settings.setValue("backend", v); backend = v; dlg.accept(); });
            if (dlg.exec() != QDialog::Accepted) return false;
        }
        return !backend.isEmpty();
    }

    bool sourceInCurrentScene() const
    {
        obs_source_t *src = obs_get_source_by_name("VyanHQ Draw");
        if (!src) return false;
        bool found = false;
        obs_source_t *sceneSrc = obs_frontend_get_current_scene();
        if (sceneSrc) {
            obs_scene_t *scene = obs_scene_from_source(sceneSrc);
            if (scene) {
                struct Ctx { obs_source_t *src; bool *found; } ctx{src, &found};
                obs_scene_enum_items(scene, [](obs_scene_t*, obs_sceneitem_t *item, void *data) {
                    auto *c = static_cast<Ctx *>(data);
                    if (obs_sceneitem_get_source(item) == c->src) { *c->found = true; return false; }
                    return true;
                }, &ctx);
            }
            obs_source_release(sceneSrc);
        }
        obs_source_release(src);
        return found;
    }

    void ensureLocalSource(const QString &canvasName)
    {
        const QString sourceName = canvasName.trimmed().isEmpty() ? QStringLiteral("VyanHQ Draw") : canvasName.trimmed();
        settings.setValue("canvasName", sourceName);
        obs_source_t *src = obs_get_source_by_name(sourceName.toUtf8().constData());
        if (!src) {
            obs_data_t *settingsObj = obs_data_create();
            obs_source_t *created = obs_source_create("vyanhq_draw", sourceName.toUtf8().constData(), settingsObj, nullptr);
            obs_data_release(settingsObj);
            src = created;
        }
        obs_source_t *sceneSrc = obs_frontend_get_current_scene();
        if (src && sceneSrc) {
            obs_scene_t *scene = obs_scene_from_source(sceneSrc);
            bool exists = false;
            if (scene) {
                struct Ctx { obs_source_t *src; bool *exists; } ctx{src, &exists};
                obs_scene_enum_items(scene, [](obs_scene_t*, obs_sceneitem_t *item, void *data) {
                    auto *c = static_cast<Ctx *>(data);
                    if (obs_sceneitem_get_source(item) == c->src) { *c->exists = true; return false; }
                    return true;
                }, &ctx);
                if (!exists) {
                    obs_sceneitem_t *item = obs_scene_add(scene, src);
                    if (item) {
                        vec2 pos{0.0f, 0.0f};
                        obs_sceneitem_set_pos(item, &pos);
                        vec2 scale{1.0f, 1.0f};
                        obs_sceneitem_set_scale(item, &scale);
                        obs_sceneitem_set_visible(item, true);
                    }
                }
            }
            obs_source_release(sceneSrc);
        }
        if (src) obs_source_release(src);
        Q_UNUSED(sceneSrc);
    }

    void drawClicked()
    {
        const QString savedName = settings.value("canvasName", "VyanHQ Draw").toString();
        const bool exists = [&]() {
            obs_source_t *src = obs_get_source_by_name(savedName.toUtf8().constData());
            if (!src) return false;
            bool found = false;
            obs_source_t *sceneSrc = obs_frontend_get_current_scene();
            if (sceneSrc) {
                obs_scene_t *scene = obs_scene_from_source(sceneSrc);
                if (scene) {
                    struct Ctx { obs_source_t *src; bool *found; } ctx{src, &found};
                    obs_scene_enum_items(scene, [](obs_scene_t*, obs_sceneitem_t *item, void *data) {
                        auto *c = static_cast<Ctx *>(data);
                        if (obs_sceneitem_get_source(item) == c->src) { *c->found = true; return false; }
                        return true;
                    }, &ctx);
                }
                obs_source_release(sceneSrc);
            }
            obs_source_release(src);
            return found;
        }();
        if (!exists) {
            QDialog dlg(this); dlg.setWindowTitle("Create Canvas");
            auto *l = new QVBoxLayout(&dlg);
            l->addWidget(new QLabel("Canvas name"));
            auto *name = new QLineEdit(); name->setPlaceholderText("VyanHQ Draw"); l->addWidget(name);
            l->addWidget(new QLabel("Private 1920×1080 native OBS canvas. No cloud room is required."));
            auto *buttons = new QHBoxLayout(); auto *cancel = new QPushButton("Cancel"); auto *create = new QPushButton("Create"); buttons->addWidget(cancel); buttons->addWidget(create); l->addLayout(buttons);
            connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
            connect(create, &QPushButton::clicked, &dlg, [&dlg, name, this]() {
                ensureDocument();
                ensureLocalSource(name->text().trimmed().isEmpty() ? QStringLiteral("VyanHQ Draw") : name->text().trimmed());
                dlg.accept();
            });
            if (dlg.exec() != QDialog::Accepted) return;
        } else {
            ensureLocalSource(savedName);
        }

        auto *window = new CanvasWindow(nullptr);
        window->setAttribute(Qt::WA_DeleteOnClose, true);
        window->show(); window->raise(); window->activateWindow();
        g_visible = true;
        setStatus("Canvas Active");
    }

    void connectClicked()
    {
        if (!backendConfigured()) return;
        if (!settings.value("room").toString().isEmpty()) { setStatus("Room Active"); return; }

        QDialog dlg(this); dlg.setWindowTitle("Create Member Room");
        auto *l = new QVBoxLayout(&dlg);
        l->addWidget(new QLabel("Room name"));
        auto *name = new QLineEdit(); name->setPlaceholderText("VyanHQ Live Draw"); l->addWidget(name);
        auto *buttons = new QHBoxLayout(); auto *cancel = new QPushButton("Cancel"); auto *create = new QPushButton("Create"); buttons->addWidget(cancel); buttons->addWidget(create); l->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(create, &QPushButton::clicked, &dlg, [this, &dlg, name]() {
            QNetworkRequest req(QUrl(backend + "/api/room/new"));
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            const QString roomNameText = name->text().trimmed().isEmpty() ? QStringLiteral("VyanHQ Live Draw") : name->text().trimmed();
            auto *reply = net.post(req, QJsonDocument(QJsonObject{{"name", roomNameText}}).toJson(QJsonDocument::Compact));
            connect(reply, &QNetworkReply::finished, this, [this, reply, &dlg, roomNameText]() {
                const auto err = reply->error();
                const auto http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const auto raw = reply->readAll(); reply->deleteLater();
                if (err != QNetworkReply::NoError || http >= 400) { QMessageBox::warning(this, "VyanHQ Draw", "Could not create member room yet."); return; }
                QJsonParseError pe{}; const auto doc = QJsonDocument::fromJson(raw, &pe);
                if (pe.error != QJsonParseError::NoError || !doc.isObject()) { QMessageBox::warning(this, "VyanHQ Draw", "Invalid room response."); return; }
                const auto o = doc.object();
                settings.setValue("room", o.value("room").toString());
                settings.setValue("roomName", o.value("name").toString(roomNameText));
                settings.setValue("hostToken", o.value("hostToken").toString());
                settings.setValue("memberToken", o.value("memberToken").toString());
                hostToken = o.value("hostToken").toString(); memberToken = o.value("memberToken").toString(); room = o.value("room").toString();
                QUrl member(backend + "/member"); QUrlQuery q; q.addQueryItem("room", room); q.addQueryItem("token", memberToken); member.setQuery(q);
                QApplication::clipboard()->setText(member.toString());
                setStatus("Room Active"); dlg.accept();
                QMessageBox::information(this, "VyanHQ Draw", "Member link copied to clipboard.");
            });
        });
        dlg.exec();
    }

    void copyMemberLink()
    {
        const QString backendUrl = trimBase(settings.value("backend").toString());
        const QString roomId = settings.value("room").toString();
        const QString token = settings.value("memberToken").toString();
        if (backendUrl.isEmpty() || roomId.isEmpty() || token.isEmpty()) { QMessageBox::information(this, "VyanHQ Draw", "Create a member room with CONNECT first."); return; }
        QUrl u(backendUrl + "/member"); QUrlQuery q; q.addQueryItem("room", roomId); q.addQueryItem("token", token); u.setQuery(q);
        QApplication::clipboard()->setText(u.toString());
    }

    void forgetRoom()
    {
        settings.remove("room"); settings.remove("roomName"); settings.remove("hostToken"); settings.remove("memberToken");
        refreshStatus();
    }
};

void hotkeyClearMine(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (pressed) clearMyCanvas();
}
void hotkeyToggle(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (pressed) { g_visible = !g_visible; }
}

} // namespace

extern "C" bool obs_module_load(void)
{
    ensureDocument();
    g_sourceInfo.id = "vyanhq_draw";
    g_sourceInfo.type = OBS_SOURCE_TYPE_INPUT;
    g_sourceInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    g_sourceInfo.get_name = sourceGetName;
    g_sourceInfo.create = sourceCreate;
    g_sourceInfo.destroy = sourceDestroy;
    g_sourceInfo.get_width = sourceWidth;
    g_sourceInfo.get_height = sourceHeight;
    g_sourceInfo.get_properties = sourceProperties;
    g_sourceInfo.video_render = sourceVideoRender;
    obs_register_source(&g_sourceInfo);

    auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    g_dock = new QDockWidget(QStringLiteral("VyanHQ Draw"), mainWindow);
    g_dock->setObjectName(QStringLiteral("VyanHQDrawDockV2"));
    g_dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    g_dock->setMinimumSize(180, 150);
    g_dock->resize(300, 220);
    g_dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    g_ui = new VyanDock(g_dock);
    g_dock->setWidget(g_ui);
    obs_frontend_add_dock_by_id("vyanhq-draw-dock-v2", "VyanHQ Draw", g_dock);

    g_clearMine = obs_hotkey_register_frontend("vyanhq_draw_clear_mine_v2", "VyanHQ Draw: Clear My Canvas", hotkeyClearMine, nullptr);
    g_toggleCanvas = obs_hotkey_register_frontend("vyanhq_draw_toggle_v2", "VyanHQ Draw: Toggle Canvas", hotkeyToggle, nullptr);

    blog(LOG_INFO, "VyanHQ Draw v2 loaded");
    return true;
}

extern "C" void obs_module_unload(void)
{
    if (g_clearMine != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(g_clearMine);
    if (g_toggleCanvas != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(g_toggleCanvas);
    if (g_texture) {
        obs_enter_graphics();
        gs_texture_destroy(g_texture);
        g_texture = nullptr;
        obs_leave_graphics();
    }
    if (g_dock) obs_frontend_remove_dock("vyanhq-draw-dock-v2");
    g_ui = nullptr;
    g_dock = nullptr;
}

#include "plugin-main.moc"
