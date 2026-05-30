"""B站直播工具 — OBS Python 脚本插件

使用方法:
  1. pip install requests cryptography
  2. OBS → 工具 → 脚本 → 添加 → 选择本文件

功能:
  - 扫码登录 B站 账号, 支持多账号切换
  - 直播标题 / 分区控制
  - 一键开播 / 停播
  - 获取 RTMP 推流地址并自动配置到 OBS
"""
import os, sys, stat, json, time, re, shutil, hashlib, logging, urllib.parse
from functools import reduce

import requests
from cryptography.fernet import Fernet, InvalidToken

try:
    from PySide6 import QtWidgets, QtCore, QtGui
except ImportError:
    from PySide2 import QtWidgets, QtCore, QtGui

try:
    import obspython as obs
    IN_OBS = True
except ImportError:
    IN_OBS = False
    obs = None

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s")
logger = logging.getLogger("BiliLiveOBS")

# ════════════════════════════════════════════════════════════════
# 常量
# ════════════════════════════════════════════════════════════════

USER_AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64)"
              " AppleWebKit/537.36 (KHTML, like Gecko)"
              " Chrome/120.0.0.0 Safari/537.36")

HEADER = {
    "accept": "application/json, text/plain, */*",
    "accept-language": "zh-CN,zh;q=0.9",
    "content-type": "application/x-www-form-urlencoded; charset=UTF-8",
    "user-agent": USER_AGENT,
    "origin": "https://www.bilibili.com",
    "referer": "https://www.bilibili.com/",
}

APP_KEY = "aae92bc66f3edfab"
APP_SEC = "af125a0d5279fd576c1b4418a3e8276d"

MIXIN_KEY_ENC_TAB = [
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
    27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30,
    4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52,
]

# ════════════════════════════════════════════════════════════════
# 工具函数
# ════════════════════════════════════════════════════════════════

def ck_str_to_dict(ck_str: str) -> dict:
    return {k: urllib.parse.unquote(v)
            for k, v in re.findall(r"(\w+)=([^;]+)(?:;|$)", ck_str)}


def mask_string(s: str, head=2, tail=2) -> str:
    if not s:
        return ""
    if len(s) <= head + tail:
        return "*" * len(s)
    return s[:head] + "*" * 5 + s[-tail:]


# ════════════════════════════════════════════════════════════════
# WBI 签名
# ════════════════════════════════════════════════════════════════

def _get_mixin_key(orig: str):
    return reduce(lambda s, i: s + orig[i], MIXIN_KEY_ENC_TAB, "")[:32]


def _enc_wbi(params: dict, img_key: str, sub_key: str):
    mixin_key = _get_mixin_key(img_key + sub_key)
    params["wts"] = round(time.time())
    params = dict(sorted(params.items()))
    params = {
        k: "".join(filter(lambda c: c not in "!'()*", str(v)))
        for k, v in params.items()
    }
    query = urllib.parse.urlencode(params)
    wbi_sign = hashlib.md5((query + mixin_key).encode()).hexdigest()
    params["w_rid"] = wbi_sign
    return params


def _get_wbi_keys():
    resp = requests.get("https://api.bilibili.com/x/web-interface/nav",
                        headers={"User-Agent": USER_AGENT, "Referer": "https://www.bilibili.com/"})
    resp.raise_for_status()
    data = resp.json()
    img_url = data["data"]["wbi_img"]["img_url"]
    sub_url = data["data"]["wbi_img"]["sub_url"]
    return img_url.rsplit("/", 1)[1].split(".")[0], sub_url.rsplit("/", 1)[1].split(".")[0]


def get_w_rid_and_wts(other_data_dict: dict):
    img_key, sub_key = _get_wbi_keys()
    signed = _enc_wbi(dict(other_data_dict), img_key, sub_key)
    return signed, urllib.parse.urlencode(signed)


# ════════════════════════════════════════════════════════════════
# 配置路径
# ════════════════════════════════════════════════════════════════

def _get_app_path():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def get_config_path():
    app_path = _get_app_path()
    env_config_home = os.environ.get("BILILIVE_CONFIG_HOME")
    if env_config_home:
        return env_config_home
    if sys.platform.startswith("linux"):
        config_home = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
        config_path = os.path.join(config_home, "BiliLiveTool")
        os.makedirs(config_path, exist_ok=True)
        old_config = os.path.join(app_path, "config.json")
        new_config = os.path.join(config_path, "config.json")
        if not os.path.isfile(new_config) and os.path.isfile(old_config):
            try:
                shutil.copy(old_config, new_config)
            except Exception:
                pass
        return config_path
    return app_path


CONFIG_FILE = os.path.join(get_config_path(), "config.json")
_KEY_PATH = os.path.join(get_config_path(), ".cookie_key")


# ════════════════════════════════════════════════════════════════
# Cookie 加密
# ════════════════════════════════════════════════════════════════

def _load_or_create_key():
    if os.path.exists(_KEY_PATH):
        try:
            with open(_KEY_PATH, "rb") as f:
                return f.read()
        except Exception:
            pass
    key = Fernet.generate_key()
    try:
        with open(_KEY_PATH, "wb") as f:
            f.write(key)
        os.chmod(_KEY_PATH, stat.S_IRUSR | stat.S_IWUSR)
    except Exception:
        pass
    return key


def encrypt_cookie(plaintext: str) -> str:
    if not plaintext:
        return ""
    try:
        return Fernet(_load_or_create_key()).encrypt(plaintext.encode()).decode()
    except Exception:
        return plaintext


def decrypt_cookie(ciphertext: str) -> str:
    if not ciphertext:
        return ""
    if not ciphertext.startswith("gAAAAA"):
        return ciphertext
    try:
        return Fernet(_load_or_create_key()).decrypt(ciphertext.encode()).decode()
    except (InvalidToken, Exception):
        return ciphertext


# ════════════════════════════════════════════════════════════════
# 配置管理
# ════════════════════════════════════════════════════════════════

class Config:
    def __init__(self):
        self.data = self._load()

    def _load(self):
        default = {"users": {}, "current_uid": None}
        if not os.path.exists(CONFIG_FILE):
            return default
        try:
            with open(CONFIG_FILE, encoding="utf-8") as f:
                data = json.load(f)
                if "cookie" in data and "users" not in data:
                    try:
                        temp = ck_str_to_dict(data["cookie"])
                        uid = temp.get("DedeUserID", "default")
                        return {"users": {uid: {"uid": uid, "uname": "Saved User", "face": "",
                            "cookie": data.get("cookie", ""), "roomId": data.get("roomId", ""),
                            "csrf": data.get("csrf", ""), "last_title": data.get("last_title", ""),
                            "last_area_id": data.get("last_area_id", ""),
                            "last_area_name": data.get("last_area_name", [])}},
                            "current_uid": uid}
                    except Exception:
                        pass
                for u in data.get("users", {}).values():
                    if u.get("cookie"):
                        u["cookie"] = decrypt_cookie(u["cookie"])
                return data
        except Exception:
            return default

    def save(self):
        try:
            copy = json.loads(json.dumps(self.data))
            for u in copy.get("users", {}).values():
                if u.get("cookie"):
                    u["cookie"] = encrypt_cookie(u["cookie"])
            with open(CONFIG_FILE, "w", encoding="utf-8") as f:
                json.dump(copy, f, ensure_ascii=False, indent=2)
        except Exception:
            pass


# ════════════════════════════════════════════════════════════════
# 会话状态
# ════════════════════════════════════════════════════════════════

class SessionState:
    def __init__(self):
        self.room_id = ""
        self.csrf = ""
        self.uid = 0
        self.current_area_id = None
        self.current_area_names = []
        self.is_live = False

    def clear(self):
        self.room_id = ""
        self.csrf = ""
        self.uid = 0
        self.current_area_id = None
        self.current_area_names = []
        self.is_live = False


# ════════════════════════════════════════════════════════════════
# B站 API
# ════════════════════════════════════════════════════════════════

class BilibiliApi:
    def __init__(self):
        self.cookies = {}
        self.headers = dict(HEADER)

    def update_cookies(self, cookies: dict):
        self.cookies = cookies

    def _mask_url(self, url):
        if not url or "?" not in url:
            return url
        try:
            parsed = urllib.parse.urlparse(url)
            qs = urllib.parse.parse_qs(parsed.query)
            changed = False
            for k in ("uid", "room_id", "key", "token", "csrf", "csrf_token", "access_key", "qrcode_key"):
                if k in qs:
                    qs[k] = [mask_string(v, 2, 2) for v in qs[k]]
                    changed = True
            if changed:
                return urllib.parse.urlunparse(parsed._replace(query=urllib.parse.urlencode(qs, doseq=True)))
            return url
        except Exception:
            return url

    def _appsign(self, params: dict) -> dict:
        params = dict(params)
        params["appkey"] = APP_KEY
        params = dict(sorted(params.items()))
        query = urllib.parse.urlencode(params)
        params["sign"] = hashlib.md5((query + APP_SEC).encode()).hexdigest()
        return params

    def _req(self, method, url, params=None, data=None):
        try:
            logger.debug(f"API Request: {method} {self._mask_url(url)}")
            req_cookies = dict(self.cookies)
            if "buvid3" in self.cookies:
                req_cookies.setdefault("buvid3", self.cookies["buvid3"])
            if method == "GET":
                resp = requests.get(url, params=params, cookies=req_cookies,
                                    headers=self.headers, timeout=10)
            else:
                resp = requests.post(url, params=params, data=data, cookies=req_cookies,
                                     headers=self.headers, timeout=10)
            j = resp.json()
            logger.info(f"API Response: {self._mask_url(url)} -> code={j.get('code')}, msg={j.get('msg', j.get('message', ''))}")
            logger.info(f"API Response Body: {json.dumps(j, ensure_ascii=False)}")
            return True, j
        except ValueError:
            logger.error(f"JSON Decode Error. Status: {resp.status_code}")
            return False, {"code": -1, "msg": "API 返回格式错误"}
        except Exception as e:
            logger.error(f"Request Error: {url} -> {e}")
            return False, {"code": -1, "msg": str(e)}

    def get_passport_qrcode(self):
        return self._req("GET", "https://passport.bilibili.com/x/passport-login/web/qrcode/generate")

    def poll_passport_qrcode(self, qrcode_key):
        try:
            url = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll"
            resp = requests.get(url, params={"qrcode_key": qrcode_key}, headers=self.headers, timeout=10)
            j = resp.json()
            logger.info(f"API Response: poll_qrcode -> code={j.get('data', {}).get('code')}")
            logger.info(f"API Response Body: {json.dumps(j, ensure_ascii=False)}")
            return True, j, resp.cookies.get_dict()
        except Exception as e:
            logger.error(f"Request Error: {url} -> {e}")
            return False, {"code": -1, "msg": str(e)}, {}

    def get_user_info(self):
        return self._req("GET", "https://api.bilibili.com/x/web-interface/nav")

    def get_user_stat(self):
        return self._req("GET", "https://api.bilibili.com/x/web-interface/nav/stat")

    def get_room_id_by_uid(self, uid):
        return self._req("GET", f"https://api.live.bilibili.com/room/v2/Room/room_id_by_uid?uid={uid}")

    def get_room_info(self, room_id):
        return self._req("GET", f"https://api.live.bilibili.com/room/v1/Room/get_info?room_id={room_id}")

    def get_area_list(self):
        return self._req("GET", "https://api.live.bilibili.com/room/v1/Area/getList", params={"show_pinyin": 1})

    def update_title(self, room_id, title, csrf):
        return self._req("POST", "https://api.live.bilibili.com/room/v1/Room/update",
                         data={"room_id": room_id, "platform": "pc_link", "title": title,
                               "csrf_token": csrf, "csrf": csrf})

    def update_area(self, room_id, area_id, csrf):
        return self._req("POST", "https://api.live.bilibili.com/room/v1/Room/update",
                         data={"room_id": room_id, "area_id": area_id, "platform": "pc_link",
                               "csrf_token": csrf, "csrf": csrf})

    def start_live(self, room_id, area_id, csrf):
        s1, t_resp = self._req("GET", "https://api.bilibili.com/x/report/click/now")
        if not s1 or t_resp["code"] != 0:
            return False, t_resp
        ts = t_resp["data"]["now"]
        v_params = self._appsign({"system_version": 2, "ts": ts})
        s2, v_resp = self._req("GET",
            "https://api.live.bilibili.com/xlive/app-blink/v1/liveVersionInfo/getHomePageLiveVersion",
            params=v_params)
        if not s2 or v_resp["code"] != 0:
            return False, v_resp
        return self._req("POST", "https://api.live.bilibili.com/room/v1/Room/startLive",
                         data=self._appsign({"room_id": room_id, "platform": "pc_link", "area_v2": area_id,
                            "backup_stream": "0", "csrf_token": csrf, "csrf": csrf,
                            "build": v_resp["data"]["build"], "version": v_resp["data"]["curr_version"], "ts": ts}))

    def stop_live(self, room_id, csrf):
        return self._req("POST", "https://api.live.bilibili.com/room/v1/Room/stopLive",
                         data={"room_id": room_id, "platform": "pc_link", "csrf_token": csrf, "csrf": csrf})

    def get_buvid3(self):
        success, res = self._req("GET", "https://api.bilibili.com/x/frontend/finger/spi")
        if success and res["code"] == 0:
            return res["data"]["b_3"]
        return None


# ════════════════════════════════════════════════════════════════
# 服务层
# ════════════════════════════════════════════════════════════════

_SENSITIVE_KEYS = {"cookie", "csrf"}

def _strip_sensitive(user_data):
    if not user_data:
        return {}
    return {k: v for k, v in user_data.items() if k not in _SENSITIVE_KEYS}


class UserService:
    def __init__(self, api_client, config_manager, session_state):
        self.api = api_client
        self.config_manager = config_manager
        self.state = session_state

    def init_current_user(self):
        uid = self.config_manager.data.get("current_uid")
        users = self.config_manager.data.get("users", {})
        if uid and uid in users:
            self.state.clear()
            user = users[uid]
            logger.info(f"Init user: {user.get('uname')} ({mask_string(str(uid))})")
            self.api.update_cookies(ck_str_to_dict(user.get("cookie", "")))
            self.state.uid = int(uid)
            self.state.room_id = user.get("roomId", "")
            self.state.csrf = user.get("csrf", "")
            self.state.current_area_id = user.get("last_area_id")
            self.state.current_area_names = user.get("last_area_name", [])
        else:
            logger.info("No current user found")
            self.state.clear()

    def save_user_data(self, uid, full_data, cookie_str, room_id, csrf):
        uid = str(uid)
        config_data = self.config_manager.data
        config_data.setdefault("users", {})
        old_data = config_data["users"].get(uid, {})
        level_info = full_data.get("level_info", {})
        wallet = full_data.get("wallet", {})
        stat = full_data.get("stat", {})
        new_data = {
            "uid": uid, "uname": full_data.get("uname", "未知用户"),
            "face": full_data.get("face", ""),
            "cookie": cookie_str, "roomId": str(room_id), "csrf": csrf,
            "level": level_info.get("current_level", 0),
            "current_exp": level_info.get("current_exp", 0),
            "next_exp": level_info.get("next_exp", 0),
            "money": full_data.get("money", 0),
            "bcoin": wallet.get("bcoin_balance", 0),
            "following": stat.get("following", 0),
            "follower": stat.get("follower", 0),
            "dynamic_count": stat.get("dynamic_count", 0),
            "last_title": old_data.get("last_title", ""),
            "last_area_id": old_data.get("last_area_id", ""),
            "last_area_name": old_data.get("last_area_name", []),
        }
        config_data["users"][uid] = new_data
        config_data["current_uid"] = uid
        self.config_manager.save()
        self.state.uid = int(uid)
        self.state.room_id = str(room_id)
        self.state.csrf = csrf
        self.state.current_area_id = new_data["last_area_id"]
        self.state.current_area_names = new_data["last_area_name"]
        return new_data

    def fetch_full_user_data(self):
        s1, nav = self.api.get_user_info()
        if not s1 or nav.get("code") != 0:
            return False, nav
        s2, stat = self.api.get_user_stat()
        stat_data = stat.get("data", {}) if s2 and stat.get("code") == 0 else {}
        full = nav["data"]
        full["stat"] = stat_data
        return True, full

    def fetch_room_id(self, cookies_dict):
        uid = cookies_dict.get("DedeUserID")
        if uid:
            success, res = self.api.get_room_id_by_uid(uid)
            if success:
                if res["code"] == 0:
                    return str(res["data"]["room_id"])
                if res.get("code") == 404:
                    raise Exception("该账号未开通直播间")
        success, res = self.api.get_user_info()
        if success and res["code"] == 0:
            rid = str(res["data"].get("live_room", {}).get("roomid", ""))
            if rid == "0":
                raise Exception("该账号未开通直播间")
            return rid
        return ""

    def load_saved_config(self):
        uid = self.config_manager.data.get("current_uid")
        users = self.config_manager.data.get("users", {})
        if uid and uid in users:
            return {"code": 0, "data": _strip_sensitive(users[uid])}
        return {"code": 0, "data": {}}

    def refresh_current_user(self):
        uid = self.config_manager.data.get("current_uid")
        if not uid or uid not in self.config_manager.data.get("users", {}):
            return {"code": -1, "msg": "未登录"}
        ok, full_data = self.fetch_full_user_data()
        if ok:
            user = self.config_manager.data["users"][uid]
            saved_user = self.save_user_data(uid, full_data, user["cookie"], user["roomId"], user["csrf"])
            return {"code": 0, "data": _strip_sensitive(saved_user)}
        return {"code": -1, "msg": "刷新失败"}

    def get_account_list(self):
        users = self.config_manager.data.get("users", {})
        lst = [_strip_sensitive(v) for v in users.values()]
        return {"code": 0, "data": {"list": lst, "current_uid": self.config_manager.data.get("current_uid")}}

    def switch_account(self, uid):
        users = self.config_manager.data.get("users", {})
        if uid in users:
            self.config_manager.data["current_uid"] = uid
            self.config_manager.save()
            self.init_current_user()
            return {"code": 0, "data": _strip_sensitive(users[uid])}
        return {"code": -1, "msg": "账户不存在"}

    def logout(self, uid):
        users = self.config_manager.data.get("users", {})
        if uid in users:
            del users[uid]
            if self.config_manager.data.get("current_uid") == uid:
                self.config_manager.data["current_uid"] = None
                self.state.clear()
                self.api.update_cookies({})
            self.config_manager.save()
            return {"code": 0}
        return {"code": -1, "msg": "账户不存在"}


class LiveService:
    def __init__(self, api_client, config_manager, session_state):
        self.api = api_client
        self.config_manager = config_manager
        self.state = session_state
        self.partition_map = {}

    def _refresh_partitions_internal(self):
        success, res = self.api.get_area_list()
        if success and res.get("code") == 0:
            self.partition_map = {}
            for p in res["data"]:
                self.partition_map[p["name"]] = {s["name"]: s["id"] for s in p["list"]}
            uid = str(self.config_manager.data.get("current_uid", ""))
            if uid and uid in self.config_manager.data.get("users", {}):
                last_aid = self.config_manager.data["users"][uid].get("last_area_id")
                if last_aid:
                    self.state.current_area_id = last_aid

    def _get_names_by_id(self, area_id):
        if not self.partition_map:
            self._refresh_partitions_internal()
        target = str(area_id)
        for p_name, sub_map in self.partition_map.items():
            for s_name, aid in sub_map.items():
                if str(aid) == target:
                    return [p_name, s_name]
        return []

    def get_partitions(self):
        if not self.partition_map:
            self._refresh_partitions_internal()
        return {"code": 0, "data": {p: list(s.keys()) for p, s in self.partition_map.items()}}

    def update_title(self, title):
        if not self.config_manager.data.get("current_uid"):
            return {"code": -1, "msg": "未登录"}
        success, res = self.api.update_title(self.state.room_id, title, self.state.csrf)
        if success and res["code"] == 0:
            uid = str(self.config_manager.data.get("current_uid", ""))
            if uid and uid in self.config_manager.data.get("users", {}):
                self.config_manager.data["users"][uid]["last_title"] = title
                self.config_manager.save()
            return {"code": 0}
        return {"code": -1, "msg": res.get("msg")}

    def update_area(self, p_name, s_name):
        if not self.config_manager.data.get("current_uid"):
            return {"code": -1, "msg": "未登录"}
        if not self.partition_map:
            self._refresh_partitions_internal()
        aid = self.partition_map.get(p_name, {}).get(s_name)
        if not aid:
            return {"code": -1, "msg": "无效分区"}
        success, res = self.api.update_area(self.state.room_id, aid, self.state.csrf)
        if success and res["code"] == 0:
            self.state.current_area_id = aid
            self.state.current_area_names = [p_name, s_name]
            uid = str(self.config_manager.data.get("current_uid", ""))
            if uid and uid in self.config_manager.data.get("users", {}):
                self.config_manager.data["users"][uid]["last_area_id"] = aid
                self.config_manager.data["users"][uid]["last_area_name"] = [p_name, s_name]
                self.config_manager.save()
            return {"code": 0}
        return {"code": -1, "msg": res.get("msg")}

    def start_live(self, p_name=None, s_name=None, title=None):
        if not self.state.room_id:
            return {"code": -1, "msg": "请先登录"}
        # 保存标题到配置
        if title:
            uid = str(self.config_manager.data.get("current_uid", ""))
            if uid and uid in self.config_manager.data.get("users", {}):
                self.config_manager.data["users"][uid]["last_title"] = title
                self.config_manager.save()
        if p_name and s_name:
            if not self.partition_map:
                self._refresh_partitions_internal()
            aid = self.partition_map.get(p_name, {}).get(s_name)
            if not aid:
                self._refresh_partitions_internal()
                aid = self.partition_map.get(p_name, {}).get(s_name)
            if aid:
                self.state.current_area_id = aid
                self.state.current_area_names = [p_name, s_name]
            else:
                return {"code": -1, "msg": f"无法识别分区: {p_name}-{s_name}"}
        if not self.state.current_area_id:
            uid = str(self.config_manager.data.get("current_uid", ""))
            if uid in self.config_manager.data.get("users", {}):
                self.state.current_area_id = self.config_manager.data["users"][uid].get("last_area_id", "235")
                self.state.current_area_names = self.config_manager.data["users"][uid].get("last_area_name", [])
            else:
                self.state.current_area_id = "235"
        success, res = self.api.start_live(self.state.room_id, self.state.current_area_id, self.state.csrf)
        if not success:
            return {"code": -1, "msg": "网络错误"}
        if res["code"] == 0:
            self.state.is_live = True
            if self.state.current_area_id:
                found = self._get_names_by_id(self.state.current_area_id)
                if found:
                    self.state.current_area_names = found
            uid = str(self.config_manager.data.get("current_uid", ""))
            if uid and uid in self.config_manager.data.get("users", {}):
                self.config_manager.data["users"][uid]["last_area_id"] = self.state.current_area_id
                self.config_manager.data["users"][uid]["last_area_name"] = self.state.current_area_names
                self.config_manager.save()
            rtmp_data = res["data"].get("rtmp", {})
            protocols = res["data"].get("protocols", [])
            rtmp_addr = rtmp_data.get("addr", "")
            rtmp_code = rtmp_data.get("code", "")
            rtmp2_addr = rtmp2_code = ""
            for p in protocols:
                if p.get("protocol") == "rtmp" and p.get("addr") and p.get("code"):
                    rtmp2_addr, rtmp2_code = p["addr"], p["code"]
                    break
            srt_addr = srt_code = ""
            for p in protocols:
                if p.get("protocol") == "srt" and p.get("addr") and p.get("code"):
                    srt_addr, srt_code = p["addr"], p["code"]
                    break
            return {"code": 0, "data": {
                "rtmp1": {"addr": rtmp_addr, "code": rtmp_code},
                "rtmp2": {"addr": rtmp2_addr, "code": rtmp2_code},
                "srt": {"addr": srt_addr, "code": srt_code}}}
        if res["code"] == 60024:
            qr_url = res["data"].get("url") or res["data"].get("qr", "")
            return {"code": 60024, "qr": qr_url}
        if res["code"] == 60043:
            return {"code": 60043, "qr": f"https://www.bilibili.com/blackboard/live/face-auth-middle.html?source_event=400&mid={self.state.uid}"}
        return {"code": -1, "msg": res.get("msg")}

    def stop_live(self):
        success, res = self.api.stop_live(self.state.room_id, self.state.csrf)
        if success and res["code"] == 0:
            self.state.is_live = False
            return {"code": 0}
        return {"code": -1}

    def check_live_status(self):
        if not self.state.room_id:
            return {"code": -1}
        success, res = self.api.get_room_info(self.state.room_id)
        if success and res.get("code") == 0:
            data = res.get("data", {})
            self.state.is_live = data.get("live_status") == 1
            return {"code": 0, "is_live": self.state.is_live}
        return {"code": -1}


class AuthService:
    def __init__(self, api_client, user_service, live_service, session_state):
        self.api = api_client
        self.user_service = user_service
        self.live_service = live_service
        self.state = session_state

    def get_login_qrcode(self):
        success, res = self.api.get_passport_qrcode()
        if success and res["code"] == 0:
            return {"code": 0, "data": res["data"]}
        return {"code": -1}

    def poll_login_status(self, key):
        success, res, cookies = self.api.poll_passport_qrcode(key)
        if not success:
            return {"code": -1, "msg": "网络请求失败"}
        data = res.get("data", {})
        if data.get("code") == 0:
            try:
                self.state.clear()
                self.api.update_cookies(cookies)
                csrf = cookies.get("bili_jct", "")
                room_id = self.user_service.fetch_room_id(cookies)
                if not room_id:
                    return {"code": -1, "msg": "获取直播间ID失败"}
                ok, full_data = self.user_service.fetch_full_user_data()
                if ok:
                    uid = str(cookies.get("DedeUserID"))
                    cookie_str = "; ".join(f"{k}={v}" for k, v in cookies.items())
                    saved_user = self.user_service.save_user_data(uid, full_data, cookie_str, room_id, csrf)
                    self.live_service._refresh_partitions_internal()
                    return {"code": 0, "data": _strip_sensitive(saved_user)}
                return {"code": -1, "msg": "获取用户信息失败"}
            except Exception as e:
                return {"code": -1, "msg": str(e)}
        return {"code": data.get("code"), "msg": data.get("message")}


# ════════════════════════════════════════════════════════════════
# Qt UI
# ════════════════════════════════════════════════════════════════

class DockWidget(QtWidgets.QWidget):
    stream_started = QtCore.Signal(dict)
    stream_stopped = QtCore.Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._auth = None
        self._live = None
        self._user = None
        self._config = None
        self._poll_timer = QtCore.QTimer(self)
        self._poll_timer.timeout.connect(self._poll_login)
        self._qrcode_key = None
        self._poll_count = 0
        self._partition_cache = {}
        self._rtmp_data = {}
        self._rtmp_addr = ""
        self._rtmp_code = ""
        self._init_ui()

    def set_services(self, auth_svc, live_svc, user_svc, config_mgr):
        self._auth = auth_svc
        self._live = live_svc
        self._user = user_svc
        self._config = config_mgr

    def _init_ui(self):
        main = QtWidgets.QVBoxLayout(self)
        main.setContentsMargins(8, 8, 8, 8)
        main.setSpacing(8)

        title = QtWidgets.QLabel("B站直播工具 - OBS 插件")
        title.setAlignment(QtCore.Qt.AlignCenter)
        title.setStyleSheet("font-size:15px;font-weight:bold;color:#fff;padding:4px")
        main.addWidget(title)

        # ── 登录区域 ──
        login_group = QtWidgets.QGroupBox("账号")
        login_layout = QtWidgets.QVBoxLayout(login_group)

        self._btn_login = QtWidgets.QPushButton("扫码登录")
        self._btn_login.clicked.connect(self._start_login)
        login_layout.addWidget(self._btn_login)

        self._login_qr = QtWidgets.QLabel()
        self._login_qr.setAlignment(QtCore.Qt.AlignCenter)
        self._login_qr.setFixedSize(200, 200)
        self._login_qr.setStyleSheet("background:white;border:2px solid #669DF6;border-radius:8px")
        self._login_qr.hide()
        login_layout.addWidget(self._login_qr, alignment=QtCore.Qt.AlignCenter)

        self._login_status = QtWidgets.QLabel()
        self._login_status.setAlignment(QtCore.Qt.AlignCenter)
        self._login_status.setStyleSheet("color:#888;font-size:12px")
        self._login_status.hide()
        login_layout.addWidget(self._login_status)

        self._user_info_label = QtWidgets.QLabel()
        self._user_info_label.setAlignment(QtCore.Qt.AlignCenter)
        self._user_info_label.setStyleSheet("color:#FFB74D;font-size:13px;font-weight:bold")
        self._user_info_label.hide()
        login_layout.addWidget(self._user_info_label)

        acc_row = QtWidgets.QHBoxLayout()
        self._account_combo = QtWidgets.QComboBox()
        self._account_combo.currentIndexChanged.connect(self._on_account_changed)
        self._account_combo.hide()
        acc_row.addWidget(self._account_combo)
        self._btn_logout = QtWidgets.QPushButton("登出")
        self._btn_logout.clicked.connect(self._do_logout)
        self._btn_logout.hide()
        acc_row.addWidget(self._btn_logout)
        login_layout.addLayout(acc_row)
        main.addWidget(login_group)

        # ── 人脸验证 QR ──
        self._verify_group = QtWidgets.QGroupBox("人脸验证")
        verify_layout = QtWidgets.QVBoxLayout(self._verify_group)
        self._verify_qr = QtWidgets.QLabel()
        self._verify_qr.setAlignment(QtCore.Qt.AlignCenter)
        self._verify_qr.setFixedSize(200, 200)
        self._verify_qr.setStyleSheet("background:white;border:2px solid #F28B82;border-radius:8px")
        verify_layout.addWidget(self._verify_qr, alignment=QtCore.Qt.AlignCenter)
        self._verify_hint = QtWidgets.QLabel("请使用 B站 App 扫描完成人脸验证")
        self._verify_hint.setAlignment(QtCore.Qt.AlignCenter)
        self._verify_hint.setStyleSheet("color:#FFB74D;font-size:12px")
        verify_layout.addWidget(self._verify_hint)
        self._verify_group.hide()
        main.addWidget(self._verify_group)

        # ── 直播控制 ──
        stream_group = QtWidgets.QGroupBox("直播控制")
        stream_layout = QtWidgets.QVBoxLayout(stream_group)

        title_row = QtWidgets.QHBoxLayout()
        title_row.addWidget(QtWidgets.QLabel("标题:"))
        self._title_edit = QtWidgets.QLineEdit()
        self._title_edit.setPlaceholderText("输入直播标题...")
        title_row.addWidget(self._title_edit)
        stream_layout.addLayout(title_row)

        area_row = QtWidgets.QHBoxLayout()
        self._parent_combo = QtWidgets.QComboBox()
        self._parent_combo.setPlaceholderText("主分区")
        self._parent_combo.currentTextChanged.connect(self._on_parent_area_changed)
        area_row.addWidget(self._parent_combo)
        self._sub_combo = QtWidgets.QComboBox()
        self._sub_combo.setPlaceholderText("子分区")
        area_row.addWidget(self._sub_combo)
        stream_layout.addLayout(area_row)

        btn_row = QtWidgets.QHBoxLayout()
        self._btn_start = QtWidgets.QPushButton("开始直播")
        self._btn_start.setObjectName("btnStart")
        self._btn_start.clicked.connect(self._do_start_live)
        btn_row.addWidget(self._btn_start)
        self._btn_stop = QtWidgets.QPushButton("停止直播")
        self._btn_stop.setObjectName("btnStop")
        self._btn_stop.clicked.connect(self._do_stop_live)
        self._btn_stop.hide()
        btn_row.addWidget(self._btn_stop)
        stream_layout.addLayout(btn_row)

        self._stream_status = QtWidgets.QLabel()
        self._stream_status.setAlignment(QtCore.Qt.AlignCenter)
        self._stream_status.setStyleSheet("color:#888;font-size:12px")
        self._stream_status.hide()
        stream_layout.addWidget(self._stream_status)
        main.addWidget(stream_group)

        # ── RTMP 信息 ──
        rtmp_group = QtWidgets.QGroupBox("推流信息")
        rtmp_layout = QtWidgets.QVBoxLayout(rtmp_group)

        addr_row = QtWidgets.QHBoxLayout()
        self._rtmp_addr_label = QtWidgets.QLabel()
        self._rtmp_addr_label.setWordWrap(True)
        self._rtmp_addr_label.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self._rtmp_addr_label.setStyleSheet("background:#2d2d2d;border:1px solid #444;border-radius:4px;"
            "padding:4px 6px;font-family:monospace;font-size:11px;color:#FFB74D")
        self._rtmp_addr_label.hide()
        addr_row.addWidget(self._rtmp_addr_label)
        self._btn_copy_addr = QtWidgets.QPushButton("复制")
        self._btn_copy_addr.setFixedWidth(50)
        self._btn_copy_addr.clicked.connect(lambda: self._copy_text(self._rtmp_addr))
        self._btn_copy_addr.hide()
        addr_row.addWidget(self._btn_copy_addr)
        rtmp_layout.addLayout(addr_row)

        code_row = QtWidgets.QHBoxLayout()
        self._rtmp_code_label = QtWidgets.QLabel()
        self._rtmp_code_label.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self._rtmp_code_label.setStyleSheet("background:#2d2d2d;border:1px solid #444;border-radius:4px;"
            "padding:4px 6px;font-family:monospace;font-size:11px;color:#FFB74D")
        self._rtmp_code_label.hide()
        code_row.addWidget(self._rtmp_code_label)
        self._btn_copy_code = QtWidgets.QPushButton("复制")
        self._btn_copy_code.setFixedWidth(50)
        self._btn_copy_code.clicked.connect(lambda: self._copy_text(self._rtmp_code))
        self._btn_copy_code.hide()
        code_row.addWidget(self._btn_copy_code)
        rtmp_layout.addLayout(code_row)

        self._rtmp_srt_label = QtWidgets.QLabel()
        self._rtmp_srt_label.setWordWrap(True)
        self._rtmp_srt_label.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self._rtmp_srt_label.setStyleSheet("background:#2d2d2d;border:1px solid #444;border-radius:4px;"
            "padding:4px 6px;font-family:monospace;font-size:11px;color:#81C784")
        self._rtmp_srt_label.hide()
        rtmp_layout.addWidget(self._rtmp_srt_label)
        main.addWidget(rtmp_group)

        self._status_bar = QtWidgets.QLabel("就绪")
        self._status_bar.setStyleSheet("color:#666;font-size:11px;padding:2px")
        main.addWidget(self._status_bar)
        main.addStretch()
        self._set_logged_out()

    # ── 登录 ──
    def _start_login(self):
        if not self._auth:
            return
        self._btn_login.setEnabled(False)
        self._btn_login.setText("获取中...")
        result = self._auth.get_login_qrcode()
        logger.info(f"get_login_qrcode: {result}")
        if result["code"] != 0:
            self._set_login_error("获取二维码失败")
            return
        data = result["data"]
        self._qrcode_key = data["qrcode_key"]
        login_url = data["url"]
        self._btn_login.hide()
        self._show_qr_in_label(self._login_qr, login_url)
        self._login_status.setText("请使用 Bilibili 客户端扫码登录")
        self._login_status.show()
        self._poll_count = 0
        self._poll_timer.start(2000)

    def _poll_login(self):
        self._poll_count += 1
        result = self._auth.poll_login_status(self._qrcode_key)
        code = result.get("code")
        logger.info(f"poll_login #{self._poll_count}: code={code} msg={result.get('msg')}")
        if code == 0:
            self._poll_timer.stop()
            self._on_login_done(result.get("data", {}))
        elif code == 86038:
            self._poll_timer.stop()
            self._set_login_error("二维码已过期")
        elif code == 86090:
            self._login_status.setText("已扫码，请在手机上确认...")
        elif self._poll_count >= 150:
            self._poll_timer.stop()
            self._set_login_error("登录超时")
        elif code is not None and code < 0:
            self._poll_timer.stop()
            self._set_login_error(result.get("msg", "登录失败"))

    def _on_login_done(self, data):
        self._hide_login_ui()
        self._verify_group.hide()
        self._user_info_label.setText(f"{data.get('uname', '')}  Lv.{data.get('level', 0)}")
        self._user_info_label.show()
        self._refresh_account_list()
        self._load_partitions()
        self._title_edit.setText(data.get("last_title", ""))
        self._btn_start.show()
        names = data.get("last_area_name", [])
        if len(names) >= 1:
            self._parent_combo.setCurrentText(names[0])
        if len(names) >= 2:
            self._sub_combo.setCurrentText(names[1])
        self._status_bar.setText("已登录 — 就绪")

    def _set_login_error(self, msg):
        self._hide_login_ui()
        self._btn_login.setEnabled(True)
        self._btn_login.setText("扫码登录")
        self._btn_login.show()
        self._status_bar.setText(msg)

    def _show_verify_qr(self, url):
        self._verify_group.show()
        self._show_qr_in_label(self._verify_qr, url)

    def _show_qr_in_label(self, label, url):
        try:
            qr_url = "https://quickchart.io/qr?text=" + urllib.parse.quote(url, safe="")
            resp = requests.get(qr_url, timeout=10)
            resp.raise_for_status()
            pm = QtGui.QPixmap()
            pm.loadFromData(resp.content)
            label.setPixmap(pm.scaled(200, 200, QtCore.Qt.KeepAspectRatio, QtCore.Qt.SmoothTransformation))
            label.show()
        except Exception as e:
            logger.error(f"获取二维码失败: {e}")
            self._login_status.setText(f"二维码加载失败, 请手动打开:\n{url}")
            self._login_status.show()

    def _hide_login_ui(self):
        self._login_qr.hide()
        self._login_status.hide()
        self._btn_login.hide()

    def _set_logged_out(self):
        self._btn_start.hide()
        self._btn_stop.hide()
        self._stream_status.hide()
        self._rtmp_addr_label.hide()
        self._btn_copy_addr.hide()
        self._rtmp_code_label.hide()
        self._btn_copy_code.hide()
        self._rtmp_srt_label.hide()
        self._verify_group.hide()
        self._parent_combo.clear()
        self._sub_combo.clear()
        self._title_edit.clear()

    # ── 账户管理 ──
    def _refresh_account_list(self):
        if not self._user:
            return
        self._account_combo.blockSignals(True)
        self._account_combo.clear()
        res = self._user.get_account_list()
        accounts = res.get("data", {}).get("list", [])
        current_uid = res.get("data", {}).get("current_uid")
        for i, acc in enumerate(accounts):
            self._account_combo.addItem(f"{acc.get('uname', '')} (Lv.{acc.get('level', 0)})", acc.get("uid"))
            if acc.get("uid") == current_uid:
                self._account_combo.setCurrentIndex(i)
        self._account_combo.blockSignals(False)
        if self._account_combo.count() > 1:
            self._account_combo.show()
            self._btn_logout.show()

    def _on_account_changed(self, idx):
        if idx < 0:
            return
        uid = self._account_combo.itemData(idx)
        if not uid or not self._user:
            return
        res = self._user.switch_account(str(uid))
        if res["code"] == 0:
            self._on_login_done(res["data"])

    def _do_logout(self):
        current = self._account_combo.currentData()
        if not current or not self._user:
            return
        self._user.logout(str(current))
        self._account_combo.hide()
        self._btn_logout.hide()
        self._user_info_label.hide()
        self._set_logged_out()
        self._btn_login.show()
        self._status_bar.setText("已登出")

    # ── 分区 ──
    def _load_partitions(self):
        if not self._live:
            return
        res = self._live.get_partitions()
        self._partition_cache = res.get("data", {})
        self._parent_combo.blockSignals(True)
        self._parent_combo.clear()
        self._parent_combo.addItem("")
        for name in self._partition_cache:
            self._parent_combo.addItem(name)
        self._parent_combo.blockSignals(False)

    def _on_parent_area_changed(self, name):
        self._sub_combo.blockSignals(True)
        self._sub_combo.clear()
        if name and name in self._partition_cache:
            self._sub_combo.addItem("")
            for sub_name in self._partition_cache[name]:
                self._sub_combo.addItem(sub_name)
        self._sub_combo.blockSignals(False)

    # ── 直播控制 ──
    def _do_start_live(self):
        if not self._live:
            return
        self._btn_start.setEnabled(False)
        self._stream_status.setText("正在开播...")
        self._stream_status.show()
        res = self._live.start_live(self._parent_combo.currentText(), self._sub_combo.currentText(), self._title_edit.text())
        if res.get("code") == 0 and res.get("data"):
            self._verify_group.hide()
            data = res["data"]
            self._rtmp_data = data
            rtmp1 = data.get("rtmp1", {})
            rtmp2 = data.get("rtmp2", {})
            srt_info = data.get("srt", {})
            self._rtmp_addr = rtmp1.get("addr", "")
            self._rtmp_code = rtmp1.get("code", "")
            self._rtmp_addr_label.setText(f"RTMP 地址: {self._rtmp_addr}")
            self._rtmp_addr_label.show()
            self._btn_copy_addr.show()
            self._rtmp_code_label.setText(f"推流码: {self._rtmp_code}")
            self._rtmp_code_label.show()
            self._btn_copy_code.show()
            if rtmp2.get("addr"):
                self._rtmp_srt_label.setText(
                    f"备路 RTMP: {rtmp2.get('addr', '')}  |  推流码: {rtmp2.get('code', '')}"
                    f"{'  |  SRT: ' + srt_info.get('addr', '') + '  |  码: ' + srt_info.get('code', '') if srt_info.get('addr') else ''}")
                self._rtmp_srt_label.show()
            self._btn_start.hide()
            self._btn_stop.show()
            self._stream_status.setText("直播中")
            self._stream_status.setStyleSheet("color:#81C784;font-size:12px")
            self._status_bar.setText("直播已开启 — 点击「应用到 OBS」配置推流")
        elif res.get("code") in (60024, 60043):
            verify_url = res.get("qr", "")
            logger.info(f"Face verify URL: {verify_url}")
            self._stream_status.setText("需要人脸验证, 请扫描下方二维码")
            self._stream_status.setStyleSheet("color:#FFB74D;font-size:12px")
            self._status_bar.setText("验证完成后重新点击开播")
            self._show_verify_qr(verify_url)
        else:
            self._stream_status.setText(f"开播失败: {res.get('msg', '未知错误')}")
            self._stream_status.setStyleSheet("color:#F28B82;font-size:12px")
        self._btn_start.setEnabled(True)

    def _on_stream_started_on_reload(self):
        """重载脚本时检测到正在直播, 恢复 UI 状态"""
        self._btn_start.hide()
        self._btn_stop.show()
        self._stream_status.setText("直播中 (重新加载前已开播)")
        self._stream_status.setStyleSheet("color:#81C784;font-size:12px")
        self._stream_status.show()
        self._status_bar.setText("直播中 — 请等待流结束或在面板中停止")

    def _do_stop_live(self):
        if not self._live:
            return
        res = self._live.stop_live()
        if res.get("code") == 0:
            self._btn_stop.hide()
            self._btn_start.show()
            self._stream_status.setText("已停止直播")
            self._stream_status.setStyleSheet("color:#888;font-size:12px")
            self._rtmp_addr_label.hide()
            self._btn_copy_addr.hide()
            self._rtmp_code_label.hide()
            self._btn_copy_code.hide()
            self._rtmp_srt_label.hide()
            self._status_bar.setText("直播已停止")
            self.stream_stopped.emit()

    @staticmethod
    def _copy_text(text):
        QtWidgets.QApplication.clipboard().setText(text)


# ════════════════════════════════════════════════════════════════
# OBS 生命周期
# ════════════════════════════════════════════════════════════════

_dock = None
_dock_name = "bili_live_dock"
_services = {}


def _init_services():
    if _services:
        return _services
    api = BilibiliApi()
    cfg = Config()
    state = SessionState()
    user = UserService(api, cfg, state)
    live = LiveService(api, cfg, state)
    auth = AuthService(api, user, live, state)
    user.init_current_user()
    _services.update(api=api, config=cfg, state=state,
                     user=user, live=live, auth=auth)
    return _services


def _register_dock():
    if hasattr(obs, "obs_frontend_add_dock"):
        try:
            obs.obs_frontend_add_dock(_dock, _dock_name, "B站直播工具")
            return
        except (TypeError, RuntimeError):
            pass
    if hasattr(obs, "obs_frontend_add_dock_by_id"):
        try:
            obs.obs_frontend_add_dock_by_id(_dock_name, "B站直播工具", _dock)
            return
        except (TypeError, RuntimeError):
            pass
    logger.info("以独立窗口模式运行")
    _dock.setWindowTitle("B站直播工具")
    _dock.setWindowFlags(_dock.windowFlags() | _dock.windowFlags().WindowStaysOnTopHint)
    _dock.resize(420, 650)
    _dock.show()


def script_description():
    return ("B站直播工具 for OBS\n\n"
            "• 扫码登录 / 多账号切换\n"
            "• 直播标题 / 分区设置\n"
            "• 一键开播 / 停播\n"
            "• RTMP 推流地址自动配置到 OBS")


def script_load(settings):
    global _dock
    if not IN_OBS:
        return
    svc = _init_services()
    _dock = DockWidget()
    _dock.set_services(svc["auth"], svc["live"], svc["user"], svc["config"])
    _register_dock()
    logger.info("插件已加载")
    current_uid = svc["config"].data.get("current_uid")
    if current_uid and current_uid in svc["config"].data.get("users", {}):
        user_data = svc["config"].data["users"][current_uid]
        _dock._on_login_done(_strip_sensitive(user_data))
        live_status = svc["live"].check_live_status()
        if live_status.get("is_live"):
            _dock._on_stream_started_on_reload()
            logger.info("检测到直播中状态, 已恢复")


def script_unload():
    global _dock, _services
    try:
        if _dock:
            _dock._poll_timer.stop()
            _dock.close()
            _dock.deleteLater()
            _dock = None
    except Exception:
        pass
    try:
        if _services.get("config"):
            _services["config"].save()
    except Exception:
        pass
    _services.clear()
