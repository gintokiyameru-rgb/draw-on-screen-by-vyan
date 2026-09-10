#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <graphics/graphics.h>

#include <QApplication>
#include <QClipboard>
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
static bool g_eraser = false;
static QPoint g_lastPoint;
static bool g_drawing = false;
static gs_texture_t *g_localTexture = nullptr;
static uint64_t g_uploadedRevision = 0;
static obs_source_info g_localSourceInfo{};

static void clearPrivateCanvas()
{
    QMutexLocker lock(&g_canvasMutex);
    g_canvas.fill(Qt::transparent);
    ++g_canvasRevision;
}

static void drawSegment(const QPoint &a, const QPoint &b)
{
    QMutexLocker lock(&g_canvasMutex);
    QPainter p(&g_canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (g_eraser) {
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(QPen(Qt::transparent, g_eraserSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    } else {
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(QPen(QColor(255, 255, 255, 255), g_brushSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
    p.drawLine(a, b);
    ++g_canvasRevision;
}

static const char *localSourceGetName(void *, void *) { return "VyanHQ Draw"; }
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
        setMinimumSize(640, 360);
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QMutexLocker lock(&g_canvasMutex);
        p.drawImage(rect(), g_canvas);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || g_memberLocked) return;
        g_drawing = true;
        g_lastPoint = mapToCanvas(e->position().toPoint());
        drawSegment(g_lastPoint, g_lastPoint);
        update();
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!g_drawing || g_memberLocked) return;
        const QPoint p = mapToCanvas(e->position().toPoint());
        drawSegment(g_lastPoint, p);
        g_lastPoint = p;
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) g_drawing = false;
    }
private:
    QPoint mapToCanvas(const QPoint &p) const
    {
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
        resize(980, 720);
        auto *root = new QVBoxLayout(this);
        auto *bar = new QHBoxLayout();
        auto *pen = new QPushButton("Pen");
        auto *eraser = new QPushButton("Eraser");
        auto *clear = new QPushButton("Clear");
        auto *bl = new QLabel("Brush");
        auto *bs = new QSlider(Qt::Horizontal); bs->setRange(1, 80); bs->setValue(g_brushSize);
        auto *el = new QLabel("Eraser");
        auto *es = new QSlider(Qt::Horizontal); es->setRange(4, 160); es->setValue(g_eraserSize);
        bar->addWidget(pen); bar->addWidget(eraser); bar->addWidget(clear);
        bar->addWidget(bl); bar->addWidget(bs, 1); bar->addWidget(el); bar->addWidget(es, 1);
        root->addLayout(bar);
        canvas = new LocalCanvasWidget(this); root->addWidget(canvas, 1);
        connect(pen, &QPushButton::clicked, this, [] { g_eraser = false; });
        connect(eraser, &QPushButton::clicked, this, [] { g_eraser = true; });
        connect(clear, &QPushButton::clicked, this, [this] { clearPrivateCanvas(); canvas->update(); });
        connect(bs, &QSlider::valueChanged, this, [](int v) { g_brushSize = v; });
        connect(es, &QSlider::valueChanged, this, [](int v) { g_eraserSize = v; });
    }
private:
    LocalCanvasWidget *canvas = nullptr;
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
    g_dock->setObjectName(QStringLiteral("VyanHQDrawDockCompactV16"));
    g_dock->setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea|Qt::TopDockWidgetArea|Qt::BottomDockWidgetArea);
    g_dock->resize(300,220);g_dock->setMinimumSize(170,140);
    g_dock->setFeatures(QDockWidget::DockWidgetMovable|QDockWidget::DockWidgetFloatable|QDockWidget::DockWidgetClosable);
    g_ui=new VyanDock(g_dock);g_dock->setWidget(g_ui);obs_frontend_add_dock_by_id("vyanhq-draw-dock-v16","VyanHQ Draw",g_dock);

    g_clearMembers=obs_hotkey_register_frontend("vyanhq_draw_clear_members","VyanHQ Draw: Clear Member Drawings",hkClearMembers,nullptr);
    g_clearMine=obs_hotkey_register_frontend("vyanhq_draw_clear_mine","VyanHQ Draw: Clear My Canvas",hkClearMine,nullptr);
    g_lock=obs_hotkey_register_frontend("vyanhq_draw_lock","VyanHQ Draw: Lock Member Drawing",hkLock,nullptr);
    g_unlock=obs_hotkey_register_frontend("vyanhq_draw_unlock","VyanHQ Draw: Unlock Member Drawing",hkUnlock,nullptr);
    g_toggle=obs_hotkey_register_frontend("vyanhq_draw_toggle","VyanHQ Draw: Toggle Private Canvas",hkToggle,nullptr);
    blog(LOG_INFO,"VyanHQ Draw loaded (v1.16)");
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
    if(g_dock)obs_frontend_remove_dock("vyanhq-draw-dock-v16");
    g_ui=nullptr;g_dock=nullptr;
}

#include "plugin-main.moc"
