#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <graphics/graphics.h>

#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QToolButton>
#include <QStatusBar>
#include <vector>
#include <algorithm>
#include <cmath>
#include <QDialog>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QSlider>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("vyanhq-draw", "en-US")

static QDockWidget *g_dock = nullptr;
static class VyanDock *g_ui = nullptr;
static obs_hotkey_id g_clearMembers = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_clearMine = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_lock = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_unlock = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_toggle = OBS_INVALID_HOTKEY_ID;

static QMutex g_canvasMutex;
static QImage g_canvas(1920, 1080, QImage::Format_RGBA8888);
static uint64_t g_canvasRevision = 1;
static bool g_canvasVisible = true;
static bool g_memberLocked = false;
static int g_brushSize = 8;
static int g_eraserSize = 32;
static QColor g_penColor(255, 255, 255, 255);
static bool g_eraser = false;
static QPoint g_lastPoint;
static QPoint g_shapeStart;
static bool g_drawing = false;
static gs_texture_t *g_localTexture = nullptr;
static uint64_t g_uploadedRevision = 0;
static obs_source_info g_localSourceInfo{};

enum class HostTool { Pen, Eraser, Line, Rectangle, Ellipse, Arrow, Text };
static HostTool g_hostTool = HostTool::Pen;

static void clearPrivateCanvas()
{
    QMutexLocker lock(&g_canvasMutex);
    g_canvas.fill(Qt::transparent);
    ++g_canvasRevision;
}

static void pushCanvasSnapshot(std::vector<QImage> &undo, std::vector<QImage> &redo)
{
    QMutexLocker lock(&g_canvasMutex);
    undo.push_back(g_canvas);
    if (undo.size() > 50) undo.erase(undo.begin());
    redo.clear();
}

static void restoreCanvasFrom(QImage img)
{
    QMutexLocker lock(&g_canvasMutex);
    g_canvas = std::move(img);
    ++g_canvasRevision;
}

static QPoint mapToCanvasPoint(const QWidget *w, const QPoint &p)
{
    const double sx = 1920.0 / qMax(1, w->width());
    const double sy = 1080.0 / qMax(1, w->height());
    return QPoint(qBound(0, qRound(p.x() * sx), 1919), qBound(0, qRound(p.y() * sy), 1079));
}

static void drawLineOnCanvas(const QPoint &a, const QPoint &b, const QPen &pen,
                             QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver)
{
    QMutexLocker lock(&g_canvasMutex);
    QPainter p(&g_canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(mode);
    p.setPen(pen);
    p.drawLine(a, b);
    ++g_canvasRevision;
}

static void drawShapeOnCanvas(const QPoint &a, const QPoint &b, HostTool tool)
{
    QMutexLocker lock(&g_canvasMutex);
    QPainter p(&g_canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    QPen pen(g_penColor, g_brushSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QRect r(QPoint(qMin(a.x(), b.x()), qMin(a.y(), b.y())),
                  QPoint(qMax(a.x(), b.x()), qMax(a.y(), b.y())));
    switch (tool) {
    case HostTool::Line: p.drawLine(a, b); break;
    case HostTool::Rectangle: p.drawRect(r); break;
    case HostTool::Ellipse: p.drawEllipse(r); break;
    case HostTool::Arrow: {
        p.drawLine(a, b);
        const QLineF line(a, b);
        const double angle = std::atan2(-line.dy(), line.dx());
        const double arrowSize = qMax(8, g_brushSize * 2);
        QPointF p1 = line.p2() - QPointF(std::cos(angle + 3.14159265358979323846 / 6) * arrowSize,
                                         -std::sin(angle + 3.14159265358979323846 / 6) * arrowSize);
        QPointF p2 = line.p2() - QPointF(std::cos(angle - 3.14159265358979323846 / 6) * arrowSize,
                                         -std::sin(angle - 3.14159265358979323846 / 6) * arrowSize);
        p.drawLine(line.p2(), p1); p.drawLine(line.p2(), p2);
        break;
    }
    default: break;
    }
    ++g_canvasRevision;
}

static const char *localSourceGetName(void *) { return "VyanHQ Draw"; }
static void *localSourceCreate(obs_data_t *, obs_source_t *) { return nullptr; }
static void localSourceDestroy(void *) {}
static uint32_t localSourceWidth(void *) { return 1920; }
static uint32_t localSourceHeight(void *) { return 1080; }

static void localSourceVideoRender(void *, gs_effect_t *effect)
{
    if (!effect || !g_canvasVisible)
        return;

    QImage copy;
    uint64_t revision = 0;
    {
        QMutexLocker lock(&g_canvasMutex);
        copy = g_canvas;
        revision = g_canvasRevision;
    }

    if (!g_localTexture)
        g_localTexture = gs_texture_create(1920, 1080, GS_RGBA, 1, nullptr, GS_DYNAMIC);

    if (g_localTexture && revision != g_uploadedRevision) {
        gs_texture_set_image(g_localTexture, copy.constBits(), static_cast<uint32_t>(copy.bytesPerLine()), false);
        g_uploadedRevision = revision;
    }

    if (g_localTexture) {
        while (gs_effect_loop(effect, "Draw"))
            gs_draw_sprite(g_localTexture, 0, 1920, 1080);
    }
}

class LocalCanvasWidget final : public QWidget {
    Q_OBJECT
public:
    explicit LocalCanvasWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setMouseTracking(true);
        setMinimumSize(420, 280);
        setCursor(Qt::CrossCursor);
    }

    void setZoom(double z) { m_zoom = qBound(0.25, z, 2.5); updateGeometry(); update(); }
    double zoom() const { return m_zoom; }
    void setTool(HostTool tool) { m_tool = tool; setCursor(tool == HostTool::Text ? Qt::IBeamCursor : Qt::CrossCursor); }
    HostTool tool() const { return m_tool; }
    void setColor(const QColor &c) { g_penColor = c; }
    void setStacks(std::vector<QImage> *u, std::vector<QImage> *r) { m_undo = u; m_redo = r; }

    QSize sizeHint() const override { return QSize(qRound(960 * m_zoom), qRound(540 * m_zoom)); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QMutexLocker lock(&g_canvasMutex);
        const QSize target(qRound(1920 * m_zoom), qRound(1080 * m_zoom));
        QRect dst(QPoint(0,0), target);
        p.drawImage(dst, g_canvas);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || g_memberLocked) return;
        const QPoint p = mapToCanvasPointScaled(e->position().toPoint());
        if (m_tool == HostTool::Text) {
            bool ok = false;
            const QString text = QInputDialog::getText(this, "Insert Text", "Text:", QLineEdit::Normal, QString(), &ok);
            if (ok && !text.isEmpty()) {
                if (m_undo) pushCanvasSnapshot(*m_undo, *m_redo);
                QMutexLocker lock(&g_canvasMutex);
                QPainter painter(&g_canvas); painter.setRenderHint(QPainter::TextAntialiasing, true);
                QFont f; f.setPointSize(qMax(8, g_brushSize * 2)); painter.setFont(f); painter.setPen(g_penColor);
                painter.drawText(p, text); ++g_canvasRevision;
                update();
            }
            return;
        }
        if (m_undo) pushCanvasSnapshot(*m_undo, *m_redo);
        g_drawing = true;
        g_lastPoint = p;
        g_shapeStart = p;
        if (m_tool == HostTool::Pen || m_tool == HostTool::Eraser) {
            QPen pen(m_tool == HostTool::Eraser ? Qt::transparent : g_penColor,
                     m_tool == HostTool::Eraser ? g_eraserSize : g_brushSize,
                     Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            drawLineOnCanvas(p, p, pen, m_tool == HostTool::Eraser ? QPainter::CompositionMode_Clear : QPainter::CompositionMode_SourceOver);
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!g_drawing || g_memberLocked) return;
        const QPoint p = mapToCanvasPointScaled(e->position().toPoint());
        if (m_tool == HostTool::Pen || m_tool == HostTool::Eraser) {
            QPen pen(m_tool == HostTool::Eraser ? Qt::transparent : g_penColor,
                     m_tool == HostTool::Eraser ? g_eraserSize : g_brushSize,
                     Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            drawLineOnCanvas(g_lastPoint, p, pen, m_tool == HostTool::Eraser ? QPainter::CompositionMode_Clear : QPainter::CompositionMode_SourceOver);
            g_lastPoint = p; update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !g_drawing) return;
        const QPoint p = mapToCanvasPointScaled(e->position().toPoint());
        if (m_tool != HostTool::Pen && m_tool != HostTool::Eraser) {
            drawShapeOnCanvas(g_shapeStart, p, m_tool);
        }
        g_drawing = false; update();
    }

private:
    HostTool m_tool = HostTool::Pen;
    double m_zoom = 1.0;
    std::vector<QImage> *m_undo = nullptr;
    std::vector<QImage> *m_redo = nullptr;
    QPoint mapToCanvasPointScaled(const QPoint &p) const {
        const double sx = 1920.0 / qMax(1, width());
        const double sy = 1080.0 / qMax(1, height());
        return QPoint(qBound(0, qRound(p.x() * sx), 1919), qBound(0, qRound(p.y() * sy), 1079));
    }
};

class HostDrawDialog final : public QDialog {
    Q_OBJECT
public:
    explicit HostDrawDialog(QWidget *parent = nullptr) : QDialog(parent)
    {
        setWindowTitle("VyanHQ Draw — Private Canvas");
        resize(1100, 760);
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(8,8,8,8);
        root->setSpacing(6);
        auto *toolbar = new QHBoxLayout();
        addTool(toolbar, "Pen", HostTool::Pen);
        addTool(toolbar, "Eraser", HostTool::Eraser);
        addTool(toolbar, "Line", HostTool::Line);
        addTool(toolbar, "Rect", HostTool::Rectangle);
        addTool(toolbar, "Circle", HostTool::Ellipse);
        addTool(toolbar, "Arrow", HostTool::Arrow);
        addTool(toolbar, "Text", HostTool::Text);
        auto *colorBtn = new QPushButton("Color"); toolbar->addWidget(colorBtn);
        auto *brushLabel = new QLabel("Brush"); toolbar->addWidget(brushLabel);
        auto *brush = new QSlider(Qt::Horizontal); brush->setRange(1,80); brush->setValue(g_brushSize); toolbar->addWidget(brush,2);
        auto *eraseLabel = new QLabel("Eraser"); toolbar->addWidget(eraseLabel);
        auto *eraser = new QSlider(Qt::Horizontal); eraser->setRange(4,160); eraser->setValue(g_eraserSize); toolbar->addWidget(eraser,2);
        auto *undoBtn = new QPushButton("Undo"); auto *redoBtn = new QPushButton("Redo"); auto *clearBtn = new QPushButton("Clear");
        toolbar->addWidget(undoBtn); toolbar->addWidget(redoBtn); toolbar->addWidget(clearBtn);
        root->addLayout(toolbar);

        auto *zoomRow = new QHBoxLayout();
        zoomRow->addWidget(new QLabel("Zoom"));
        zoom = new QSlider(Qt::Horizontal); zoom->setRange(25,250); zoom->setValue(100); zoomRow->addWidget(zoom,1);
        zoomLabel = new QLabel("100%"); zoomRow->addWidget(zoomLabel);
        root->addLayout(zoomRow);

        canvas = new LocalCanvasWidget(this); canvas->setStacks(&undo, &redo);
        root->addWidget(canvas,1);

        connect(colorBtn,&QPushButton::clicked,this,[this]{ const QColor c=QColorDialog::getColor(g_penColor,this,"Choose drawing color"); if(c.isValid()){g_penColor=c;canvas->setColor(c);} });
        connect(brush,&QSlider::valueChanged,this,[](int v){g_brushSize=v;});
        connect(eraser,&QSlider::valueChanged,this,[](int v){g_eraserSize=v;});
        connect(clearBtn,&QPushButton::clicked,this,[this]{ pushCanvasSnapshot(undo,redo); clearPrivateCanvas(); canvas->update(); });
        connect(undoBtn,&QPushButton::clicked,this,[this]{ if(undo.empty())return; {QMutexLocker lock(&g_canvasMutex); redo.push_back(g_canvas);} restoreCanvasFrom(undo.back()); undo.pop_back(); canvas->update(); });
        connect(redoBtn,&QPushButton::clicked,this,[this]{ if(redo.empty())return; {QMutexLocker lock(&g_canvasMutex); undo.push_back(g_canvas);} restoreCanvasFrom(redo.back()); redo.pop_back(); canvas->update(); });
        connect(zoom,&QSlider::valueChanged,this,[this](int v){double z=v/100.0;zoomLabel->setText(QString::number(v)+"%");canvas->setZoom(z);});
    }
private:
    LocalCanvasWidget *canvas = nullptr;
    QSlider *zoom = nullptr;
    QLabel *zoomLabel = nullptr;
    std::vector<QImage> undo, redo;
    void addTool(QHBoxLayout *bar, const QString &name, HostTool tool) {
        auto *b = new QPushButton(name); b->setCheckable(true); b->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred); bar->addWidget(b);
        if(tool==HostTool::Pen) b->setChecked(true);
        connect(b,&QPushButton::clicked,this,[this,tool,b,bar]{ canvas->setTool(tool); for(auto *w: bar->parentWidget()->findChildren<QPushButton*>()){ if(w!=b && w->text()!=QChar(0)){} } });
    }
};

static QString trimBase(QString s)
{
    s = s.trimmed();
    while (s.endsWith('/')) s.chop(1);
    return s;
}

class VyanDock final : public QWidget {
    Q_OBJECT
public:
    explicit VyanDock(QWidget *parent = nullptr) : QWidget(parent), settings("VyanHQ", "VyanHQ Draw")
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(170, 140);
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(5);

        auto *title = new QLabel("<b style='font-size:14px'>VyanHQ Draw</b>");
        root->addWidget(title);
        statusLabel = new QLabel("Disconnected");
        root->addWidget(statusLabel);

        auto *r1 = new QHBoxLayout();
        drawBtn = new QPushButton("Draw");
        profileBtn = new QPushButton(QString::fromUtf8("◉"));
        profileBtn->setToolTip("Copy member link");
        drawBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        profileBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        r1->addWidget(drawBtn, 4); r1->addWidget(profileBtn, 1); root->addLayout(r1, 3);

        auto *r2 = new QHBoxLayout();
        connectBtn = new QPushButton("Connect");
        forgetBtn = new QPushButton("Forget");
        connectBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        forgetBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        r2->addWidget(connectBtn); r2->addWidget(forgetBtn); root->addLayout(r2, 2);

        connect(drawBtn, &QPushButton::clicked, this, &VyanDock::drawClicked);
        connect(profileBtn, &QPushButton::clicked, this, &VyanDock::copyMemberLink);
        connect(connectBtn, &QPushButton::clicked, this, &VyanDock::connectClicked);
        connect(forgetBtn, &QPushButton::clicked, this, &VyanDock::forgetRoom);
        loadState();
    }

    void doControl(const QString &action) { control(action); }
    void toggleOverlay() { g_canvasVisible = !g_canvasVisible; refreshStatus(); }

private:
    QSettings settings;
    QNetworkAccessManager net;
    QLabel *statusLabel = nullptr;
    QPushButton *drawBtn = nullptr;
    QPushButton *profileBtn = nullptr;
    QPushButton *connectBtn = nullptr;
    QPushButton *forgetBtn = nullptr;
    QString server, room, roomName, hostToken, memberToken, canvasName;
    bool memberRoomActive = false;
    bool canvasExists = false;

    void setStatus(const QString &text, bool good) {
        statusLabel->setText(text);
        statusLabel->setStyleSheet(good ? "color:#65d995;font-size:10px;" : "color:#f0ad4e;font-size:10px;");
    }
    QString backend() const { return trimBase(server); }
    void loadState() {
        server=settings.value("server").toString();
        room=settings.value("room").toString(); roomName=settings.value("roomName").toString();
        hostToken=settings.value("hostToken").toString(); memberToken=settings.value("memberToken").toString();
        canvasName=settings.value("canvasName").toString(); canvasExists=settings.value("canvasExists",false).toBool();
        memberRoomActive=!room.isEmpty()&&!hostToken.isEmpty()&&!memberToken.isEmpty();
        refreshStatus();
    }
    void refreshStatus() {
        if (g_memberLocked && memberRoomActive) setStatus("Locked", true);
        else if (canvasExists && g_canvasVisible) setStatus("Canvas Active", true);
        else if (memberRoomActive) setStatus("Room Active", true);
        else if (!backend().isEmpty()) setStatus("Connected", true);
        else setStatus("Disconnected", false);
    }
    bool backendConfigured() {
        if (backend().isEmpty() || backend().contains("your-worker", Qt::CaseInsensitive)) { showBackendDialog(); return false; }
        settings.setValue("server", backend()); return true;
    }
    void showBackendDialog() {
        QDialog dlg(this); dlg.setWindowTitle("Cloud Backend Setup"); auto *l=new QVBoxLayout(&dlg);
        l->addWidget(new QLabel("Cloudflare Worker URL (set once for member rooms)"));
        auto *e=new QLineEdit(server); e->setPlaceholderText("https://your-worker.workers.dev"); l->addWidget(e);
        auto *r=new QHBoxLayout(); auto *c=new QPushButton("Cancel"); auto *s=new QPushButton("Save"); r->addWidget(c);r->addWidget(s);l->addLayout(r);
        connect(c,&QPushButton::clicked,&dlg,&QDialog::reject);
        connect(s,&QPushButton::clicked,&dlg,[this,&dlg,e](){QString v=trimBase(e->text());if(v.isEmpty())return;server=v;settings.setValue("server",server);dlg.accept();refreshStatus();});
        dlg.exec();
    }
    void connectClicked() {
        if(!backendConfigured()) return;
        if(memberRoomActive){ setStatus("Room Active", true); return; }
        QDialog dlg(this); dlg.setWindowTitle("Create Member Room"); auto *l=new QVBoxLayout(&dlg);
        l->addWidget(new QLabel("Room name")); auto *n=new QLineEdit(); n->setPlaceholderText("VyanHQ Live Draw"); l->addWidget(n);
        auto *r=new QHBoxLayout(); auto *c=new QPushButton("Cancel"); auto *make=new QPushButton("Create"); r->addWidget(c);r->addWidget(make);l->addLayout(r);
        connect(c,&QPushButton::clicked,&dlg,&QDialog::reject);
        connect(make,&QPushButton::clicked,&dlg,[this,&dlg,n](){
            QString name=n->text().trimmed(); if(name.isEmpty())name="VyanHQ Live Draw";
            connectBtn->setEnabled(false); setStatus("Connecting…",false);
            QNetworkRequest req(QUrl(backend()+"/api/room/new")); req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
            auto *reply=net.post(req,QJsonDocument(QJsonObject{{"name",name}}).toJson(QJsonDocument::Compact));
            connect(reply,&QNetworkReply::finished,this,[this,reply,&dlg,name](){
                auto err=reply->error(); auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); auto raw=reply->readAll(); reply->deleteLater(); connectBtn->setEnabled(true);
                if(err!=QNetworkReply::NoError||status>=400){refreshStatus();QMessageBox::warning(this,"VyanHQ Draw","Could not create member room.");return;}
                QJsonParseError pe{};auto doc=QJsonDocument::fromJson(raw,&pe);if(pe.error!=QJsonParseError::NoError||!doc.isObject()){QMessageBox::warning(this,"VyanHQ Draw","Invalid Worker response.");return;}
                auto o=doc.object();room=o.value("room").toString();roomName=o.value("name").toString(name);hostToken=o.value("hostToken").toString();memberToken=o.value("memberToken").toString();memberRoomActive=true;
                settings.setValue("room",room);settings.setValue("roomName",roomName);settings.setValue("hostToken",hostToken);settings.setValue("memberToken",memberToken);
                QApplication::clipboard()->setText(makeMemberUrl()); createMemberOverlaySource(); refreshStatus(); dlg.accept();
                QMessageBox::information(this,"VyanHQ Draw","Member room created. The member link was copied to the clipboard.");
            });
        });
        dlg.exec();
    }
    void forgetRoom(){
        room.clear();roomName.clear();hostToken.clear();memberToken.clear();memberRoomActive=false;
        settings.remove("room");settings.remove("roomName");settings.remove("hostToken");settings.remove("memberToken");
        removeMemberOverlaySource(); refreshStatus();
    }
    QString makeMemberUrl() const { QUrl u(backend()+"/member");QUrlQuery q;q.addQueryItem("room",room);q.addQueryItem("token",memberToken);u.setQuery(q);return u.toString(); }
    QString makeMemberOverlayUrl() const { QUrl u(backend()+"/overlay");QUrlQuery q;q.addQueryItem("room",room);q.addQueryItem("token",hostToken);q.addQueryItem("role","overlay");u.setQuery(q);return u.toString(); }
    void copyMemberLink(){ if(!memberRoomActive){QMessageBox::information(this,"VyanHQ Draw","Create a member room with Connect first.");return;}QApplication::clipboard()->setText(makeMemberUrl()); }

    void drawClicked(){
        if(!canvasExists){
            QDialog dlg(this); dlg.setWindowTitle("Create Canvas"); auto *l=new QVBoxLayout(&dlg); l->addWidget(new QLabel("Canvas name"));
            auto *e=new QLineEdit(canvasName); e->setPlaceholderText("VyanHQ Draw"); l->addWidget(e);
            auto *hint=new QLabel("Private canvas for your own drawing. No Cloudflare or member room is required.");hint->setWordWrap(true);hint->setStyleSheet("color:#888;font-size:10px;");l->addWidget(hint);
            auto *r=new QHBoxLayout();auto *c=new QPushButton("Cancel");auto *make=new QPushButton("Create");r->addWidget(c);r->addWidget(make);l->addLayout(r);
            connect(c,&QPushButton::clicked,&dlg,&QDialog::reject);
            connect(make,&QPushButton::clicked,&dlg,[this,&dlg,e](){canvasName=e->text().trimmed();if(canvasName.isEmpty())canvasName="VyanHQ Draw";canvasExists=true;settings.setValue("canvasName",canvasName);settings.setValue("canvasExists",true);createLocalObsSource();dlg.accept();});
            dlg.exec();
        }
        if(canvasExists){ createLocalObsSource(); auto *host=new HostDrawDialog(this); host->setAttribute(Qt::WA_DeleteOnClose,true); host->show();host->raise();host->activateWindow();g_canvasVisible=true;setStatus("Canvas Active",true); }
    }
    void createLocalObsSource(){
        obs_source_t *existing=obs_get_source_by_name("VyanHQ Draw");
        if(!existing){
            obs_data_t *d=obs_data_create();
            obs_source_t *src=obs_source_create("vyanhq_draw_local","VyanHQ Draw",d,nullptr);
            obs_data_release(d);
            if(src){
                obs_source_t *current=obs_frontend_get_current_scene();
                if(current){obs_scene_t *scene=obs_scene_from_source(current);if(scene)obs_scene_add(scene,src);obs_source_release(current);} 
                obs_source_release(src);
            }
        } else obs_source_release(existing);
    }
    void createMemberOverlaySource(){
        if(!memberRoomActive)return;
        const QString url=makeMemberOverlayUrl();
        obs_source_t *current=obs_frontend_get_current_scene();if(!current)return;
        obs_scene_t *scene=obs_scene_from_source(current);if(!scene){obs_source_release(current);return;}
        const char *name="VyanHQ Member Overlay";
        obs_source_t *existing=obs_get_source_by_name(name);
        if(existing){obs_data_t*d=obs_source_get_settings(existing);obs_data_set_string(d,"url",url.toUtf8().constData());obs_data_set_int(d,"width",1920);obs_data_set_int(d,"height",1080);obs_data_set_int(d,"fps",60);obs_source_update(existing,d);obs_data_release(d);obs_source_release(existing);obs_source_release(current);return;}
        obs_data_t*d=obs_data_create();obs_data_set_string(d,"url",url.toUtf8().constData());obs_data_set_int(d,"width",1920);obs_data_set_int(d,"height",1080);obs_data_set_int(d,"fps",60);
        obs_source_t*browser=obs_source_create("browser_source",name,d,nullptr);obs_data_release(d);if(browser){obs_scene_add(scene,browser);obs_source_release(browser);}obs_source_release(current);
    }
    void removeMemberOverlaySource(){
        obs_source_t *s=obs_get_source_by_name("VyanHQ Member Overlay");if(!s)return;
        obs_source_t *current=obs_frontend_get_current_scene();if(!current){obs_source_release(s);return;}obs_scene_t*scene=obs_scene_from_source(current);if(scene){
            struct Ctx{obs_source_t*target;};Ctx ctx{s};
            obs_scene_enum_items(scene,[](obs_scene_t*,obs_sceneitem_t*item,void*data){auto*c=static_cast<Ctx*>(data);if(obs_sceneitem_get_source(item)==c->target){obs_sceneitem_remove(item);return false;}return true;},&ctx);
        }obs_source_release(s);obs_source_release(current);
    }
    void control(const QString &action){
        if(action=="clear-mine"){clearPrivateCanvas();return;}
        if(action=="toggle"){g_canvasVisible=!g_canvasVisible;refreshStatus();return;}
        if(!memberRoomActive||!backendConfigured())return;
        QNetworkRequest req(QUrl(backend()+"/api/control"));req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
        auto *reply=net.post(req,QJsonDocument(QJsonObject{{"room",room},{"hostToken",hostToken},{"action",action}}).toJson(QJsonDocument::Compact));
        connect(reply,&QNetworkReply::finished,this,[this,reply,action](){auto err=reply->error();auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();reply->deleteLater();if(err!=QNetworkReply::NoError||status>=400){setStatus("Control Failed",false);return;}if(action=="lock"){g_memberLocked=true;setStatus("Locked",true);}else if(action=="unlock"){g_memberLocked=false;refreshStatus();}else if(action=="clear-members"){refreshStatus();}});
    }
};

static void hkClearMembers(void *,obs_hotkey_id,obs_hotkey_t *,bool pressed){if(pressed&&g_ui)g_ui->doControl("clear-members");}
static void hkClearMine(void *,obs_hotkey_id,obs_hotkey_t *,bool pressed){if(pressed&&g_ui)g_ui->doControl("clear-mine");}
static void hkLock(void *,obs_hotkey_id,obs_hotkey_t *,bool pressed){if(pressed&&g_ui)g_ui->doControl("lock");}
static void hkUnlock(void *,obs_hotkey_id,obs_hotkey_t *,bool pressed){if(pressed&&g_ui)g_ui->doControl("unlock");}
static void hkToggle(void *,obs_hotkey_id,obs_hotkey_t *,bool pressed){if(pressed&&g_ui)g_ui->doControl("toggle");}

extern "C" bool obs_module_load(void)
{
    g_canvas.fill(Qt::transparent);
    g_localSourceInfo.id="vyanhq_draw_local";
    g_localSourceInfo.type=OBS_SOURCE_TYPE_INPUT;
    g_localSourceInfo.output_flags=OBS_SOURCE_VIDEO;
    g_localSourceInfo.get_name=localSourceGetName;
    g_localSourceInfo.create=localSourceCreate;
    g_localSourceInfo.destroy=localSourceDestroy;
    g_localSourceInfo.get_width=localSourceWidth;
    g_localSourceInfo.get_height=localSourceHeight;
    g_localSourceInfo.video_render=localSourceVideoRender;
    obs_register_source(&g_localSourceInfo);

    auto *mainWindow=static_cast<QMainWindow *>(obs_frontend_get_main_window());
    g_dock=new QDockWidget(QStringLiteral("VyanHQ Draw"),mainWindow);
    g_dock->setObjectName(QStringLiteral("VyanHQDrawDockCompactV18"));
    g_dock->setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea|Qt::TopDockWidgetArea|Qt::BottomDockWidgetArea);
    g_dock->resize(300,220);g_dock->setMinimumSize(170,140);
    g_dock->setFeatures(QDockWidget::DockWidgetMovable|QDockWidget::DockWidgetFloatable|QDockWidget::DockWidgetClosable);
    g_ui=new VyanDock(g_dock);g_dock->setWidget(g_ui);obs_frontend_add_dock_by_id("vyanhq-draw-dock-v18","VyanHQ Draw",g_dock);

    g_clearMembers=obs_hotkey_register_frontend("vyanhq_draw_clear_members","VyanHQ Draw: Clear Member Drawings",hkClearMembers,nullptr);
    g_clearMine=obs_hotkey_register_frontend("vyanhq_draw_clear_mine","VyanHQ Draw: Clear My Canvas",hkClearMine,nullptr);
    g_lock=obs_hotkey_register_frontend("vyanhq_draw_lock","VyanHQ Draw: Lock Member Drawing",hkLock,nullptr);
    g_unlock=obs_hotkey_register_frontend("vyanhq_draw_unlock","VyanHQ Draw: Unlock Member Drawing",hkUnlock,nullptr);
    g_toggle=obs_hotkey_register_frontend("vyanhq_draw_toggle","VyanHQ Draw: Toggle Private Canvas",hkToggle,nullptr);
    blog(LOG_INFO,"VyanHQ Draw loaded (v1.18)");
    return true;
}

extern "C" void obs_module_unload(void)
{
    if(g_clearMembers!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(g_clearMembers);
    if(g_clearMine!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(g_clearMine);
    if(g_lock!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(g_lock);
    if(g_unlock!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(g_unlock);
    if(g_toggle!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(g_toggle);
    if(g_localTexture){obs_enter_graphics();gs_texture_destroy(g_localTexture);g_localTexture=nullptr;obs_leave_graphics();}
    if(g_dock)obs_frontend_remove_dock("vyanhq-draw-dock-v18");
    g_ui=nullptr;g_dock=nullptr;
}

#include "plugin-main.moc"
