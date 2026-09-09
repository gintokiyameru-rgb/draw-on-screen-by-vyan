#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QDockWidget>
#include <QFormLayout>
#include <QDesktopServices>

#include <obs-data.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("vyanhq-draw", "en-US")

static QDockWidget *g_dock = nullptr;
static class VyanDock *g_ui = nullptr;
static obs_hotkey_id g_clear = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_lock = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_unlock = OBS_INVALID_HOTKEY_ID;

class VyanDock : public QWidget {
    Q_OBJECT
public:
    explicit VyanDock(QWidget *parent = nullptr)
        : QWidget(parent), settings("VyanHQ", "VyanHQ Draw") {
        setMinimumWidth(330);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto *title = new QLabel(
            "<h2 style='margin:0'>VyanHQ Draw</h2>"
            "<span style='color:#9aa3b5'>Member collaborative overlay control</span>");
        title->setTextFormat(Qt::RichText);
        root->addWidget(title);

        auto *form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        server = new QLineEdit(settings.value("server").toString());
        server->setPlaceholderText("https://your-worker.workers.dev");
        form->addRow("Server", server);

        room = new QLineEdit(settings.value("room").toString());
        room->setPlaceholderText("room-xxxxxxxx");
        form->addRow("Room", room);

        hostToken = new QLineEdit(settings.value("hostToken").toString());
        hostToken->setEchoMode(QLineEdit::Password);
        form->addRow("Host token", hostToken);

        memberToken = new QLineEdit(settings.value("memberToken").toString());
        memberToken->setEchoMode(QLineEdit::Password);
        form->addRow("Member token", memberToken);

        root->addLayout(form);

        auto *save = new QPushButton("Save settings");
        connect(save, &QPushButton::clicked, this, [this]() {
            saveSettings();
            setStatus("Settings saved");
        });
        root->addWidget(save);

        status = new QLabel("Status: ready");
        status->setWordWrap(true);
        root->addWidget(status);

        auto *controlRow = new QHBoxLayout();
        clearBtn = new QPushButton("Clear All");
        lockBtn = new QPushButton("Lock");
        unlockBtn = new QPushButton("Unlock");
        controlRow->addWidget(clearBtn);
        controlRow->addWidget(lockBtn);
        controlRow->addWidget(unlockBtn);
        root->addLayout(controlRow);

        connect(clearBtn, &QPushButton::clicked, this, [this]() { doControl("clear"); });
        connect(lockBtn, &QPushButton::clicked, this, [this]() { doControl("lock"); });
        connect(unlockBtn, &QPushButton::clicked, this, [this]() { doControl("unlock"); });

        auto *urlRow = new QHBoxLayout();
        copyMember = new QPushButton("Copy Member URL");
        copyOverlay = new QPushButton("Copy OBS URL");
        openHost = new QPushButton("Open Host");
        urlRow->addWidget(copyMember);
        urlRow->addWidget(copyOverlay);
        urlRow->addWidget(openHost);
        root->addLayout(urlRow);

        connect(copyMember, &QPushButton::clicked, this, [this]() { copyUrl(true); });
        connect(copyOverlay, &QPushButton::clicked, this, [this]() { copyUrl(false); });
        connect(openHost, &QPushButton::clicked, this, [this]() {
            QString base = normalizedServer();
            if (!base.isEmpty()) QDesktopServices::openUrl(QUrl(base + "/host"));
        });

        auto *help = new QLabel(
            "Hotkeys are configured in OBS Settings → Hotkeys.\n"
            "This dock controls the cloud room; the transparent drawing layer is added separately as an OBS Browser Source.");
        help->setWordWrap(true);
        help->setStyleSheet("color:#9aa3b5;");
        root->addWidget(help);

        root->addStretch(1);
    }

    void saveSettings() {
        settings.setValue("server", server->text().trimmed());
        settings.setValue("room", room->text().trimmed());
        settings.setValue("hostToken", hostToken->text());
        settings.setValue("memberToken", memberToken->text());
    }

    QString normalizedServer() const {
        QString base = server->text().trimmed();
        while (base.endsWith('/')) base.chop(1);
        return base;
    }

    void setStatus(const QString &text) {
        status->setText("Status: " + text);
    }

    void doControl(const QString &action) {
        saveSettings();
        const QString base = normalizedServer();
        const QString roomName = room->text().trimmed();
        const QString token = hostToken->text();
        if (base.isEmpty() || roomName.isEmpty() || token.isEmpty()) {
            setStatus("Missing server, room, or host token");
            return;
        }

        QNetworkRequest req(QUrl(base + "/api/control"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

        QJsonObject obj;
        obj.insert("room", roomName);
        obj.insert("hostToken", token);
        obj.insert("action", action);

        auto *reply = net.post(req, QJsonDocument(obj).toJson(QJsonDocument::Compact));
        setStatus("Sending " + action + "...");
        connect(reply, &QNetworkReply::finished, this, [this, reply, action]() {
            const auto error = reply->error();
            if (error == QNetworkReply::NoError) {
                setStatus(action + " sent");
            } else {
                setStatus(action + " failed: " + reply->errorString());
            }
            reply->deleteLater();
        });
    }

    void copyUrl(bool member) {
        saveSettings();
        const QString base = normalizedServer();
        if (base.isEmpty()) {
            setStatus("Server URL is empty");
            return;
        }
        const QString path = member ? "/member" : "/overlay";
        const QString token = member ? memberToken->text() : hostToken->text();
        QString url = base + path;
        QUrlQuery query;
        query.addQueryItem("room", room->text().trimmed());
        query.addQueryItem("token", token);
        url += "?" + query.toString(QUrl::FullyEncoded);
        QApplication::clipboard()->setText(url);
        setStatus(member ? "Member URL copied" : "OBS URL copied");
    }

    void doControlFromHotkey(const char *action) { doControl(QString::fromUtf8(action)); }

private:
    QLineEdit *server = nullptr;
    QLineEdit *room = nullptr;
    QLineEdit *hostToken = nullptr;
    QLineEdit *memberToken = nullptr;
    QPushButton *clearBtn = nullptr;
    QPushButton *lockBtn = nullptr;
    QPushButton *unlockBtn = nullptr;
    QPushButton *copyMember = nullptr;
    QPushButton *copyOverlay = nullptr;
    QPushButton *openHost = nullptr;
    QLabel *status = nullptr;
    QNetworkAccessManager net;
    QSettings settings;
};

static void hk_clear(void *, obs_hotkey_id, bool pressed) {
    if (pressed && g_ui) g_ui->doControlFromHotkey("clear");
}
static void hk_lock(void *, obs_hotkey_id, bool pressed) {
    if (pressed && g_ui) g_ui->doControlFromHotkey("lock");
}
static void hk_unlock(void *, obs_hotkey_id, bool pressed) {
    if (pressed && g_ui) g_ui->doControlFromHotkey("unlock");
}

extern "C" bool obs_module_load(void) {
    g_dock = new QDockWidget("VyanHQ Draw", obs_frontend_get_main_window());
    g_dock->setObjectName("VyanHQDrawDock");
    g_ui = new VyanDock(g_dock);
    g_dock->setWidget(g_ui);

    if (!obs_frontend_add_dock_by_id("vyanhq-draw-dock", "VyanHQ Draw", g_dock)) {
        delete g_dock;
        g_dock = nullptr;
        g_ui = nullptr;
        blog(LOG_ERROR, "VyanHQ Draw: failed to add dock");
        return false;
    }

    g_clear = obs_hotkey_register_frontend("vyanhq_draw_clear_all", "VyanHQ Draw: Clear All", hk_clear, nullptr);
    g_lock = obs_hotkey_register_frontend("vyanhq_draw_lock", "VyanHQ Draw: Lock", hk_lock, nullptr);
    g_unlock = obs_hotkey_register_frontend("vyanhq_draw_unlock", "VyanHQ Draw: Unlock", hk_unlock, nullptr);

    blog(LOG_INFO, "VyanHQ Draw loaded");
    return true;
}

extern "C" void obs_module_unload(void) {
    if (g_clear != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(g_clear);
    if (g_lock != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(g_lock);
    if (g_unlock != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(g_unlock);

    obs_frontend_remove_dock("vyanhq-draw-dock");
    g_ui = nullptr;
    g_dock = nullptr;
}

#include "plugin-main.moc"
