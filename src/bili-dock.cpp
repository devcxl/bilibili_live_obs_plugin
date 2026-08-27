#include "bili-dock.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QImage>
#include <QPixmap>
#include <QBuffer>
#include <QDebug>
#include <QSplitter>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <qrencode.h>
#include <thread>

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

void BiliDock::set_danmaku_ws(DanmakuWebSocket *ws)
{
    danmaku_ws_ = ws;
    if (!ws || !danmaku_display_) return;

    connect(ws, &DanmakuWebSocket::danmaku_received,
            danmaku_display_, &DanmakuDisplay::append_danmaku);
    connect(ws, &DanmakuWebSocket::gift_received,
            danmaku_display_, &DanmakuDisplay::append_gift);
    connect(ws, &DanmakuWebSocket::super_chat_received,
            danmaku_display_, &DanmakuDisplay::append_super_chat);
    connect(ws, &DanmakuWebSocket::connection_state_changed,
            this, [this](bool connected, int popularity) {
        danmaku_display_->set_connected(connected);
        danmaku_display_->set_popularity(popularity);
    });
}

bool BiliDock::configure_obs_stream(const std::string &server, const std::string &key)
{
    if (server.empty() || key.empty()) return false;

    bool first_override = !obs_service_overridden_;
    if (first_override) {
        obs_service_t *current = obs_frontend_get_streaming_service();
        previous_obs_service_ = current ? obs_service_get_ref(current) : nullptr;
    }

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "server", server.c_str());
    obs_data_set_string(settings, "key", key.c_str());

    obs_service_t *service = obs_service_create(
        "rtmp_custom", "bili-live-obs-service", settings, nullptr);
    obs_data_release(settings);
    if (!service) {
        if (first_override) {
            obs_service_release(previous_obs_service_);
            previous_obs_service_ = nullptr;
        }
        return false;
    }

    obs_frontend_set_streaming_service(service);
    obs_frontend_save_streaming_service();
    obs_service_release(service);
    obs_service_overridden_ = true;
    return true;
}

void BiliDock::apply_pending_stream_route()
{
    if (pending_stream_route_ < 0) return;

    int target_route = pending_stream_route_;
    const auto &target_addr = target_route == 0 ? primary_rtmp_addr_ : backup_rtmp_addr_;
    const auto &target_code = target_route == 0 ? primary_rtmp_code_ : backup_rtmp_code_;

    if (configure_obs_stream(target_addr, target_code)) {
        active_stream_route_ = target_route;
        pending_stream_route_ = -1;
        stream_status_->setText(
            QString("正在连接%1...").arg(stream_route_combo_->itemText(active_stream_route_)));
    } else {
        pending_stream_route_ = -1;
        stream_route_combo_->blockSignals(true);
        stream_route_combo_->setCurrentIndex(active_stream_route_);
        stream_route_combo_->blockSignals(false);
        stream_status_->setText("线路切换失败，正在恢复原线路...");
        stream_status_->setStyleSheet("color:#F28B82;font-size:12px");
    }

    QTimer::singleShot(0, this, [this]() {
        if (bili_live_started_ && !restore_obs_service_pending_)
            obs_frontend_streaming_start();
    });
}

void BiliDock::restore_obs_stream_service()
{
    if (!obs_service_overridden_) return;

    if (previous_obs_service_) {
        obs_frontend_set_streaming_service(previous_obs_service_);
        obs_frontend_save_streaming_service();
    }
    obs_service_release(previous_obs_service_);
    previous_obs_service_ = nullptr;
    obs_service_overridden_ = false;
    restore_obs_service_pending_ = false;
}

void BiliDock::on_obs_streaming_started()
{
    if (!bili_live_started_) return;
    stream_route_combo_->setEnabled(stream_route_combo_->count() > 1);
    stream_status_->setText(
        QString("直播中 — OBS 使用%1").arg(stream_route_combo_->currentText()));
    stream_status_->setStyleSheet("color:#81C784;font-size:12px");
    status_bar_->setText("B站直播与 OBS 推流均已启动");
}

void BiliDock::on_obs_streaming_stopped()
{
    if (pending_stream_route_ >= 0) {
        apply_pending_stream_route();
        return;
    }

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

    // 上部设置区容器（与弹幕区用 QSplitter 分离，可拖拽调整高度）
    auto *upper = new QWidget(this);
    auto *upper_layout = new QVBoxLayout(upper);
    upper_layout->setContentsMargins(0, 0, 0, 0);
    upper_layout->setSpacing(8);

    // ── Title row ──
    auto *title_row = new QHBoxLayout();
    auto *title_label = new QLabel("B站直播工具 - OBS 插件");
    title_label->setStyleSheet("font-size:15px;font-weight:bold;color:#fff;padding:4px");
    title_row->addWidget(title_label);
    title_row->addStretch();

    btn_header_logout_ = new QPushButton("退出登录");
    btn_header_logout_->setStyleSheet(
        "QPushButton { color:#FFB74D; background:transparent; border:1px solid #FFB74D; "
        "border-radius:4px; padding:2px 12px; font-size:12px; }"
        "QPushButton:hover { background:#FFB74D; color:#1e1e1e; }");
    btn_header_logout_->setFixedHeight(24);
    btn_header_logout_->hide();
    connect(btn_header_logout_, &QPushButton::clicked, this, &BiliDock::do_logout);
    title_row->addWidget(btn_header_logout_);
    upper_layout->addLayout(title_row);

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
    level_progress_ = new QProgressBar();
    level_progress_->setFixedHeight(10);
    level_progress_->setStyleSheet(
        "QProgressBar {"
        "  background:#2d2d2d; border:1px solid #444; border-radius:4px;"
        "  text-align:center; color:#aaa; font-size:10px;"
        "}"
        "QProgressBar::chunk { background:#669DF6; border-radius:3px; }"
    );
    level_progress_->hide();
    user_col->addWidget(level_progress_);
    user_row->addLayout(user_col);
    user_row->addStretch();

    // 账号卡片右上角：手动刷新用户信息按钮
    btn_refresh_user_ = new QPushButton("刷新");
    btn_refresh_user_->setStyleSheet(
        "QPushButton {"
        "  background:#3a3a3a; color:#ccc; border:1px solid #555;"
        "  border-radius:3px; padding:2px 10px; font-size:11px;"
        "}"
        "QPushButton:hover { background:#4a4a4a; }"
        "QPushButton:pressed { background:#333; }"
    );
    btn_refresh_user_->hide();
    connect(btn_refresh_user_, &QPushButton::clicked, this, [this]() {
        if (!do_refresh_account()) {
            status_bar_->setText("账户信息刷新失败");
        }
    });
    user_row->addWidget(btn_refresh_user_, 0, Qt::AlignTop);
    login_layout->addLayout(user_row);
    upper_layout->addWidget(login_group);

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
    upper_layout->addWidget(verify_group_);

    // ── Stream control ──
    auto *stream_group = new QGroupBox("直播控制");
    auto *stream_layout = new QVBoxLayout(stream_group);

    auto *title_edit_row = new QHBoxLayout();
    title_edit_row->addWidget(new QLabel("标题:"));
    title_edit_ = new QLineEdit();
    title_edit_->setPlaceholderText("输入直播标题...");
    connect(title_edit_, &QLineEdit::editingFinished, this, &BiliDock::on_title_edited);
    title_edit_row->addWidget(title_edit_);
    stream_layout->addLayout(title_edit_row);

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
    upper_layout->addWidget(stream_group);

    // ── Stream route ──
    stream_route_group_ = new QGroupBox("推流线路");
    auto *route_layout = new QHBoxLayout(stream_route_group_);
    route_layout->addWidget(new QLabel("当前线路:"));
    stream_route_combo_ = new QComboBox();
    connect(stream_route_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BiliDock::on_stream_route_changed);
    route_layout->addWidget(stream_route_combo_);
    stream_route_group_->hide();
    upper_layout->addWidget(stream_route_group_);

    // ── 弹幕开关 ──
    // 弹幕区容器（可拖拽调整高度）
    auto *danmaku_pane = new QWidget(this);
    auto *danmaku_layout = new QVBoxLayout(danmaku_pane);
    danmaku_layout->setContentsMargins(0, 0, 0, 0);
    danmaku_layout->setSpacing(8);

    danmaku_toggle_ = new QCheckBox("启用弹幕");
    danmaku_toggle_->setChecked(false);
    danmaku_toggle_->setEnabled(false);
    danmaku_toggle_->setToolTip("开播后可查看实时弹幕，弹幕区域可拖拽调整高度");
    connect(danmaku_toggle_, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked && danmaku_ws_) {
            danmaku_ws_->disconnect_from_room();
        } else if (checked && bili_live_started_) {
            // 关闭后重新开启必须恢复连接：连接只在开播/恢复时发起，
            // 若开启分支不补发，重新勾选后永远收不到弹幕
            start_danmaku();
        }
        if (danmaku_display_) danmaku_display_->setVisible(checked);
    });
    danmaku_layout->addWidget(danmaku_toggle_);

    // ── Danmaku display ──
    danmaku_display_ = new DanmakuDisplay();
    danmaku_display_->hide();
    danmaku_layout->addWidget(danmaku_display_);

    status_bar_ = new QLabel("就绪");
    status_bar_->setStyleSheet("color:#666;font-size:11px;padding:2px");
    // 垂直分割：上方设置区 / 弹幕区（拖拽分隔条调整高度）
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(upper);
    splitter->addWidget(danmaku_pane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);
    splitter->setHandleWidth(6);
    splitter->setStyleSheet("QSplitter::handle { background:#3a3a3a; }");
    splitter->setSizes({480, 320});   // 初始：弹幕区 320px，之后可拖拽
    main->addWidget(splitter);
    main->addWidget(status_bar_);

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
    if (polling_in_flight_ || !auth_ || qrcode_key_.isEmpty()) return;
    polling_in_flight_ = true;
    poll_count_++;

    std::string key = qrcode_key_.toStdString();
    AuthService *auth = auth_;

    std::thread([this, auth, key]() {
        json result = auth->poll_login_status(key);
        QMetaObject::invokeMethod(this, [this, result = std::move(result)]() {
            polling_in_flight_ = false;
            handle_poll_login_result(result);
        }, Qt::QueuedConnection);
    }).detach();
}

void BiliDock::handle_poll_login_result(const json &result)
{
    int code = result.value("code", -1);

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

    btn_header_logout_->show();
    danmaku_toggle_->setEnabled(true);
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
        QString("UID: %1  |  房间: %2\n关注 %3 · 粉丝 %4 · 动态 %5  |  硬币 %6 · B币 %7")
            .arg(data["uid"].toString())
            .arg(data["roomId"].toString())
            .arg(format_count(data["following"].toInt()))
            .arg(format_count(data["follower"].toInt()))
            .arg(format_count(data["dynamic_count"].toInt()))
            .arg(format_count(data["money"].toInt()))
            .arg(data["bcoin"].toInt()));
    user_detail_label_->show();

    // 升级经验条（当前经验 / 升级所需经验）
    int cur_exp = data["current_exp"].toInt();
    int next_exp = data["next_exp"].toInt();
    if (next_exp > 0) {
        level_progress_->setRange(0, next_exp);
        level_progress_->setValue(cur_exp);
        level_progress_->setFormat(QString("经验 %1 / %2").arg(cur_exp).arg(next_exp));
        level_progress_->show();
    } else {
        level_progress_->hide();
    }

    if (btn_refresh_user_) btn_refresh_user_->show();

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
    polling_in_flight_ = false;
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
    stream_route_group_->hide();
    verify_group_->hide();
    user_face_label_->hide();
    user_face_label_->clear();
    user_info_label_->hide();
    user_detail_label_->hide();
    if (level_progress_) level_progress_->hide();
    if (btn_refresh_user_) btn_refresh_user_->hide();
    parent_combo_->clear();
    sub_combo_->clear();
    title_edit_->clear();
    if (btn_header_logout_) btn_header_logout_->hide();
    if (danmaku_toggle_) {
        danmaku_toggle_->setChecked(false);
        danmaku_toggle_->setEnabled(false);
    }
    if (danmaku_display_) danmaku_display_->hide();
    if (danmaku_ws_) danmaku_ws_->disconnect_from_room();
}

// ── Account management ──

void BiliDock::do_logout()
{
    std::string uid = cfg_->current_uid;
    if (uid.empty() || !user_) return;

    auto result = user_->logout(uid);
    set_logged_out();
    btn_login_->show();
    if (result.value("server_logout", false)) {
        status_bar_->setText("已登出");
    } else {
        status_bar_->setText("已登出（B站会话注销失败，凭据可能仍有效）");
    }
}

void BiliDock::restore_live_state()
{
    if (!live_ || !user_ || !user_->has_valid_session()) return;

    auto result = live_->check_live_status();
    if (result["code"] != 0 || !result["is_live"].get<bool>()) return;

    bili_live_started_ = true;

    btn_start_->hide();
    btn_stop_->setEnabled(true);
    btn_stop_->show();
    stream_status_->setText("直播中 — OBS 已重启，推流未连接");
    stream_status_->setStyleSheet("color:#FFB74D;font-size:12px");
    stream_status_->show();
    status_bar_->setText("B站直播仍在进行，OBS 推流已中断");

    if (danmaku_ws_ && danmaku_display_) {
        std::string room_id = live_->get_room_id();
        if (!room_id.empty()) {
            danmaku_ws_->connect_to_room(room_id);
            danmaku_display_->clear_all();
            danmaku_display_->show();
        }
    }
}

// 发起弹幕连接：开播时 / 弹幕开关重新勾选时共用。
// 需已开播且开关处于勾选状态，否则不连接（无房间可连）。
void BiliDock::start_danmaku()
{
    if (!danmaku_ws_ || !danmaku_display_ || !live_ || !danmaku_toggle_->isChecked()) return;
    std::string room_id = live_->get_room_id();
    if (room_id.empty()) return;
    danmaku_ws_->connect_to_room(room_id);
    danmaku_display_->clear_all();
    danmaku_display_->show();
}

// 刷新当前用户信息（自动刷新静默降级；手动刷新由调用方决定是否提示失败）
bool BiliDock::do_refresh_account()
{
    if (!user_ || !user_->has_valid_session()) return false;

    auto result = user_->refresh_current_user();
    if (result["code"] == 0 && result.contains("data")) {
        QJsonObject data = QJsonDocument::fromJson(
            QByteArray::fromStdString(result["data"].dump())).object();
        update_user_display(data);
        status_bar_->setText("账户信息已更新");
        return true;
    }
    return false;
}

void BiliDock::refresh_account_info()
{
    do_refresh_account();   // 自动刷新：失败静默降级（保持旧缓存数据）
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

void BiliDock::on_stream_route_changed(int index)
{
    if (!bili_live_started_ || pending_stream_route_ >= 0 || index < 0 ||
        index == active_stream_route_)
        return;
    if (index == 1 && backup_rtmp_addr_.empty()) return;

    pending_stream_route_ = index;
    stream_route_combo_->setEnabled(false);
    stream_status_->setText(
        QString("正在切换到%1...").arg(stream_route_combo_->itemText(index)));
    stream_status_->setStyleSheet("color:#FFB74D;font-size:12px");

    if (obs_frontend_streaming_active())
        obs_frontend_streaming_stop();
    else
        apply_pending_stream_route();
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

        primary_rtmp_addr_ = rtmp1["addr"].get<std::string>();
        primary_rtmp_code_ = rtmp1["code"].get<std::string>();
        backup_rtmp_addr_ = rtmp2["addr"].get<std::string>();
        backup_rtmp_code_ = rtmp2["code"].get<std::string>();

        stream_route_combo_->blockSignals(true);
        stream_route_combo_->clear();
        stream_route_combo_->addItem("主线路");
        if (!backup_rtmp_addr_.empty() && !backup_rtmp_code_.empty())
            stream_route_combo_->addItem("备用线路");
        stream_route_combo_->setCurrentIndex(0);
        stream_route_combo_->setEnabled(false);
        stream_route_combo_->blockSignals(false);
        active_stream_route_ = 0;
        pending_stream_route_ = -1;

        if (!configure_obs_stream(primary_rtmp_addr_, primary_rtmp_code_)) {
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
                btn_stop_->setEnabled(true);
                btn_stop_->show();
            }
            btn_start_->setEnabled(true);
            return;
        }

        bili_live_started_ = true;
        start_danmaku();
        QTimer::singleShot(0, this, [this]() {
            if (bili_live_started_ && !restore_obs_service_pending_)
                obs_frontend_streaming_start();
        });
        stream_route_group_->show();

        btn_start_->hide();
        btn_stop_->setEnabled(true);
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
    if (!live_ || !bili_live_started_) return;
    btn_stop_->setEnabled(false);
    auto result = live_->stop_live();
    if (result["code"] == 0) {
        bili_live_started_ = false;
        if (danmaku_ws_ && danmaku_display_) {
            danmaku_ws_->disconnect_from_room();
            danmaku_display_->hide();
        }
        pending_stream_route_ = -1;
        stream_route_combo_->setEnabled(false);
        if (obs_frontend_streaming_active()) {
            restore_obs_service_pending_ = true;
            obs_frontend_streaming_stop();
        } else {
            restore_obs_stream_service();
        }

        btn_stop_->hide();
        btn_start_->show();
        btn_start_->setEnabled(!restore_obs_service_pending_);
        stream_route_group_->hide();
        primary_rtmp_addr_.clear();
        primary_rtmp_code_.clear();
        backup_rtmp_addr_.clear();
        backup_rtmp_code_.clear();
        stream_status_->setText("已停止直播");
        stream_status_->setStyleSheet("color:#888;font-size:12px");
        status_bar_->setText("直播已停止");
    } else {
        btn_stop_->setEnabled(true);
        stream_status_->setText("B站停播失败，请重试");
        stream_status_->setStyleSheet("color:#F28B82;font-size:12px");
    }
}
