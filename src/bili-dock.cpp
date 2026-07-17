#include "bili-dock.h"

#include <QClipboard>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QImage>
#include <QPixmap>
#include <QBuffer>
#include <QDebug>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <qrencode.h>

// ── QR pixmap generator (local, no network) ──

static QPixmap qr_pixmap(const QString &text, int size = 200)
{
    QPixmap pix;
    QRcode *qr = QRcode_encodeString(text.toUtf8().constData(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!qr) return pix;

    int margin = 4;
    int qr_size = qr->width;
    int pixel_size = (size - 2 * margin) / qr_size;
    int img_size = qr_size * pixel_size + 2 * margin;

    QImage img(img_size, img_size, QImage::Format_Mono);
    img.fill(1);

    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (qr->data[y * qr_size + x] & 1) {
                for (int py = 0; py < pixel_size; py++)
                    for (int px = 0; px < pixel_size; px++)
                        img.setPixel(margin + x * pixel_size + px, margin + y * pixel_size + py, 0);
            }
        }
    }
    QRcode_free(qr);
    pix = QPixmap::fromImage(img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    return pix;
}

// ── BiliDock ──

BiliDock::BiliDock(QWidget *parent)
    : QWidget(parent)
{
    net_ = new QNetworkAccessManager(this);
    poll_timer_ = new QTimer(this);
    connect(poll_timer_, &QTimer::timeout, this, &BiliDock::poll_login);
    init_ui();
}

BiliDock::~BiliDock()
{
    restore_obs_stream_service();
}

void BiliDock::set_services(AuthService *auth, LiveService *live,
                             UserService *user, ConfigManager *cfg)
{
    auth_ = auth;
    live_ = live;
    user_ = user;
    cfg_ = cfg;
}

bool BiliDock::configure_obs_stream(const std::string &server, const std::string &key)
{
    if (server.empty() || key.empty() || previous_obs_service_) return false;

    obs_service_t *current = obs_frontend_get_streaming_service();
    previous_obs_service_ = current ? obs_service_get_ref(current) : nullptr;

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "server", server.c_str());
    obs_data_set_string(settings, "key", key.c_str());

    obs_service_t *service = obs_service_create(
        "rtmp_custom", "bili-live-obs-service", settings, nullptr);
    obs_data_release(settings);
    if (!service) {
        obs_service_release(previous_obs_service_);
        previous_obs_service_ = nullptr;
        return false;
    }

    obs_frontend_set_streaming_service(service);
    obs_frontend_save_streaming_service();
    obs_service_release(service);
    return true;
}

void BiliDock::restore_obs_stream_service()
{
    if (!previous_obs_service_) return;

    obs_frontend_set_streaming_service(previous_obs_service_);
    obs_frontend_save_streaming_service();
    obs_service_release(previous_obs_service_);
    previous_obs_service_ = nullptr;
    restore_obs_service_pending_ = false;
}

void BiliDock::on_obs_streaming_started()
{
    if (!bili_live_started_) return;
    stream_status_->setText("直播中 — OBS 推流已启动");
    stream_status_->setStyleSheet("color:#81C784;font-size:12px");
    status_bar_->setText("B站直播与 OBS 推流均已启动");
}

void BiliDock::on_obs_streaming_stopped()
{
    if (restore_obs_service_pending_) {
        restore_obs_stream_service();
        btn_start_->setEnabled(true);
    }

    if (!bili_live_started_) return;
    stream_status_->setText("OBS 推流已停止，B站直播仍可能开启");
    stream_status_->setStyleSheet("color:#F28B82;font-size:12px");
    status_bar_->setText("请点击停止直播关闭 B站直播间");
}

void BiliDock::init_ui()
{
    auto *main = new QVBoxLayout(this);
    main->setContentsMargins(8, 8, 8, 8);
    main->setSpacing(8);

    auto *title = new QLabel("B站直播工具 - OBS 插件");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:15px;font-weight:bold;color:#fff;padding:4px");
    main->addWidget(title);

    // ── Login section ──
    auto *login_group = new QGroupBox("账号");
    auto *login_layout = new QVBoxLayout(login_group);

    btn_login_ = new QPushButton("扫码登录");
    connect(btn_login_, &QPushButton::clicked, this, &BiliDock::start_login);
    login_layout->addWidget(btn_login_);

    login_qr_ = new QLabel();
    login_qr_->setAlignment(Qt::AlignCenter);
    login_qr_->setFixedSize(200, 200);
    login_qr_->setStyleSheet("background:white;border:2px solid #669DF6;border-radius:8px");
    login_qr_->hide();
    login_layout->addWidget(login_qr_, 0, Qt::AlignCenter);

    login_status_ = new QLabel();
    login_status_->setAlignment(Qt::AlignCenter);
    login_status_->setStyleSheet("color:#888;font-size:12px");
    login_status_->hide();
    login_layout->addWidget(login_status_);

    auto *user_row = new QHBoxLayout();
    user_face_label_ = new QLabel();
    user_face_label_->setFixedSize(48, 48);
    user_face_label_->setStyleSheet("background:#2d2d2d;border:1px solid #444;border-radius:24px");
    user_face_label_->setScaledContents(true);
    user_face_label_->hide();
    user_row->addWidget(user_face_label_);

    auto *user_col = new QVBoxLayout();
    user_info_label_ = new QLabel();
    user_info_label_->setStyleSheet("color:#FFB74D;font-size:13px;font-weight:bold");
    user_info_label_->hide();
    user_col->addWidget(user_info_label_);
    user_detail_label_ = new QLabel();
    user_detail_label_->setStyleSheet("color:#888;font-size:11px");
    user_detail_label_->hide();
    user_col->addWidget(user_detail_label_);
    user_row->addLayout(user_col);
    user_row->addStretch();
    login_layout->addLayout(user_row);

    auto *acc_row = new QHBoxLayout();
    account_combo_ = new QComboBox();
    connect(account_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BiliDock::on_account_changed);
    account_combo_->hide();
    acc_row->addWidget(account_combo_);
    btn_logout_ = new QPushButton("登出");
    connect(btn_logout_, &QPushButton::clicked, this, &BiliDock::do_logout);
    btn_logout_->hide();
    acc_row->addWidget(btn_logout_);
    login_layout->addLayout(acc_row);
    main->addWidget(login_group);

    // ── Face verification ──
    verify_group_ = new QGroupBox("人脸验证");
    auto *verify_layout = new QVBoxLayout(verify_group_);
    verify_qr_ = new QLabel();
    verify_qr_->setAlignment(Qt::AlignCenter);
    verify_qr_->setFixedSize(200, 200);
    verify_qr_->setStyleSheet("background:white;border:2px solid #F28B82;border-radius:8px");
    verify_layout->addWidget(verify_qr_, 0, Qt::AlignCenter);
    verify_hint_ = new QLabel("请使用 B站 App 扫描完成人脸验证");
    verify_hint_->setAlignment(Qt::AlignCenter);
    verify_hint_->setStyleSheet("color:#FFB74D;font-size:12px");
    verify_layout->addWidget(verify_hint_);
    verify_group_->hide();
    main->addWidget(verify_group_);

    // ── Stream control ──
    auto *stream_group = new QGroupBox("直播控制");
    auto *stream_layout = new QVBoxLayout(stream_group);

    auto *title_row = new QHBoxLayout();
    title_row->addWidget(new QLabel("标题:"));
    title_edit_ = new QLineEdit();
    title_edit_->setPlaceholderText("输入直播标题...");
    connect(title_edit_, &QLineEdit::editingFinished, this, &BiliDock::on_title_edited);
    title_row->addWidget(title_edit_);
    stream_layout->addLayout(title_row);

    auto *area_row = new QHBoxLayout();
    parent_combo_ = new QComboBox();
    parent_combo_->setPlaceholderText("主分区");
    connect(parent_combo_, &QComboBox::currentTextChanged, this, &BiliDock::on_parent_area_changed);
    area_row->addWidget(parent_combo_);
    sub_combo_ = new QComboBox();
    sub_combo_->setPlaceholderText("子分区");
    connect(sub_combo_, &QComboBox::currentTextChanged, this, &BiliDock::on_sub_area_changed);
    area_row->addWidget(sub_combo_);
    stream_layout->addLayout(area_row);

    auto *btn_row = new QHBoxLayout();
    btn_start_ = new QPushButton("开始直播");
    btn_start_->setObjectName("btnStart");
    connect(btn_start_, &QPushButton::clicked, this, &BiliDock::do_start_live);
    btn_row->addWidget(btn_start_);
    btn_stop_ = new QPushButton("停止直播");
    btn_stop_->setObjectName("btnStop");
    connect(btn_stop_, &QPushButton::clicked, this, &BiliDock::do_stop_live);
    btn_stop_->hide();
    btn_row->addWidget(btn_stop_);
    stream_layout->addLayout(btn_row);

    stream_status_ = new QLabel();
    stream_status_->setAlignment(Qt::AlignCenter);
    stream_status_->setStyleSheet("color:#888;font-size:12px");
    stream_status_->hide();
    stream_layout->addWidget(stream_status_);
    main->addWidget(stream_group);

    // ── RTMP info ──
    auto *rtmp_group = new QGroupBox("推流信息");
    auto *rtmp_layout = new QVBoxLayout(rtmp_group);

    auto *addr_row = new QHBoxLayout();
    rtmp_addr_label_ = new QLabel();
    rtmp_addr_label_->setWordWrap(true);
    rtmp_addr_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rtmp_addr_label_->setStyleSheet("background:#2d2d2d;border:1px solid #444;border-radius:4px;"
        "padding:4px 6px;font-family:monospace;font-size:11px;color:#FFB74D");
    rtmp_addr_label_->hide();
    addr_row->addWidget(rtmp_addr_label_);
    btn_copy_addr_ = new QPushButton("复制");
    btn_copy_addr_->setFixedWidth(50);
    connect(btn_copy_addr_, &QPushButton::clicked, [this]() {
        QApplication::clipboard()->setText(QString::fromStdString(rtmp_addr_));
    });
    btn_copy_addr_->hide();
    addr_row->addWidget(btn_copy_addr_);
    rtmp_layout->addLayout(addr_row);

    auto *code_row = new QHBoxLayout();
    rtmp_code_label_ = new QLabel();
    rtmp_code_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rtmp_code_label_->setStyleSheet("background:#2d2d2d;border:1px solid #444;border-radius:4px;"
        "padding:4px 6px;font-family:monospace;font-size:11px;color:#FFB74D");
    rtmp_code_label_->hide();
    code_row->addWidget(rtmp_code_label_);
    btn_copy_code_ = new QPushButton("复制");
    btn_copy_code_->setFixedWidth(50);
    connect(btn_copy_code_, &QPushButton::clicked, [this]() {
        QApplication::clipboard()->setText(QString::fromStdString(rtmp_code_));
    });
    btn_copy_code_->hide();
    code_row->addWidget(btn_copy_code_);
    rtmp_layout->addLayout(code_row);

    rtmp_srt_label_ = new QLabel();
    rtmp_srt_label_->setWordWrap(true);
    rtmp_srt_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rtmp_srt_label_->setStyleSheet("background:#2d2d2d;border:1px solid #444;border-radius:4px;"
        "padding:4px 6px;font-family:monospace;font-size:11px;color:#81C784");
    rtmp_srt_label_->hide();
    rtmp_layout->addWidget(rtmp_srt_label_);
    main->addWidget(rtmp_group);

    status_bar_ = new QLabel("就绪");
    status_bar_->setStyleSheet("color:#666;font-size:11px;padding:2px");
    main->addWidget(status_bar_);
    main->addStretch();

    set_logged_out();
}

// ── Login ──

void BiliDock::start_login()
{
    if (!auth_) return;
    btn_login_->setEnabled(false);
    btn_login_->setText("获取中...");

    auto result = auth_->get_login_qrcode();
    if (result["code"] != 0) {
        set_login_error("获取二维码失败");
        return;
    }

    qrcode_key_ = QString::fromStdString(result["data"]["qrcode_key"].get<std::string>());
    QString login_url = QString::fromStdString(result["data"]["url"].get<std::string>());

    btn_login_->hide();
    show_qr_in_label(login_qr_, login_url);
    login_status_->setText("请使用 Bilibili 客户端扫码登录");
    login_status_->show();
    poll_count_ = 0;
    poll_timer_->start(2000);
}

void BiliDock::poll_login()
{
    poll_count_++;
    auto result = auth_->poll_login_status(qrcode_key_.toStdString());
    int code = result["code"];

    if (code == 0) {
        poll_timer_->stop();
        on_login_done(QJsonDocument::fromJson(
            QByteArray::fromStdString(result["data"].dump())).object());
    } else if (code == 86038) {
        poll_timer_->stop();
        set_login_error("二维码已过期");
    } else if (code == 86090) {
        login_status_->setText("已扫码，请在手机上确认...");
    } else if (poll_count_ >= 150) {
        poll_timer_->stop();
        set_login_error("登录超时");
    } else if (code < 0) {
        poll_timer_->stop();
        set_login_error(QString::fromStdString(result.value("msg", "")));
    }
}

void BiliDock::on_login_done(const QJsonObject &data)
{
    hide_login_ui();
    verify_group_->hide();
    update_user_display(data);
    refresh_account_list();
    load_partitions();
    btn_start_->show();

    restoring_ = true;
    title_edit_->setText(data["last_title"].toString());
    auto names = data["last_area_name"].toArray();
    if (names.size() >= 1)
        parent_combo_->setCurrentText(names[0].toString());
    if (names.size() >= 2)
        sub_combo_->setCurrentText(names[1].toString());
    restoring_ = false;

    status_bar_->setText("已登录 — 就绪");
}

static QString format_count(qint64 n)
{
    if (n >= 10000)
        return QString::number(n / 10000.0, 'f', 1) + "万";
    return QString::number(n);
}

void BiliDock::update_user_display(const QJsonObject &data)
{
    user_info_label_->setText(
        QString("%1  Lv.%2")
            .arg(data["uname"].toString())
            .arg(data["level"].toInt()));
    user_info_label_->show();

    user_detail_label_->setText(
        QString("UID: %1  |  房间: %2\n关注 %3 · 粉丝 %4  |  硬币 %5 · B币 %6")
            .arg(data["uid"].toString())
            .arg(data["roomId"].toString())
            .arg(format_count(data["following"].toInt()))
            .arg(format_count(data["follower"].toInt()))
            .arg(format_count(data["money"].toInt()))
            .arg(data["bcoin"].toInt()));
    user_detail_label_->show();

    QString face = data["face"].toString();
    if (!face.isEmpty()) {
        QNetworkRequest req{QUrl(face)};
        req.setRawHeader("Referer", "https://www.bilibili.com/");
        auto *reply = net_->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            on_face_reply(reply);
        });
    }
}

void BiliDock::on_face_reply(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) return;

    QPixmap pix;
    pix.loadFromData(reply->readAll());
    if (pix.isNull()) return;

    user_face_label_->setPixmap(pix.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    user_face_label_->show();
}

void BiliDock::set_login_error(const QString &msg)
{
    hide_login_ui();
    btn_login_->setEnabled(true);
    btn_login_->setText("扫码登录");
    btn_login_->show();
    status_bar_->setText(msg);
}

void BiliDock::show_verify_qr(const QString &url)
{
    verify_group_->show();
    show_qr_in_label(verify_qr_, url);
}

void BiliDock::show_qr_in_label(QLabel *label, const QString &url)
{
    QPixmap pix = qr_pixmap(url, 200);
    if (!pix.isNull()) {
        label->setPixmap(pix);
        label->show();
    } else {
        login_status_->setText("二维码生成失败");
        login_status_->show();
    }
}

void BiliDock::hide_login_ui()
{
    login_qr_->hide();
    login_status_->hide();
    btn_login_->hide();
}

void BiliDock::set_logged_out()
{
    btn_start_->hide();
    btn_stop_->hide();
    stream_status_->hide();
    rtmp_addr_label_->hide();
    btn_copy_addr_->hide();
    rtmp_code_label_->hide();
    btn_copy_code_->hide();
    rtmp_srt_label_->hide();
    verify_group_->hide();
    user_face_label_->hide();
    user_face_label_->clear();
    user_info_label_->hide();
    user_detail_label_->hide();
    parent_combo_->clear();
    sub_combo_->clear();
    title_edit_->clear();
}

// ── Account management ──

void BiliDock::refresh_account_list()
{
    if (!user_) return;
    account_combo_->blockSignals(true);
    account_combo_->clear();

    auto result = user_->get_account_list();
    auto accounts = result["data"]["list"];
    auto current_uid = result["data"]["current_uid"].get<std::string>();

    for (auto &acc : accounts) {
        QString label = QString("%1 (Lv.%2)")
            .arg(QString::fromStdString(acc["uname"].get<std::string>()))
            .arg(QString::fromStdString(
                acc["level"].is_string() ? acc["level"].get<std::string>() : std::to_string(acc["level"].get<int64_t>())));
        account_combo_->addItem(label, QString::fromStdString(acc["uid"].get<std::string>()));
        if (acc["uid"].get<std::string>() == current_uid)
            account_combo_->setCurrentIndex(account_combo_->count() - 1);
    }
    account_combo_->blockSignals(false);

    if (account_combo_->count() > 1) {
        account_combo_->show();
        btn_logout_->show();
    }
}

void BiliDock::on_account_changed(int idx)
{
    if (idx < 0 || !user_) return;
    QString uid = account_combo_->itemData(idx).toString();
    if (uid.isEmpty()) return;

    auto result = user_->switch_account(uid.toStdString());
    if (result["code"] == 0) {
        on_login_done(QJsonDocument::fromJson(
            QByteArray::fromStdString(result["data"].dump())).object());
    }
}

void BiliDock::do_logout()
{
    QString uid = account_combo_->currentData().toString();
    if (uid.isEmpty() || !user_) return;

    user_->logout(uid.toStdString());
    account_combo_->hide();
    btn_logout_->hide();
    set_logged_out();
    btn_login_->show();
    status_bar_->setText("已登出");
}

// ── Partitions ──

void BiliDock::load_partitions()
{
    if (!live_) return;
    auto result = live_->get_partitions();
    partition_cache_.clear();

    auto data = result["data"];
    parent_combo_->blockSignals(true);
    parent_combo_->clear();
    parent_combo_->addItem("");

    for (auto it = data.begin(); it != data.end(); ++it) {
        std::string parent = it.key();
        parent_combo_->addItem(QString::fromStdString(parent));
        std::vector<std::string> subs;
        for (auto &s : it.value())
            subs.push_back(s.get<std::string>());
        partition_cache_[parent] = subs;
    }
    parent_combo_->blockSignals(false);
}

void BiliDock::on_parent_area_changed(const QString &name)
{
    sub_combo_->blockSignals(true);
    sub_combo_->clear();
    if (!name.isEmpty()) {
        auto it = partition_cache_.find(name.toStdString());
        if (it != partition_cache_.end()) {
            sub_combo_->addItem("");
            for (auto &s : it->second)
                sub_combo_->addItem(QString::fromStdString(s));
        }
    }
    sub_combo_->blockSignals(false);
}

void BiliDock::on_sub_area_changed(const QString &name)
{
    if (restoring_ || name.isEmpty() || !live_) return;
    auto parent = parent_combo_->currentText();
    if (parent.isEmpty()) return;

    auto result = live_->update_area(parent.toStdString(), name.toStdString());
    if (result["code"] == 0)
        status_bar_->setText(QString("分区已更新: %1 / %2").arg(parent, name));
    else
        status_bar_->setText(
            QString("分区更新失败: %1").arg(QString::fromStdString(result.value("msg", ""))));
}

void BiliDock::on_title_edited()
{
    if (restoring_ || !live_) return;
    auto title = title_edit_->text();
    if (title.isEmpty()) return;

    auto result = live_->update_title(title.toStdString());
    if (result["code"] == 0)
        status_bar_->setText("标题已更新");
    else
        status_bar_->setText(
            QString("标题更新失败: %1").arg(QString::fromStdString(result.value("msg", ""))));
}

// ── Stream control ──

void BiliDock::do_start_live()
{
    if (!live_) return;
    if (restore_obs_service_pending_) {
        stream_status_->setText("正在停止上一轮 OBS 推流，请稍候");
        stream_status_->setStyleSheet("color:#FFB74D;font-size:12px");
        stream_status_->show();
        return;
    }
    if (bili_live_started_) {
        stream_status_->setText("B站直播已开启，请先停止当前直播");
        stream_status_->setStyleSheet("color:#F28B82;font-size:12px");
        stream_status_->show();
        return;
    }
    if (obs_frontend_streaming_active()) {
        stream_status_->setText("OBS 已在推流，请先停止当前直播");
        stream_status_->setStyleSheet("color:#F28B82;font-size:12px");
        stream_status_->show();
        return;
    }

    btn_start_->setEnabled(false);
    stream_status_->setText("正在开播...");
    stream_status_->show();

    auto result = live_->start_live(
        parent_combo_->currentText().toStdString(),
        sub_combo_->currentText().toStdString(),
        title_edit_->text().toStdString());

    if (result["code"] == 0 && result.contains("data")) {
        verify_group_->hide();
        auto data = result["data"];
        auto rtmp1 = data["rtmp1"];
        auto rtmp2 = data["rtmp2"];
        auto srt = data["srt"];

        rtmp_addr_ = rtmp1["addr"].get<std::string>();
        rtmp_code_ = rtmp1["code"].get<std::string>();

        if (!configure_obs_stream(rtmp_addr_, rtmp_code_)) {
            auto rollback = live_->stop_live();
            bili_live_started_ = rollback["code"] != 0;
            stream_status_->setText(
                rollback["code"] == 0
                    ? "OBS 推流配置失败，已回滚 B站开播"
                    : "OBS 推流配置失败，且 B站开播回滚失败，请手动停播");
            stream_status_->setStyleSheet("color:#F28B82;font-size:12px");
            status_bar_->setText("自动配置 OBS 失败");
            if (bili_live_started_) {
                btn_start_->hide();
                btn_stop_->show();
            }
            btn_start_->setEnabled(true);
            return;
        }

        bili_live_started_ = true;
        QTimer::singleShot(0, this, []() {
            obs_frontend_streaming_start();
        });

        rtmp_addr_label_->setText(QString("RTMP 地址: %1").arg(QString::fromStdString(rtmp_addr_)));
        rtmp_addr_label_->show();
        btn_copy_addr_->show();
        rtmp_code_label_->setText(QString("推流码: %1").arg(QString::fromStdString(rtmp_code_)));
        rtmp_code_label_->show();
        btn_copy_code_->show();

        if (!rtmp2["addr"].get<std::string>().empty()) {
            QString srt_text;
            auto srt_addr = srt["addr"].get<std::string>();
            if (!srt_addr.empty())
                srt_text = QString("  |  SRT: %1  |  码: %2")
                    .arg(QString::fromStdString(srt_addr))
                    .arg(QString::fromStdString(srt["code"].get<std::string>()));
            rtmp_srt_label_->setText(
                QString("备路 RTMP: %1  |  推流码: %2%3")
                    .arg(QString::fromStdString(rtmp2["addr"].get<std::string>()))
                    .arg(QString::fromStdString(rtmp2["code"].get<std::string>()))
                    .arg(srt_text));
            rtmp_srt_label_->show();
        }

        btn_start_->hide();
        btn_stop_->show();
        stream_status_->setText("B站已开播 — 等待 OBS 推流启动");
        stream_status_->setStyleSheet("color:#81C784;font-size:12px");
        status_bar_->setText("直播已开启 — OBS 推流已自动配置");

    } else if (result["code"] == 60024 || result["code"] == 60043) {
        QString qr = QString::fromStdString(result.value("qr", ""));
        stream_status_->setText("需要人脸验证, 请扫描下方二维码");
        stream_status_->setStyleSheet("color:#FFB74D;font-size:12px");
        status_bar_->setText("验证完成后重新点击开播");
        show_verify_qr(qr);
    } else {
        stream_status_->setText(
            QString("开播失败: %1").arg(QString::fromStdString(result.value("msg", "未知错误"))));
        stream_status_->setStyleSheet("color:#F28B82;font-size:12px");
    }
    btn_start_->setEnabled(true);
}

void BiliDock::do_stop_live()
{
    if (!live_) return;
    auto result = live_->stop_live();
    if (result["code"] == 0) {
        bili_live_started_ = false;
        if (obs_frontend_streaming_active()) {
            restore_obs_service_pending_ = true;
            obs_frontend_streaming_stop();
        } else {
            restore_obs_stream_service();
        }

        btn_stop_->hide();
        btn_start_->show();
        btn_start_->setEnabled(!restore_obs_service_pending_);
        stream_status_->setText("已停止直播");
        stream_status_->setStyleSheet("color:#888;font-size:12px");
        rtmp_addr_label_->hide();
        btn_copy_addr_->hide();
        rtmp_code_label_->hide();
        btn_copy_code_->hide();
        rtmp_srt_label_->hide();
        status_bar_->setText("直播已停止");
    } else {
        stream_status_->setText("B站停播失败，请重试");
        stream_status_->setStyleSheet("color:#F28B82;font-size:12px");
    }
}
