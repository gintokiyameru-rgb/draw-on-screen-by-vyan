#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDockWidget>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFrame>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("vyanhq-draw", "en-US")

static QDockWidget *g_dock = nullptr;
static class VyanDock *g_ui = nullptr;
static obs_hotkey_id g_clear = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_lock = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_unlock = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_toggle = OBS_INVALID_HOTKEY_ID;

static QString trimBase(QString s) {
    s = s.trimmed();
    while (s.endsWith('/')) s.chop(1);
    return s;
}

class VyanDock final : public QWidget {
    Q_OBJECT
public:
    explicit VyanDock(QWidget *parent = nullptr)
        : QWidget(parent), settings("VyanHQ", "VyanHQ Draw") {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(170, 150);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(6);

        auto *title = new QLabel("<b style='font-size:14px'>VyanHQ Draw</b>");
        title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        root->addWidget(title);

        statusLabel = new QLabel("Disconnected");
        statusLabel->setStyleSheet("color:#f0ad4e;font-size:10px;");
        statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        root->addWidget(statusLabel);

        auto *topRow = new QHBoxLayout();
        drawBtn = new QPushButton("Draw");
        profileBtn = new QPushButton();
        profileBtn->setText(QString::fromUtf8("◯"));
        profileBtn->setToolTip("Copy member link");
        drawBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        profileBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        topRow->addWidget(drawBtn, 2);
        topRow->addWidget(profileBtn, 1);
        root->addLayout(topRow, 2);

        auto *bottomRow = new QHBoxLayout();
        connectBtn = new QPushButton("Connect");
        forgetBtn = new QPushButton("Forget");
        connectBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        forgetBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        bottomRow->addWidget(connectBtn, 1);
        bottomRow->addWidget(forgetBtn, 1);
        root->addLayout(bottomRow, 1);

        connect(drawBtn, &QPushButton::clicked, this, &VyanDock::drawClicked);
        connect(profileBtn, &QPushButton::clicked, this, &VyanDock::copyMemberLink);
        connect(connectBtn, &QPushButton::clicked, this, &VyanDock::connectClicked);
        connect(forgetBtn, &QPushButton::clicked, this, &VyanDock::forgetRoom);

        loadState();
    }

    void doControl(const QString &action) { control(action); }
    void toggleOverlay() { toggleCanvasSource(); }

private:
    QSettings settings;
    QNetworkAccessManager net;
    QLabel *statusLabel = nullptr;
    QPushButton *drawBtn = nullptr;
    QPushButton *profileBtn = nullptr;
    QPushButton *connectBtn = nullptr;
    QPushButton *forgetBtn = nullptr;
    QString server;
    QString room;
    QString roomName;
    QString hostToken;
    QString memberToken;
    QString canvasId;
    QString canvasName;
    QString hostCanvasToken;
    bool memberRoomActive = false;

    void setStatus(const QString &text, bool good) {
        statusLabel->setText(text);
        statusLabel->setStyleSheet(good ? "color:#65d995;font-size:10px;" : "color:#f0ad4e;font-size:10px;");
    }

    QString backend() const { return trimBase(server); }

    void loadState() {
        server = settings.value("server").toString();
        room = settings.value("room").toString();
        roomName = settings.value("roomName").toString();
        hostToken = settings.value("hostToken").toString();
        memberToken = settings.value("memberToken").toString();
        canvasId = settings.value("canvasId").toString();
        canvasName = settings.value("canvasName").toString();
        hostCanvasToken = settings.value("hostCanvasToken").toString();
        memberRoomActive = !room.isEmpty() && !hostToken.isEmpty() && !memberToken.isEmpty();
        refreshStatus();
    }

    void refreshStatus() {
        if (canvasId.isEmpty()) setStatus(memberRoomActive ? "Room Active" : "Disconnected", memberRoomActive);
        else setStatus("Canvas Active", true);
        connectBtn->setText(memberRoomActive ? "Connected" : "Connect");
    }

    bool backendConfigured() {
        if (backend().isEmpty() || backend().contains("your-worker", Qt::CaseInsensitive)) {
            showBackendDialog();
            return false;
        }
        settings.setValue("server", backend());
        return true;
    }

    void showBackendDialog() {
        QDialog dlg(this);
        dlg.setWindowTitle("VyanHQ Draw — Backend Setup");
        dlg.setModal(true);
        auto *layout = new QVBoxLayout(&dlg);
        auto *info = new QLabel("Enter your Cloudflare Worker URL once.\nIt is stored locally and hidden from the main dock.");
        info->setWordWrap(true);
        layout->addWidget(info);
        auto *edit = new QLineEdit(server);
        edit->setPlaceholderText("https://your-worker.workers.dev");
        layout->addWidget(edit);
        auto *buttons = new QHBoxLayout();
        auto *cancel = new QPushButton("Cancel");
        auto *save = new QPushButton("Save");
        buttons->addWidget(cancel);
        buttons->addWidget(save);
        layout->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(save, &QPushButton::clicked, &dlg, [&]() {
            const QString v = trimBase(edit->text());
            if (v.isEmpty()) { QMessageBox::warning(&dlg, "VyanHQ Draw", "Please enter the Worker URL."); return; }
            server = v;
            settings.setValue("server", server);
            dlg.accept();
        });
        dlg.exec();
    }

    void connectClicked() {
        if (memberRoomActive) {
            QMessageBox::information(this, "VyanHQ Draw", "A member room is already active.");
            return;
        }
        if (!backendConfigured()) return;

        QDialog dlg(this);
        dlg.setWindowTitle("Create Draw Room");
        dlg.setModal(true);
        auto *layout = new QVBoxLayout(&dlg);
        auto *name = new QLineEdit();
        name->setPlaceholderText("VyanHQ Live Draw");
        layout->addWidget(new QLabel("Room name"));
        layout->addWidget(name);
        auto *buttons = new QHBoxLayout();
        auto *cancel = new QPushButton("Cancel");
        auto *create = new QPushButton("Create");
        buttons->addWidget(cancel);
        buttons->addWidget(create);
        layout->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(create, &QPushButton::clicked, &dlg, [&]() {
            QString v = name->text().trimmed();
            if (v.isEmpty()) v = "VyanHQ Live Draw";
            createMemberRoom(v, &dlg);
        });
        dlg.exec();
    }

    void createMemberRoom(const QString &name, QDialog *dlg) {
        connectBtn->setEnabled(false);
        setStatus("Creating…", false);
        QNetworkRequest req(QUrl(backend() + "/api/room/new"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QJsonObject body{{"name", name}};
        auto *reply = net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, dlg, name]() {
            const auto err = reply->error();
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            connectBtn->setEnabled(true);
            if (err != QNetworkReply::NoError || status >= 400) {
                setStatus("Disconnected", false);
                QMessageBox::warning(dlg, "VyanHQ Draw", "Could not create the room. Check the Worker URL.");
                return;
            }
            QJsonParseError pe{}; const auto doc = QJsonDocument::fromJson(raw, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject()) { QMessageBox::warning(dlg, "VyanHQ Draw", "Invalid Worker response."); return; }
            const auto o = doc.object();
            room = o.value("room").toString();
            roomName = o.value("name").toString(name);
            hostToken = o.value("hostToken").toString();
            memberToken = o.value("memberToken").toString();
            settings.setValue("room", room); settings.setValue("roomName", roomName);
            settings.setValue("hostToken", hostToken); settings.setValue("memberToken", memberToken);
            memberRoomActive = true;
            refreshStatus();
            QApplication::clipboard()->setText(makeMemberUrl());
            dlg->accept();
            QMessageBox::information(this, "VyanHQ Draw", "Room created. The member link has been copied to your clipboard.");
        });
    }

    void forgetRoom() {
        room.clear(); roomName.clear(); hostToken.clear(); memberToken.clear(); memberRoomActive = false;
        settings.remove("room"); settings.remove("roomName"); settings.remove("hostToken"); settings.remove("memberToken");
        setStatus(canvasId.isEmpty() ? "Disconnected" : "Canvas Active", !canvasId.isEmpty());
        connectBtn->setText("Connect");
    }

    QString makeMemberUrl() const {
        QUrl u(backend() + "/member"); QUrlQuery q; q.addQueryItem("room", room); q.addQueryItem("token", memberToken); u.setQuery(q); return u.toString();
    }
    QString makeHostCanvasUrl() const {
        QUrl u(backend() + "/hostdraw"); QUrlQuery q; q.addQueryItem("room", canvasId); q.addQueryItem("token", hostCanvasToken); q.addQueryItem("role", "host"); u.setQuery(q); return u.toString();
    }
    QString makeOverlayUrl() const {
        QUrl u(backend() + "/overlay");
        QUrlQuery q;
        if (!canvasId.isEmpty()) { q.addQueryItem("room", canvasId); q.addQueryItem("token", hostCanvasToken); q.addQueryItem("role", "overlay"); }
        else if (!room.isEmpty()) { q.addQueryItem("room", room); q.addQueryItem("token", hostToken); q.addQueryItem("role", "overlay"); }
        u.setQuery(q); return u.toString();
    }

    bool createOrUseCanvas() {
        // DRAW is intentionally independent from CONNECT/member room.
        // If a private canvas already exists, we only need the backend to open it.
        if (!canvasId.isEmpty() && !hostCanvasToken.isEmpty()) {
            return backendConfigured();
        }

        // Show the canvas creation dialog FIRST. Backend setup must not
        // appear before the user has explicitly chosen to create a canvas.
        QDialog dlg(this);
        dlg.setWindowTitle("Create Canvas");
        auto *layout = new QVBoxLayout(&dlg);
        layout->addWidget(new QLabel("Canvas name"));
        auto *name = new QLineEdit(canvasName);
        name->setPlaceholderText("VyanHQ Draw");
        layout->addWidget(name);
        auto *hint = new QLabel("Creates a private host canvas for your stream. This does not create or require a member room.");
        hint->setStyleSheet("color:#888;font-size:10px;"); hint->setWordWrap(true); layout->addWidget(hint);
        auto *buttons = new QHBoxLayout(); auto *cancel = new QPushButton("Cancel"); auto *accept = new QPushButton("Create"); buttons->addWidget(cancel); buttons->addWidget(accept); layout->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(accept, &QPushButton::clicked, &dlg, [&]() {
            QString n=name->text().trimmed(); if(n.isEmpty()) n="VyanHQ Draw";
            // Only now, after the user explicitly accepted canvas creation,
            // request the one-time Cloudflare Worker configuration if needed.
            if (!backendConfigured()) return;
            QNetworkRequest req(QUrl(backend()+"/api/canvas/new")); req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
            auto *reply=net.post(req,QJsonDocument(QJsonObject{{"name",n}}).toJson(QJsonDocument::Compact));
            connect(reply,&QNetworkReply::finished,this,[this,reply,&dlg,n](){
                auto err=reply->error(); auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); auto raw=reply->readAll(); reply->deleteLater();
                if(err!=QNetworkReply::NoError||status>=400){QMessageBox::warning(&dlg,"VyanHQ Draw","Could not create canvas.");return;}
                QJsonParseError pe{}; auto doc=QJsonDocument::fromJson(raw,&pe); if(pe.error!=QJsonParseError::NoError||!doc.isObject()){QMessageBox::warning(&dlg,"VyanHQ Draw","Invalid canvas response.");return;}
                auto o=doc.object(); canvasId=o.value("canvas").toString(); canvasName=o.value("name").toString(n); hostCanvasToken=o.value("hostToken").toString();
                settings.setValue("canvasId",canvasId); settings.setValue("canvasName",canvasName); settings.setValue("hostCanvasToken",hostCanvasToken); refreshStatus(); dlg.accept();
            });
        });
        return dlg.exec()==QDialog::Accepted;
    }

    void drawClicked() {
        if (!createOrUseCanvas()) return;
        setupCanvasSource();
        QDesktopServices::openUrl(QUrl(makeHostCanvasUrl()));
        setStatus("Canvas Active", true);
    }

    void copyMemberLink() {
        if (!memberRoomActive) {
            setStatus("Disconnected", false);
            QMessageBox::information(this, "VyanHQ Draw", "Create a member room with Connect first.");
            return;
        }
        QApplication::clipboard()->setText(makeMemberUrl());
        setStatus("Member Link Copied", true);
    }

    void setupCanvasSource() {
        const QString url=makeOverlayUrl();
        obs_source_t *current=obs_frontend_get_current_scene(); if(!current){QMessageBox::warning(this,"VyanHQ Draw","No active scene.");return;}
        obs_scene_t *scene=obs_scene_from_source(current); if(!scene){obs_source_release(current);return;}
        const char *sourceName = "VyanHQ Draw";
        obs_source_t *existing=obs_get_source_by_name(sourceName);
        if(existing){
            obs_data_t *data=obs_source_get_settings(existing); obs_data_set_string(data,"url",url.toUtf8().constData()); obs_data_set_int(data,"width",1920); obs_data_set_int(data,"height",1080); obs_data_set_int(data,"fps",60); obs_source_update(existing,data); obs_data_release(data); obs_source_release(existing); obs_source_release(current); return;
        }
        obs_data_t *data=obs_data_create(); obs_data_set_string(data,"url",url.toUtf8().constData()); obs_data_set_int(data,"width",1920); obs_data_set_int(data,"height",1080); obs_data_set_int(data,"fps",60);
        obs_source_t *browser=obs_source_create("browser_source",sourceName,data,nullptr); obs_data_release(data);
        if(browser){obs_scene_add(scene,browser);obs_source_release(browser);} obs_source_release(current);
    }

    void toggleCanvasSource() {
        obs_source_t *s=obs_get_source_by_name("VyanHQ Draw"); if(!s)return; obs_source_release(s);
        // Browser source visibility is handled by toggling its scene item in the active scene.
        obs_source_t *current=obs_frontend_get_current_scene(); if(!current)return; obs_scene_t *scene=obs_scene_from_source(current); if(!scene){obs_source_release(current);return;}
        struct ToggleCtx { obs_source_t *target; } ctx;
        s=obs_get_source_by_name("VyanHQ Draw"); ctx.target=s;
        obs_scene_enum_items(scene,[](obs_scene_t *, obs_sceneitem_t *item, void *data){
            auto *ctx=static_cast<ToggleCtx*>(data); obs_source_t *src=obs_sceneitem_get_source(item); if(src==ctx->target) obs_sceneitem_set_visible(item,!obs_sceneitem_visible(item)); return true;},&ctx);
        obs_source_release(s); obs_source_release(current);
    }

    void control(const QString &action) {
        if (!backendConfigured()) return;
        QString controlRoom = !canvasId.isEmpty() ? canvasId : room;
        QString token = !canvasId.isEmpty() ? hostCanvasToken : hostToken;
        if(controlRoom.isEmpty() || token.isEmpty()) return;
        QNetworkRequest req(QUrl(backend()+"/api/control")); req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
        QJsonObject body{{"room",controlRoom},{"hostToken",token},{"action",action}};
        auto *reply=net.post(req,QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply,&QNetworkReply::finished,this,[this,reply,action](){auto err=reply->error();auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();reply->deleteLater();if(err!=QNetworkReply::NoError||status>=400){setStatus("Control Failed",false);return;} if(action=="clear-members")setStatus("Members Cleared",true);else if(action=="lock")setStatus("Locked",true);else if(action=="unlock")setStatus("Canvas Active",true);});
    }
};

static void hk_clear(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) { if(pressed&&g_ui) g_ui->doControl("clear-members"); }
static void hk_lock(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) { if(pressed&&g_ui) g_ui->doControl("lock"); }
static void hk_unlock(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) { if(pressed&&g_ui) g_ui->doControl("unlock"); }
static void hk_toggle(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) { if(pressed&&g_ui) g_ui->toggleOverlay(); }

extern "C" bool obs_module_load(void) {
    auto *mainWindow=static_cast<QMainWindow *>(obs_frontend_get_main_window());
    g_dock=new QDockWidget(QStringLiteral("VyanHQ Draw"),mainWindow);
    g_dock->setObjectName(QStringLiteral("VyanHQDrawDockCompactV15"));
    g_dock->setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea|Qt::TopDockWidgetArea|Qt::BottomDockWidgetArea);
    g_dock->resize(300,220);
    g_dock->setMinimumSize(170,150);
    g_dock->setFeatures(QDockWidget::DockWidgetMovable|QDockWidget::DockWidgetFloatable|QDockWidget::DockWidgetClosable);
    g_ui=new VyanDock(g_dock); g_dock->setWidget(g_ui);
    obs_frontend_add_dock_by_id("vyanhq-draw-dock-v15","VyanHQ Draw",g_dock);

    g_clear=obs_hotkey_register_frontend("vyanhq_draw_clear_members","VyanHQ Draw: Clear Member Drawings",hk_clear,nullptr);
    g_lock=obs_hotkey_register_frontend("vyanhq_draw_lock","VyanHQ Draw: Lock Member Drawing",hk_lock,nullptr);
    g_unlock=obs_hotkey_register_frontend("vyanhq_draw_unlock","VyanHQ Draw: Unlock Member Drawing",hk_unlock,nullptr);
    g_toggle=obs_hotkey_register_frontend("vyanhq_draw_toggle","VyanHQ Draw: Toggle Canvas",hk_toggle,nullptr);
    blog(LOG_INFO,"VyanHQ Draw loaded (v1.15)");
    return true;
}
extern "C" void obs_module_unload(void){
    if(g_clear!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(g_clear);
    if(g_lock!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(g_lock);
    if(g_unlock!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(g_unlock);
    if(g_toggle!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(g_toggle);
    if(g_dock)obs_frontend_remove_dock("vyanhq-draw-dock-v15"); g_ui=nullptr; g_dock=nullptr;
}

#include "plugin-main.moc"
