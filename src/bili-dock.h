#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include "bilibili-api.h"
#include "config-manager.h"
#include "auth-service.h"

class BiliDock : public QWidget {
    Q_OBJECT

public:
    explicit BiliDock(QWidget *parent = nullptr);
    ~BiliDock() override = default;

    void set_services(AuthService *auth, LiveService *live,
                      UserService *user, ConfigManager *cfg);

public slots:
    void on_login_done(const QJsonObject &data);

private slots:
    void start_login();
    void poll_login();
    void on_account_changed(int idx);
    void do_logout();
    void on_parent_area_changed(const QString &name);
    void on_sub_area_changed(const QString &name);
    void on_title_edited();
    void on_face_reply(QNetworkReply *reply);
    void do_start_live();
    void do_stop_live();

private:
    void init_ui();
    void set_logged_out();
    void hide_login_ui();
    void set_login_error(const QString &msg);
    void show_qr_in_label(QLabel *label, const QString &url);
    void show_verify_qr(const QString &url);
    void refresh_account_list();
    void load_partitions();
    void update_user_display(const QJsonObject &data);

    AuthService *auth_ = nullptr;
    LiveService *live_ = nullptr;
    UserService *user_ = nullptr;
    ConfigManager *cfg_ = nullptr;

    QTimer *poll_timer_;
    QString qrcode_key_;
    int poll_count_ = 0;

    QNetworkAccessManager *net_;

    // UI elements
    QPushButton *btn_login_;
    QLabel *login_qr_;
    QLabel *login_status_;
    QLabel *user_face_label_;
    QLabel *user_info_label_;
    QLabel *user_detail_label_;
    QComboBox *account_combo_;
    QPushButton *btn_logout_;

    QGroupBox *verify_group_;
    QLabel *verify_qr_;
    QLabel *verify_hint_;

    QLineEdit *title_edit_;
    QComboBox *parent_combo_;
    QComboBox *sub_combo_;
    QPushButton *btn_start_;
    QPushButton *btn_stop_;
    QLabel *stream_status_;

    QLabel *rtmp_addr_label_;
    QPushButton *btn_copy_addr_;
    QLabel *rtmp_code_label_;
    QPushButton *btn_copy_code_;
    QLabel *rtmp_srt_label_;

    QLabel *status_bar_;

    std::unordered_map<std::string, std::vector<std::string>> partition_cache_;
    std::string rtmp_addr_;
    std::string rtmp_code_;
    bool restoring_ = false;
};
