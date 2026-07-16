#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QWidget>
#include <QMainWindow>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>

#include "bili-dock.h"
#include "bilibili-api.h"
#include "config-manager.h"
#include "auth-service.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("bili-live-obs", "en-US")

static BiliDock *s_dock = nullptr;
static BilibiliApi *s_api = nullptr;
static ConfigManager *s_cfg = nullptr;
static SessionState *s_state = nullptr;
static UserService *s_user = nullptr;
static LiveService *s_live = nullptr;
static AuthService *s_auth = nullptr;

static void init_services()
{
    if (s_api) return;
    s_api = new BilibiliApi();
    s_cfg = new ConfigManager();
    s_state = new SessionState();
    s_user = new UserService(s_api, s_cfg, s_state);
    s_live = new LiveService(s_api, s_cfg, s_state);
    s_auth = new AuthService(s_api, s_user, s_live, s_state);
    s_user->init_current_user();
}

static void destroy_services()
{
    delete s_auth;  s_auth = nullptr;
    delete s_live;  s_live = nullptr;
    delete s_user;  s_user = nullptr;
    delete s_state; s_state = nullptr;
    delete s_cfg;   s_cfg = nullptr;
    delete s_api;   s_api = nullptr;
}

static void dock_load()
{
    init_services();

    auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    s_dock = new BiliDock(main);
    s_dock->set_services(s_auth, s_live, s_user, s_cfg);

    auto saved = s_user->load_saved_config();
    if (saved["code"] == 0 && !saved["data"].is_null() && !saved["data"].empty()) {
        auto data = saved["data"];
        s_dock->on_login_done(
            QJsonDocument::fromJson(QByteArray::fromStdString(data.dump())).object());
    }

    obs_frontend_add_dock_by_id(
        "bili_live_dock",
        "B站直播工具",
        s_dock);
}

static void dock_unload()
{
    if (s_dock) {
        delete s_dock;
        s_dock = nullptr;
    }
    destroy_services();
}

static void ui_dock_load(void *)
{
    dock_load();
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        obs_queue_task(OBS_TASK_UI, ui_dock_load, nullptr, false);
    }
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        dock_unload();
    }
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[bili-live-obs] version %s loaded", PROJECT_VERSION);

    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[bili-live-obs] unloaded");
}
