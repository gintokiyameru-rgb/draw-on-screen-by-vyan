#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QApplication>
#include <QClipboard>
#include <QDockWidget>
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
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("vyanhq-draw", "en-US")

static QDockWidget *g_dock = nullptr;
static class VyanDock *g_ui = nullptr;
static obs_hotkey_id g_clear = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_lock = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_unlock = OBS_INVALID_HOTKEY_ID;

static QString trimBase(QString s)
{
    s = s.trimmed();
    while (s.endsWith('/')) s.chop(1);
    return s;
}

class VyanDock final : public QWidget {
    Q_OBJECT
public:
    explicit VyanDock(QWidget *parent = nullptr)
        : QWidget(parent), settings("VyanHQ", "VyanHQ Draw")
    {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(10);

        auto *title = new QLabel("<b>VyanHQ Draw</b><br><span style='color:#9aa3b5'>Interactive member overlay</span>");
        root->addWidget(title);

        auto *statusRow = new QHBoxLayout();
        statusLabel = new QLabel("● No active room");
        statusLabel->setStyleSheet("color:#f0ad4e;");
        statusRow->addWidget(statusLabel);
        statusRow->addStretch();
        root->addLayout(statusRow);

        auto *roomBox = new QGroupBox("Room");
        auto *roomLayout = new QVBoxLayout(roomBox);
        roomLabel = new QLabel("No room yet");
        roomLabel->setStyleSheet("font-weight:600;font-size:14px;");
        roomLayout->addWidget(roomLabel);
        auto *roomRow = new QHBoxLayout();
        createRoomBtn = new QPushButton("Create New Room");
        forgetRoomBtn = new QPushButton("Forget");
        roomRow->addWidget(createRoomBtn);
        roomRow->addWidget(forgetRoomBtn);
        roomLayout->addLayout(roomRow);
        root->addWidget(roomBox);

        auto *shareBox = new QGroupBox("Member & OBS");
        auto *shareLayout = new QVBoxLayout(shareBox);
        auto *row1 = new QHBoxLayout();
        copyMemberBtn = new QPushButton("Copy Member Link");
        copyOverlayBtn = new QPushButton("Copy OBS Link");
        row1->addWidget(copyMemberBtn);
        row1->addWidget(copyOverlayBtn);
        shareLayout->addLayout(row1);
        addOverlayBtn = new QPushButton("Add / Update OBS Overlay");
        shareLayout->addWidget(addOverlayBtn);
        root->addWidget(shareBox);

        auto *controlsBox = new QGroupBox("Live Controls");
        auto *controls = new QHBoxLayout(controlsBox);
        clearBtn = new QPushButton("Clear All");
        lockBtn = new QPushButton("Lock");
        unlockBtn = new QPushButton("Unlock");
        controls->addWidget(clearBtn);
        controls->addWidget(lockBtn);
        controls->addWidget(unlockBtn);
        root->addWidget(controlsBox);

        auto *advanced = new QGroupBox("Advanced");
        advanced->setCheckable(true);
        advanced->setChecked(false);
        auto *adv = new QVBoxLayout(advanced);
        adv->addWidget(new QLabel("Backend URL (set once)"));
        serverEdit = new QLineEdit(settings.value("server", "https://YOUR-WORKER.workers.dev").toString());
        adv->addWidget(serverEdit);
        auto *saveServer = new QPushButton("Save Backend");
        adv->addWidget(saveServer);
        root->addWidget(advanced);

        auto *info = new QLabel("Host/member tokens are hidden from the UI and stored locally. Members only receive the member link. Hotkeys: OBS Settings → Hotkeys.");
        info->setWordWrap(true);
        info->setStyleSheet("color:#9aa3b5;font-size:11px;");
        root->addWidget(info);

        connect(createRoomBtn, &QPushButton::clicked, this, &VyanDock::createRoom);
        connect(forgetRoomBtn, &QPushButton::clicked, this, &VyanDock::forgetRoom);
        connect(copyMemberBtn, &QPushButton::clicked, this, [this]{ copyUrl(true); });
        connect(copyOverlayBtn, &QPushButton::clicked, this, [this]{ copyUrl(false); });
        connect(addOverlayBtn, &QPushButton::clicked, this, &VyanDock::addOrUpdateOverlay);
        connect(clearBtn, &QPushButton::clicked, this, [this]{ control("clear"); });
        connect(lockBtn, &QPushButton::clicked, this, [this]{ control("lock"); });
        connect(unlockBtn, &QPushButton::clicked, this, [this]{ control("unlock"); });
        connect(saveServer, &QPushButton::clicked, this, &VyanDock::saveServer);

        loadState();
    }

    void doControl(const QString &action) { control(action); }

private:
    QSettings settings;
    QNetworkAccessManager net;
    QLineEdit *serverEdit = nullptr;
    QLabel *roomLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *createRoomBtn = nullptr;
    QPushButton *forgetRoomBtn = nullptr;
    QPushButton *copyMemberBtn = nullptr;
    QPushButton *copyOverlayBtn = nullptr;
    QPushButton *addOverlayBtn = nullptr;
    QPushButton *clearBtn = nullptr;
    QPushButton *lockBtn = nullptr;
    QPushButton *unlockBtn = nullptr;
    QString room;
    QString hostToken;
    QString memberToken;

    void setStatus(const QString &text, bool ok)
    {
        statusLabel->setText(text);
        statusLabel->setStyleSheet(ok ? "color:#65d995;" : "color:#f0ad4e;");
    }
    QString serverUrl() const { return trimBase(serverEdit->text()); }
    void saveServer() { settings.setValue("server", serverUrl()); setStatus("● Backend saved", true); }
    void saveServerSilent() { settings.setValue("server", serverUrl()); }

    void loadState()
    {
        room = settings.value("room").toString();
        hostToken = settings.value("hostToken").toString();
        memberToken = settings.value("memberToken").toString();
        updateRoomLabel();
        setStatus(room.isEmpty() ? "● No active room" : "● Room ready", !room.isEmpty());
    }
    void updateRoomLabel() { roomLabel->setText(room.isEmpty() ? "No room yet" : QString("Active: %1").arg(room)); }

    bool backendConfigured()
    {
        saveServerSilent();
        const QString b = serverUrl();
        if (b.isEmpty() || b.contains("YOUR-WORKER")) {
            QMessageBox::information(this, "VyanHQ Draw", "Buka Advanced dan masukkan URL Cloudflare Worker kamu.");
            return false;
        }
        return true;
    }
    bool ensureRoom()
    {
        if (room.isEmpty() || hostToken.isEmpty() || memberToken.isEmpty()) {
            QMessageBox::information(this, "VyanHQ Draw", "Belum ada room aktif. Klik Create New Room terlebih dahulu.");
            return false;
        }
        return true;
    }

    void createRoom()
    {
        if (!backendConfigured()) return;
        createRoomBtn->setEnabled(false);
        setStatus("● Creating room…", false);
        QNetworkRequest req(QUrl(serverUrl() + "/api/room/new"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto *reply = net.post(req, QByteArray("{}"));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            const auto error = reply->error();
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            createRoomBtn->setEnabled(true);
            if (error != QNetworkReply::NoError || status >= 400) {
                setStatus("● Create failed", false);
                QMessageBox::warning(this, "VyanHQ Draw", "Gagal membuat room. Pastikan Worker aktif dan Backend URL benar.");
                return;
            }
            QJsonParseError pe{};
            const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
                setStatus("● Invalid server response", false);
                QMessageBox::warning(this, "VyanHQ Draw", "Response Worker tidak valid.");
                return;
            }
            const auto o = doc.object();
            room = o.value("room").toString();
            hostToken = o.value("hostToken").toString();
            memberToken = o.value("memberToken").toString();
            settings.setValue("room", room);
            settings.setValue("hostToken", hostToken);
            settings.setValue("memberToken", memberToken);
            updateRoomLabel();
            setStatus("● Room ready", true);
            QMessageBox::information(this, "VyanHQ Draw", "Room siap. Gunakan Copy Member Link untuk Discord dan Add / Update OBS Overlay untuk OBS.");
        });
    }

    void forgetRoom()
    {
        room.clear(); hostToken.clear(); memberToken.clear();
        settings.remove("room"); settings.remove("hostToken"); settings.remove("memberToken");
        updateRoomLabel(); setStatus("● No active room", false);
    }

    QString makeUrl(bool member) const
    {
        QUrl url(serverUrl() + (member ? "/member" : "/overlay"));
        QUrlQuery q;
        q.addQueryItem("room", room);
        q.addQueryItem("token", member ? memberToken : hostToken);
        url.setQuery(q);
        return url.toString();
    }
    void copyUrl(bool member)
    {
        if (!ensureRoom()) return;
        QApplication::clipboard()->setText(makeUrl(member));
        setStatus(member ? "● Member link copied" : "● OBS link copied", true);
    }

    void addOrUpdateOverlay()
    {
        if (!ensureRoom()) return;
        const QString url = makeUrl(false);
        obs_source_t *current = obs_frontend_get_current_scene();
        if (!current) { QMessageBox::warning(this, "VyanHQ Draw", "Tidak ada scene aktif."); return; }
        obs_scene_t *scene = obs_scene_from_source(current);
        if (!scene) { obs_source_release(current); QMessageBox::warning(this, "VyanHQ Draw", "Scene aktif tidak dapat diakses."); return; }

        obs_source_t *existing = obs_get_source_by_name("VyanHQ Member Draw");
        if (existing) {
            obs_data_t *data = obs_source_get_settings(existing);
            obs_data_set_string(data, "url", url.toUtf8().constData());
            obs_data_set_int(data, "width", 1920);
            obs_data_set_int(data, "height", 1080);
            obs_data_set_int(data, "fps", 60);
            obs_source_update(existing, data);
            obs_data_release(data);
            obs_source_release(existing);
            obs_source_release(current);
            setStatus("● OBS overlay updated", true);
            return;
        }

        obs_data_t *data = obs_data_create();
        obs_data_set_string(data, "url", url.toUtf8().constData());
        obs_data_set_int(data, "width", 1920);
        obs_data_set_int(data, "height", 1080);
        obs_data_set_int(data, "fps", 60);
        obs_source_t *browser = obs_source_create("browser_source", "VyanHQ Member Draw", data, nullptr);
        obs_data_release(data);
        if (!browser) {
            obs_source_release(current);
            QMessageBox::warning(this, "VyanHQ Draw", "Browser Source tidak tersedia di OBS.");
            return;
        }
        obs_scene_add(scene, browser);
        obs_source_release(browser);
        obs_source_release(current);
        setStatus("● OBS overlay added", true);
    }

    void control(const QString &action)
    {
        if (!ensureRoom()) return;
        QNetworkRequest req(QUrl(serverUrl() + "/api/control"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QJsonObject body{{"room", room}, {"hostToken", hostToken}, {"action", action}};
        auto *reply = net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, action]() {
            const auto err = reply->error(); const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply->deleteLater();
            if (err != QNetworkReply::NoError || status >= 400) { setStatus("● Control failed", false); return; }
            setStatus(action == "clear" ? "● Cleared" : action == "lock" ? "● Locked" : "● Unlocked", true);
        });
    }
};

static void hk_clear(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) { if (pressed && g_ui) g_ui->doControl("clear"); }
static void hk_lock(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) { if (pressed && g_ui) g_ui->doControl("lock"); }
static void hk_unlock(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) { if (pressed && g_ui) g_ui->doControl("unlock"); }

extern "C" bool obs_module_load(void)
{
    auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    g_dock = new QDockWidget(QStringLiteral("VyanHQ Draw"), mainWindow);
    g_dock->setObjectName(QStringLiteral("VyanHQDrawDock"));
    g_ui = new VyanDock(g_dock);
    g_dock->setWidget(g_ui);
    obs_frontend_add_dock_by_id("vyanhq-draw-dock", "VyanHQ Draw", g_dock);
    g_clear = obs_hotkey_register_frontend("vyanhq_draw_clear_all", "VyanHQ Draw: Clear All", hk_clear, nullptr);
    g_lock = obs_hotkey_register_frontend("vyanhq_draw_lock", "VyanHQ Draw: Lock", hk_lock, nullptr);
    g_unlock = obs_hotkey_register_frontend("vyanhq_draw_unlock", "VyanHQ Draw: Unlock", hk_unlock, nullptr);
    blog(LOG_INFO, "VyanHQ Draw loaded");
    return true;
}

extern "C" void obs_module_unload(void)
{
    if (g_clear != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(g_clear);
    if (g_lock != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(g_lock);
    if (g_unlock != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(g_unlock);
    if (g_dock) obs_frontend_remove_dock("vyanhq-draw-dock");
    g_ui = nullptr; g_dock = nullptr;
}

#include "plugin-main.moc"
