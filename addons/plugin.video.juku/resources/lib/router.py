# -*- coding: utf-8 -*-
import os
import json
import time
import re
import urllib.parse

def safe_int(val, default=0):
    try:
        return int(val)
    except (ValueError, TypeError):
        return default

def parse_episode_number(raw_name):
    """从文件名或集数标签中正则提取集数数字"""
    if not raw_name:
        return 0
    m = re.search(r'(?:S\d+)?E(\d+)', raw_name, re.IGNORECASE)
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            pass
    m = re.search(r'EP(\d+)', raw_name, re.IGNORECASE)
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            pass
    m = re.search(r'第\s*(\d+)\s*[集话]', raw_name)
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            pass
    m = re.search(r'(?:^|[^\d])(\d{1,4})(?:[^\d]|$)', raw_name)
    if m:
        try:
            return int(m.group(1))
        except ValueError:
            pass
    return 0

VIDEO_EXTS = {'.mp4', '.mkv', '.avi', '.mov', '.flv', '.wmv', '.webm', '.ts', '.m2ts', '.m4v', '.rmvb', '.vob', '.iso', '.mpg', '.mpeg', '.3gp', '.asf', '.ogv'}
AUDIO_EXTS = {'.mp3', '.flac', '.wav', '.ape', '.dts', '.m4a', '.aac', '.ogg', '.wma', '.opus', '.alac', '.aiff', '.dsf', '.dff', '.ac3', '.pcm', '.mid', '.midi', '.mka'}
IMAGE_EXTS = {'.jpg', '.jpeg', '.png', '.gif', '.webp', '.bmp', '.svg', '.tiff'}

def get_file_category(file_name):
    ext = os.path.splitext(file_name)[1].lower()
    if ext in VIDEO_EXTS:
        return "video", ext
    elif ext in AUDIO_EXTS:
        return "audio", ext
    elif ext in IMAGE_EXTS:
        return "image", ext
    return "other", ext

def should_mark_pan_link_invalid(err_msg):
    if not err_msg:
        return False
    normalized = err_msg.replace('：', ':')
    is_explicitly_invalid = "分享" in normalized and ("已失效" in normalized or "无可播放文件" in normalized)
    is_share_token_error = "分享 token 失败: HTTP 404" in normalized or "分享 token 失败: HTTP 403" in normalized
    return is_explicitly_invalid or is_share_token_error


# ------------------------------------------------------------------------------
# 1. 标签本地 TTL 缓存管理器 (TagsCacheManager)
# ------------------------------------------------------------------------------
class TagsCacheManager:
    """处理视频标签的本地持久化与 TTL 缓存机制"""
    def __init__(self, cache_dir, ttl=3600):
        self.cache_dir = cache_dir
        self.cache_file = os.path.join(cache_dir, "tags_cache.json")
        self.ttl = ttl

    def get_cached_tags(self):
        """读取未过期的标签缓存"""
        if not os.path.exists(self.cache_file):
            return None
        try:
            with open(self.cache_file, 'r', encoding='utf-8') as f:
                data = json.load(f)
            if time.time() - data.get("timestamp", 0) < self.ttl:
                return data.get("tags")
        except Exception:
            pass
        return None

    def save_tags(self, tags):
        """将标签写入本地文件"""
        if not os.path.exists(self.cache_dir):
            try:
                os.makedirs(self.cache_dir)
            except Exception:
                pass
        try:
            data = {
                "timestamp": time.time(),
                "tags": tags
            }
            with open(self.cache_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except Exception:
            pass


# ------------------------------------------------------------------------------
# 1.5 播放历史记录管理器 (HistoryManager)
# ------------------------------------------------------------------------------
class HistoryManager:
    """管理视频的播放历史记录"""
    def __init__(self, cache_dir, max_items=50):
        self.cache_dir = cache_dir
        self.history_file = os.path.join(cache_dir, "play_history.json")
        self.max_items = max_items

    def load_history(self):
        if not os.path.exists(self.history_file):
            return []
        try:
            with open(self.history_file, 'r', encoding='utf-8') as f:
                return json.load(f)
        except Exception:
            return []

    def save_history(self, history):
        if not os.path.exists(self.cache_dir):
            try:
                os.makedirs(self.cache_dir)
            except Exception:
                pass
        try:
            with open(self.history_file, 'w', encoding='utf-8') as f:
                json.dump(history, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def add_record(self, record):
        """
        添加一条播放记录。如果已存在相同的记录（通过相同播放参数判定），则将其移动到最前。
        """
        history = self.load_history()
        new_history = []
        
        def is_same(r1, r2):
            p1 = r1.get("play_params") or {}
            p2 = r2.get("play_params") or {}
            a1 = p1.get("action")
            a2 = p2.get("action")
            if a1 != a2:
                return False
            if a1 == "play_hls":
                return p1.get("url") == p2.get("url")
            elif a1 == "pan_play":
                return p1.get("url") == p2.get("url") and p1.get("file_id") == p2.get("file_id")
            elif a1 == "line_episodes":
                return p1.get("id") == p2.get("id") and p1.get("line") == p2.get("line")
            elif a1 == "list_files":
                return p1.get("url") == p2.get("url")
            return False

        for item in history:
            if not is_same(item, record):
                new_history.append(item)
                
        new_history.insert(0, record)
        new_history = new_history[:self.max_items]
        self.save_history(new_history)

    def clear_history(self):
        self.save_history([])


# ------------------------------------------------------------------------------
# 1.6 搜索历史记录管理器 (SearchHistoryManager)
# ------------------------------------------------------------------------------
class SearchHistoryManager:
    """管理搜索历史记录"""
    def __init__(self, cache_dir, max_items=20):
        self.cache_dir = cache_dir
        self.history_file = os.path.join(cache_dir, "search_history.json")
        self.max_items = max_items

    def load_history(self):
        if not os.path.exists(self.history_file):
            return []
        try:
            with open(self.history_file, 'r', encoding='utf-8') as f:
                res = json.load(f)
                if isinstance(res, list):
                    return res
                return []
        except Exception:
            return []

    def save_history(self, history):
        if not os.path.exists(self.cache_dir):
            try:
                os.makedirs(self.cache_dir)
            except Exception:
                pass
        try:
            with open(self.history_file, 'w', encoding='utf-8') as f:
                json.dump(history, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def add_query(self, query):
        """
        添加一条搜索关键词。去重并置顶。
        """
        if not query:
            return
        query = query.strip()
        if not query:
            return
        history = self.load_history()
        # 去重
        if query in history:
            try:
                history.remove(query)
            except Exception:
                pass
        # 置顶
        history.insert(0, query)
        history = history[:self.max_items]
        self.save_history(history)

    def clear_history(self):
        """清除所有搜索记录"""
        self.save_history([])


# ------------------------------------------------------------------------------
# 1.7 推送历史记录管理器 (PushHistoryManager)
# ------------------------------------------------------------------------------
class PushHistoryManager:
    """管理推送网盘链接的历史记录"""
    def __init__(self, cache_dir, max_items=50):
        self.cache_dir = cache_dir
        self.history_file = os.path.join(cache_dir, "push_history.json")
        self.max_items = max_items

    def load_history(self):
        if not os.path.exists(self.history_file):
            return []
        try:
            with open(self.history_file, 'r', encoding='utf-8') as f:
                res = json.load(f)
                if isinstance(res, list):
                    return res
                return []
        except Exception:
            return []

    def save_history(self, history):
        if not os.path.exists(self.cache_dir):
            try:
                os.makedirs(self.cache_dir)
            except Exception:
                pass
        try:
            with open(self.history_file, 'w', encoding='utf-8') as f:
                json.dump(history, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def add_record(self, record):
        """
        添加一条推送记录。如果已存在相同的 URL，则将其移动到最前。
        """
        if not record or not record.get("url"):
            return
        history = self.load_history()
        new_history = []
        url = record.get("url").strip()
        
        for item in history:
            if item.get("url", "").strip() != url:
                new_history.append(item)
                
        new_history.insert(0, record)
        new_history = new_history[:self.max_items]
        self.save_history(new_history)

    def clear_history(self):
        self.save_history([])


# ------------------------------------------------------------------------------
# 2. 筛选项常量定义
# ------------------------------------------------------------------------------
YEARS = ["全部"] + [str(y) for y in range(2026, 2006, -1)] + ["更早"]
YEAR_VALUES = [None] + [str(y) for y in range(2026, 2006, -1)] + ["earlier"]

AREAS = ["全部", "大陆", "香港", "台湾", "美国", "西班牙", "法国", "英国", "日本", "韩国", "泰国", "德国", "印度", "意大利", "加拿大", "其它"]
AREA_VALUES = [None if a == "全部" else a for a in AREAS]

GENRES = ["全部", "剧情", "喜剧", "惊悚", "动作", "犯罪", "爱情", "恐怖", "悬疑", "纪录", "冒险", "奇幻", "家庭", "科幻", "动画", "历史", "电视电影", "科幻玄幻", "音乐", "动作冒险", "战争", "西部", "真人秀", "战争政治", "古装"]
GENRE_VALUES = [None if g == "全部" else g for g in GENRES]


# cloudid 或 provider key → 网盘名称映射
CLOUD_PROVIDER_NAMES = {
    "1": "阿里云盘", "ALIYUN": "阿里云盘", "aliyun": "阿里云盘",
    "2": "夸克网盘", "QUARK": "夸克网盘", "quark": "夸克网盘",
    "3": "UC网盘", "UC": "UC网盘", "uc": "UC网盘",
    "4": "百度网盘", "BAIDU": "百度网盘", "baidu": "百度网盘",
    "5": "迅雷网盘", "XUNLEI": "迅雷网盘", "xunlei": "迅雷网盘",
    "6": "123云盘", "CLOUD123": "123云盘", "cloud123": "123云盘",
    "7": "115网盘", "CLOUD115": "115网盘", "cloud115": "115网盘",
    "8": "移动云盘", "CHINA_MOBILE": "移动云盘", "china_mobile": "移动云盘",
    "9": "天翼云盘", "CHINA_TELECOM": "天翼云盘", "china_telecom": "天翼云盘",
    "11": "光鸭云盘", "GUANGYA": "光鸭云盘", "guangya": "光鸭云盘",
}

# cloudid 或 provider key → Go daemon provider key
CLOUD_PROVIDER_KEYS = {
    "1": "ALIYUN", "ALIYUN": "ALIYUN", "aliyun": "ALIYUN",
    "2": "QUARK", "QUARK": "QUARK", "quark": "QUARK",
    "3": "UC", "UC": "UC", "uc": "UC",
    "4": "BAIDU", "BAIDU": "BAIDU", "baidu": "BAIDU",
    "5": "XUNLEI", "XUNLEI": "XUNLEI", "xunlei": "XUNLEI",
    "6": "CLOUD123", "CLOUD123": "CLOUD123", "cloud123": "CLOUD123",
    "7": "CLOUD115", "CLOUD115": "CLOUD115", "cloud115": "CLOUD115",
    "8": "CHINA_MOBILE", "CHINA_MOBILE": "CHINA_MOBILE", "china_mobile": "CHINA_MOBILE",
    "9": "CHINA_TELECOM", "CHINA_TELECOM": "CHINA_TELECOM", "china_telecom": "CHINA_TELECOM",
    "11": "GUANGYA", "GUANGYA": "GUANGYA", "guangya": "GUANGYA",
}

PAN_ALIAS_TO_ID = {
    "7": "7", "115": "7", "cloud115": "7", "CLOUD115": "7",
    "4": "4", "bd": "4", "baidu": "4", "BAIDU": "4",
    "2": "2", "qk": "2", "quark": "2", "QUARK": "2",
    "3": "3", "uc": "3", "UC": "3",
    "11": "11", "gy": "11", "guangya": "11", "GUANGYA": "11",
    "6": "6", "123": "6", "cloud123": "6", "CLOUD123": "6",
    "9": "9", "ty": "9", "china_telecom": "9", "CHINA_TELECOM": "9",
    "8": "8", "yd": "8", "china_mobile": "8", "CHINA_MOBILE": "8",
    "5": "5", "xl": "5", "xunlei": "5", "XUNLEI": "5",
    "1": "1", "ali": "1", "aliyun": "1", "ALIYUN": "1",
    "10": "10"
}

def sort_pan_keys(pan_keys, pan_order=None):
    """根据 pan_order 权重序列对 pan_keys 进行重新排序"""
    if not isinstance(pan_order, list):
        pan_order = ["7", "4", "2", "3", "11", "6", "9", "8", "5", "1", "10"]
    def get_order_idx(k):
        cloud_id = PAN_ALIAS_TO_ID.get(str(k), str(k))
        try:
            return pan_order.index(cloud_id)
        except (ValueError, AttributeError):
            return 999
    return sorted(list(pan_keys), key=get_order_idx)

CLOUD_SHARE_PREFIXES = {
    "1": "https://www.alipan.com/s/",
    "2": "https://pan.quark.cn/s/",
    "3": "https://drive.uc.cn/s/",
    "4": "https://pan.baidu.com/s/",
    "5": "https://pan.xunlei.com/s/",
    "6": "https://www.123pan.com/s/",
    "7": "https://115.com/s/",
    "8": "https://caiyun.139.com/m/i?",
    "9": "https://cloud.189.cn/t/",
    "11": "https://www.guangyapan.com/s/",
    
    "ALIYUN": "https://www.alipan.com/s/",
    "QUARK": "https://pan.quark.cn/s/",
    "UC": "https://drive.uc.cn/s/",
    "BAIDU": "https://pan.baidu.com/s/",
    "XUNLEI": "https://pan.xunlei.com/s/",
    "CLOUD123": "https://www.123pan.com/s/",
    "CLOUD115": "https://115.com/s/",
    "CHINA_MOBILE": "https://caiyun.139.com/m/i?",
    "CHINA_TELECOM": "https://cloud.189.cn/t/",
    "GUANGYA": "https://www.guangyapan.com/s/",
}

def get_pic_url(item):
    if not isinstance(item, dict):
        return ""
    pic = item.get("pic") or item.get("coverUrl") or item.get("sourceCoverUrl") or item.get("cover") or item.get("pic_url") or ""
    if not pic:
        return ""
    pic = pic.strip()
    if pic.startswith("http://") or pic.startswith("https://"):
        return pic
    if pic.startswith("/"):
        return f"https://image.tmdb.org/t/p/original{pic}"
    return pic

def get_backdrop_url(item):
    if not isinstance(item, dict):
        return ""
    bg = item.get("backdropPath") or item.get("backdrop_path") or item.get("fanart") or ""
    if not bg:
        return ""
    bg = bg.strip()
    if bg.startswith("http://") or bg.startswith("https://"):
        return bg
    if not bg.startswith("/"):
        bg = "/" + bg
    return f"https://image.tmdb.org/t/p/original{bg}"

def to_share_url(raw_url, pan_name):
    if not raw_url:
        return ""
    raw_url = raw_url.strip()
    if raw_url.startswith("http://") or raw_url.startswith("https://"):
        return raw_url
    prefix = CLOUD_SHARE_PREFIXES.get(str(pan_name))
    if prefix:
        if pan_name in ["8", "CHINA_MOBILE"] and not raw_url.startswith("cid="):
            return prefix + "cid=" + raw_url
        return prefix + raw_url
    return raw_url


# ------------------------------------------------------------------------------
# 3. 核心 VFS 路由分发器 (VfsRouter)
# ------------------------------------------------------------------------------
class VfsRouter:
    """
    核心 VFS 路由分发器，控制目录跳转逻辑与数据流渲染。
    支持分类即时筛选（Container.Update）、网盘多视频懒加载以及全自动外挂字幕匹配和扫码登录 Dialog 整合。
    """
    def __init__(self, adapter, api_client):
        self.adapter = adapter
        self.api_client = api_client

    def route(self, query_string):
        params = dict(urllib.parse.parse_qsl(query_string.lstrip('?')))
        action = params.get('action', 'root')

        if action == 'root':
            self.show_root_menu()
        elif action == 'home':
            self.show_home_menu()
        elif action == 'home_list':
            self.show_home_list(params.get('type'))
        elif action == 'recommend':
            self.show_recommend()
        elif action == 'playlists':
            self.show_playlists()
        elif action == 'playlist_category':
            self.show_playlist_category(params.get('slug'))
        elif action == 'playlist_detail':
            self.show_playlist_detail(params.get('slug'))
        elif action == 'search_menu':
            self.show_search_menu()
        elif action == 'new_search':
            self.start_new_search()
        elif action == 'clear_search':
            self.clear_search_history()
        elif action == 'search':
            self.show_search_results(params.get('q'), int(params.get('page', 1)))
        elif action == 'category':
            self.show_category(params)
        elif action == 'select_filter':
            self.handle_filter_selection(params)
        elif action == 'detail':
            self.show_video_detail(params.get('id'))
        elif action == 'hls_lines':
            self.show_hls_lines(params)
        elif action == 'line_episodes':
            self.show_line_episodes(params)
        elif action == 'play_hls':
            self.play_hls(params)
        elif action == 'play_all_lines':
            self.play_all_lines(params)
        elif action == 'pan_sources':
            self.show_pan_sources(params)
        elif action == 'list_files':
            self.show_pan_files(params)
        elif action == 'pan_play':
            self.play_pan_file(params)
        elif action == 'play_all_pan_files':
            self.play_all_pan_files(params)
        elif action == 'history':
            self.show_history()
        elif action == 'clear_history':
            self.clear_history()
        elif action == 'my_pans_menu':
            self.show_my_pans_menu()
        elif action == 'pan_detail_menu':
            self.show_pan_detail_menu(params.get('provider'))
        elif action == 'pan_logout':
            self.handle_pan_logout(params.get('provider'))
        elif action == 'pan_concurrency':
            self.handle_pan_concurrency(params.get('provider'))
        elif action == 'pan_chunk_size':
            self.handle_pan_chunk_size(params.get('provider'))
        elif action == 'list_user_pan_files':
            self.list_user_pan_files(params)
        elif action == 'play_user_pan':
            self.play_user_pan(params)
        elif action == 'unsupported_file':
            return
        elif action == 'ignore':
            return
        elif action == 'qr_login':
            self.start_qr_login(params.get('provider'))
        elif action == 'manual_login':
            self.start_manual_login(params.get('provider'))
        elif action == 'push_menu':
            self.show_push_menu()
        elif action == 'connect_phone':
            self.start_push_dialog()
        elif action == 'new_push':
            self.start_push_dialog()
        elif action == 'clear_push_history':
            self.clear_push_history()
        elif action == 'pansou_menu':
            self.show_pansou_menu()
        elif action == 'pansou_search':
            self.show_pansou_results(params.get('q'))
        elif action == 'pansou_settings':
            self.show_pansou_settings_page(params)
        elif action == 'pansou_settings_save':
            self.save_pansou_settings(params)
        elif action == 'share_square':
            self.show_share_square(params)
        elif action == 'share_favorites':
            self.show_share_favorites(params)
        elif action in ['share_detail', 'show_share_detail']:
            self.show_share_detail(params)
        elif action == 'share_toggle_favorite':
            self.handle_share_toggle_favorite(params)
        else:
            self.show_root_menu()

    def show_root_menu(self):
        """渲染顶级主菜单"""
        self.adapter.add_directory_item("连接手机", {"action": "connect_phone"}, is_folder=False)
        self.adapter.add_directory_item("最近热门", {"action": "recommend"}, is_folder=True)
        self.adapter.add_directory_item("电影", {"action": "category", "typeId": 1, "page": 1}, is_folder=True)
        self.adapter.add_directory_item("剧集", {"action": "category", "typeId": 2, "page": 1}, is_folder=True)
        self.adapter.add_directory_item("动漫", {"action": "category", "typeId": 3, "page": 1}, is_folder=True)
        self.adapter.add_directory_item("综艺", {"action": "category", "typeId": 4, "page": 1}, is_folder=True)
        self.adapter.add_directory_item("纪录", {"action": "category", "typeId": 5, "page": 1}, is_folder=True)
        self.adapter.add_directory_item("短剧", {"action": "category", "typeId": 7, "page": 1}, is_folder=True)
        self.adapter.add_directory_item("专题片单", {"action": "playlists"}, is_folder=True)
        self.adapter.add_directory_item("播放记录", {"action": "history"}, is_folder=True)
        self.adapter.add_directory_item("视频搜索", {"action": "search_menu"}, is_folder=True)
        self.adapter.add_directory_item("分享广场", {"action": "share_square"}, is_folder=True)
        self.adapter.add_directory_item("我的网盘", {"action": "my_pans_menu"}, is_folder=True)
        self.adapter.add_directory_item("推送记录", {"action": "push_menu"}, is_folder=True)
        self.adapter.add_directory_item("盘搜", {"action": "pansou_menu"}, is_folder=True)
        self.adapter.end_of_directory()

    def show_home_menu(self):
        """首页二级菜单版块分类"""
        self.adapter.add_directory_item("个性化推荐", {"action": "recommend"}, is_folder=True)
        self.adapter.add_directory_item("精选轮播", {"action": "home_list", "type": "banner"}, is_folder=True)
        self.adapter.add_directory_item("热播电影", {"action": "home_list", "type": "movie"}, is_folder=True)
        self.adapter.add_directory_item("热播剧集", {"action": "home_list", "type": "tv"}, is_folder=True)
        self.adapter.add_directory_item("热播动漫", {"action": "home_list", "type": "anime"}, is_folder=True)
        self.adapter.add_directory_item("热播综艺", {"action": "home_list", "type": "variety"}, is_folder=True)
        self.adapter.add_directory_item("热播纪录", {"action": "home_list", "type": "documentary"}, is_folder=True)
        self.adapter.add_directory_item("热播短剧", {"action": "home_list", "type": "short"}, is_folder=True)
        self.adapter.end_of_directory()

    def show_home_list(self, section_type):
        """渲染首页具体频道视频列表"""
        data = self.api_client.get_home_data()
        field_map = {
            "banner": "bannerVideos",
            "movie": "movieVideos",
            "tv": "tvVideos",
            "anime": "animeVideos",
            "variety": "varietyVideos",
            "documentary": "documentaryVideos",
            "short": "shortVideos"
        }
        field = field_map.get(section_type, "movieVideos")
        videos = data.get(field) or []
        for v in videos:
            self._render_video_item(v)
        self.adapter.end_of_directory()

    def show_recommend(self):
        """渲染个性化推荐"""
        videos = self.api_client.get_recommendations()
        for v in videos:
            self._render_video_item(v)
        self.adapter.end_of_directory()

    def show_playlists(self):
        """渲染片单分类"""
        categories = self.api_client.get_playlist_categories()
        for cat in categories:
            self.adapter.add_directory_item(
                cat.get("name"), 
                {"action": "playlist_category", "slug": cat.get("slug")}, 
                is_folder=True
            )
        self.adapter.end_of_directory()

    def show_playlist_category(self, slug):
        """渲染特定分类下的主题片单列表"""
        categories = self.api_client.get_playlist_categories()
        matched = next((cat for cat in categories if cat.get("slug") == slug), None)
        if matched:
            for pl in matched.get("playlists", []):
                label = f"{pl.get('title')} ({pl.get('itemCount')}部)"
                self.adapter.add_directory_item(
                    label, 
                    {"action": "playlist_detail", "slug": pl.get("slug")}, 
                    is_folder=True, 
                    icon=pl.get("coverUrl")
                )
        self.adapter.end_of_directory()

    def show_playlist_detail(self, slug):
        """渲染具体片单内的视频项"""
        detail = self.api_client.get_playlist_detail(slug)
        items = detail.get("items", [])
        for item in items:
            vod_info_id = item.get("vodInfoId")
            if vod_info_id:
                info = {
                    "plot": f"导演: {item.get('director') or '未知'}\n主演: {item.get('starring') or '未知'}"
                }
                self.adapter.add_directory_item(
                    item.get("title"),
                    {"action": "detail", "id": vod_info_id},
                    is_folder=True,
                    icon=item.get("coverUrl") or item.get("sourceCoverUrl"),
                    info=info
                )
        self.adapter.end_of_directory()

    def show_search_menu(self):
        """渲染搜索主菜单"""
        self.adapter.add_directory_item("新建搜索...", {"action": "new_search"}, is_folder=False)
        
        # 加载并渲染历史搜索记录
        profile_dir = self.adapter.get_profile_path()
        history_mgr = SearchHistoryManager(profile_dir)
        history = history_mgr.load_history()
        
        if history:
            self.adapter.add_directory_item("清除搜索历史记录", {"action": "clear_search"}, is_folder=False)
            for q in history:
                self.adapter.add_directory_item(
                    f"历史: {q}", 
                    {"action": "search", "q": q, "page": 1}, 
                    is_folder=True
                )
                
        self.adapter.end_of_directory()

    def clear_search_history(self):
        """清除搜索记录"""
        profile_dir = self.adapter.get_profile_path()
        history_mgr = SearchHistoryManager(profile_dir)
        history_mgr.clear_history()
        self.adapter.show_notification("搜索历史", "已清除搜索记录")
        self.adapter.refresh_container()

    def start_new_search(self):
        """新建搜索交互输入"""
        if self.adapter.xbmc:
            keyboard = self.adapter.xbmc.Keyboard("", "请输入搜索关键字")
            keyboard.doModal()
            if keyboard.isConfirmed():
                query = keyboard.getText()
                if query:
                    search_params = {"action": "search", "q": query, "page": 1}
                    self.adapter.update_container(search_params, replace=False)

    def show_search_results(self, query, page=1):
        """请求并渲染搜索结果及分页"""
        if page == 1 and query:
            profile_dir = self.adapter.get_profile_path()
            history_mgr = SearchHistoryManager(profile_dir)
            history_mgr.add_query(query)

        data = self.api_client.search_videos(query, page=page)
        videos = data.get("items") or []
        pagination = data.get("pagination") or {}

        for v in videos:
            self._render_video_item(v)

        current_page = pagination.get("page", 1)
        total_pages = pagination.get("totalPages", 1)
        if current_page < total_pages:
            self.adapter.add_directory_item(
                "下一页 >>", 
                {"action": "search", "q": query, "page": current_page + 1}, 
                is_folder=True
            )
        self.adapter.end_of_directory()

    # --------------------------------------------------------------------------
    # 4. 分类即时筛选功能整合
    # --------------------------------------------------------------------------
    def fetch_tags(self):
        """获取视频标签，优先使用 TTL 缓存"""
        profile_dir = self.adapter.get_profile_path()
        cache_manager = TagsCacheManager(profile_dir)
        cached = cache_manager.get_cached_tags()
        if cached is not None:
            return cached

        tags = []
        try:
            tags = self.api_client.get_tags()
        except Exception as e:
            self.adapter.log(f"获取标签列表失败: {str(e)}", "warning")

        if tags:
            cache_manager.save_tags(tags)
        return tags

    def show_category(self, params):
        """渲染分类视频目录并固定在第一页头部展示控制筛选项与筛选结果数量"""
        type_id = int(params.get('typeId', 1))
        year = params.get('year') or None
        area = params.get('area') or None
        tag_id = int(params.get('tagId', 0))
        genre = params.get('genre') or None
        page = int(params.get('page', 1))

        # 1. 优先获取数据，以取得 pagination.total 数量
        data = self.api_client.get_category_videos(type_id, year, area, tag_id, page, page_size=22, genre=genre)
        videos = data.get("items") or []
        pagination = data.get("pagination") or {}
        total = pagination.get("total", 0)

        # 2. 如果是第一页，在最前面固定展示筛选条件
        if page == 1:
            # 2.1 标签匹配处理，计算当前选中 tag_label
            tags = self.fetch_tags()
            tag_label = "全部"
            if tag_id > 0:
                for t in tags:
                     tid = t.get("tagId") or t.get("id")
                     if tid is not None and safe_int(tid) == tag_id:
                         tag_label = t.get("tagName") or t.get("name")
                         break

            year_label = year if year else "全部"
            area_label = area if area else "全部"
            genre_label = genre if genre else "全部"

            # 2.3 年份、地区、类型、标签各自的筛选控制按钮 (年份->地区->类型->标签)
            self.adapter.add_directory_item(
                f"年份: {year_label} (点击修改)",
                {"action": "select_filter", "typeId": type_id, "filter_type": "year", "year": year or "", "area": area or "", "tagId": tag_id, "genre": genre or ""},
                is_folder=False
            )
            self.adapter.add_directory_item(
                f"地区: {area_label} (点击修改)",
                {"action": "select_filter", "typeId": type_id, "filter_type": "area", "year": year or "", "area": area or "", "tagId": tag_id, "genre": genre or ""},
                is_folder=False
            )
            self.adapter.add_directory_item(
                f"类型: {genre_label} (点击修改)",
                {"action": "select_filter", "typeId": type_id, "filter_type": "genre", "year": year or "", "area": area or "", "tagId": tag_id, "genre": genre or ""},
                is_folder=False
            )
            self.adapter.add_directory_item(
                f"标签: {tag_label} (点击修改)",
                {"action": "select_filter", "typeId": type_id, "filter_type": "tag", "year": year or "", "area": area or "", "tagId": tag_id, "genre": genre or ""},
                is_folder=False
            )

            # 2.4 重置筛选按钮
            if year or area or tag_id > 0 or genre:
                self.adapter.add_directory_item(
                    "重置当前所有筛选条件",
                    {"action": "category", "typeId": type_id, "page": 1},
                    is_folder=True
                )

            self.adapter.add_directory_item(
                "返回主页",
                {"action": "root"},
                is_folder=True
            )

        # 3. 追加常规视频列表
        for v in videos:
            self._render_video_item(v)

        # 4. 分页处理
        current_page = pagination.get("page", 1)
        total_pages = pagination.get("totalPages", 1)
        if current_page < total_pages:
            self.adapter.add_directory_item(
                "下一页 >>", 
                {"action": "category", "typeId": type_id, "year": year or "", "area": area or "", "tagId": tag_id, "genre": genre or "", "page": current_page + 1}, 
                is_folder=True
            )
        self.adapter.end_of_directory()

    def handle_filter_selection(self, params):
        """处理点击控制筛选项时的对话框选择和 Container 重定向"""
        type_id = int(params.get('typeId', 1))
        filter_type = params.get('filter_type')
        current_year = params.get('year') or None
        current_area = params.get('area') or None
        current_tag_id = int(params.get('tagId', 0))
        current_genre = params.get('genre') or None

        new_year = current_year
        new_area = current_area
        new_tag_id = current_tag_id
        new_genre = current_genre

        if filter_type == 'genre':
            default_idx = GENRES.index(current_genre) if current_genre in GENRES else 0
            idx = self.adapter.show_select_dialog("选择类型", GENRES, default_idx)
            if idx >= 0:
                new_genre = GENRE_VALUES[idx]

        elif filter_type == 'year':
            default_idx = YEARS.index(current_year) if current_year in YEARS else 0
            idx = self.adapter.show_select_dialog("选择年份", YEARS, default_idx)
            if idx >= 0:
                new_year = YEAR_VALUES[idx]

        elif filter_type == 'area':
            default_idx = AREAS.index(current_area) if current_area in AREAS else 0
            idx = self.adapter.show_select_dialog("选择地区", AREAS, default_idx)
            if idx >= 0:
                new_area = AREA_VALUES[idx]

        elif filter_type == 'tag':
            tags = self.fetch_tags()
            tags_display = ["全部"] + [(t.get("tagName") or t.get("name") or "") for t in tags]
            tags_values = [0] + [safe_int(t.get("tagId") or t.get("id")) for t in tags]

            default_idx = 0
            if current_tag_id > 0 and current_tag_id in tags_values:
                default_idx = tags_values.index(current_tag_id)

            idx = self.adapter.show_select_dialog("选择标签", tags_display, default_idx)
            if idx >= 0:
                new_tag_id = tags_values[idx]

        new_params = {
            "action": "category",
            "typeId": type_id,
            "year": new_year or "",
            "area": new_area or "",
            "tagId": new_tag_id,
            "genre": new_genre or "",
            "page": 1
        }
        self.adapter.update_container(new_params, replace=True)

    # --------------------------------------------------------------------------
    # 5. 网盘剧集多文件懒加载与播放整合
    # --------------------------------------------------------------------------
    def show_video_detail(self, video_id):
        """渲染播放源类型分组菜单（类似 Android 端的 HLS/网盘分组）"""
        detail = self.api_client.get_video_detail(video_id)
        if not detail:
            self.adapter.end_of_directory()
            return

        play_list_by_line = detail.get("playListByLine") or {}
        pan_list = detail.get("panList") or {}
        vod_title = detail.get("title") or ""
        pic = get_pic_url(detail)
        fanart = get_backdrop_url(detail)
        type_id = detail.get("typeId") or detail.get("type_id")

        # HLS 源分组
        if play_list_by_line:
            total_lines = len(play_list_by_line)
            self.adapter.add_directory_item(
                f"HLS ({total_lines})",
                {"action": "hls_lines", "id": video_id, "vod_title": vod_title, "type_id": type_id, "pic": pic, "fanart": fanart},
                is_folder=True,
                pic=pic,
                fanart=fanart
            )

        status_data = self.api_client.get_pan_status() or {}
        pan_order = self.api_client.get_pan_order()
        if not isinstance(pan_order, list):
            pan_order = ["7", "4", "2", "3", "11", "6", "9", "8", "5", "1", "10"]

        def get_sort_key(item):
            pan_key, _ = item
            cloud_id = PAN_ALIAS_TO_ID.get(str(pan_key), str(pan_key))
            try:
                order_idx = pan_order.index(cloud_id)
            except Exception:
                order_idx = 999
            return order_idx

        sorted_pan_list = sorted(pan_list.items(), key=get_sort_key)

        # 网盘源按 provider 分组
        for pan_key, items in sorted_pan_list:
            count = len(items)
            display_name = CLOUD_PROVIDER_NAMES.get(str(pan_key), f"网盘{pan_key}")
            self.adapter.add_directory_item(
                f"{display_name} ({count})",
                {"action": "pan_sources", "id": video_id, "pan_name": pan_key, "vod_title": vod_title, "type_id": type_id, "pic": pic, "fanart": fanart},
                is_folder=True,
                pic=pic,
                fanart=fanart
            )

        if not play_list_by_line and not pan_list:
            self.adapter.add_directory_item("暂无播放源", {"action": "root"}, is_folder=False)

        self.adapter.end_of_directory()

    def show_hls_lines(self, params):
        """渲染 HLS 线路列表"""
        video_id = params.get("id")
        vod_title = params.get("vod_title") or ""
        type_id = params.get("type_id")
        detail = self.api_client.get_video_detail(video_id)
        if not detail:
            self.adapter.end_of_directory()
            return

        pic = params.get("pic") or get_pic_url(detail)
        fanart = params.get("fanart") or get_backdrop_url(detail)

        if not type_id and detail:
            type_id = detail.get("typeId") or detail.get("type_id")

        play_list_by_line = detail.get("playListByLine") or {}
        for line_name, items in play_list_by_line.items():
            display_line_name = line_name
            if not display_line_name.startswith("线路"):
                display_line_name = f"线路{display_line_name}"
            label = f"{display_line_name} (共 {len(items)} 集)"
            self.adapter.add_directory_item(
                label,
                {"action": "line_episodes", "id": video_id, "line": line_name, "vod_title": vod_title or detail.get("title") or "", "type_id": type_id, "pic": pic, "fanart": fanart},
                is_folder=True,
                pic=pic,
                fanart=fanart
            )
        self.adapter.end_of_directory()

    def show_pan_sources(self, params):
        """渲染某个网盘 provider 下的分享链接列表"""
        video_id = params.get("id")
        pan_name = params.get("pan_name")
        vod_title = params.get("vod_title") or ""
        type_id = params.get("type_id")
        detail = self.api_client.get_video_detail(video_id)
        if not detail:
            self.adapter.end_of_directory()
            return

        pic = params.get("pic") or get_pic_url(detail)
        fanart = params.get("fanart") or get_backdrop_url(detail)

        if not type_id and detail:
            type_id = detail.get("typeId") or detail.get("type_id")

        pan_list = detail.get("panList") or {}
        items = pan_list.get(pan_name) or []

        for idx, item in enumerate(items):
            share_url = to_share_url(item.get("url"), pan_name)
            code = item.get("code") or ""
            remark = item.get("remark") or ""
            label = f"链接{idx+1}{remark}"
            self.adapter.add_directory_item(
                label,
                {
                    "action": "list_files", 
                    "url": share_url, 
                    "code": code, 
                    "pan_name": pan_name,
                    "vod_title": vod_title or detail.get("title") or "",
                    "vod_id": video_id,
                    "link_id": item.get("id"),
                    "link_idx": idx + 1,
                    "type_id": type_id,
                    "pic": pic,
                    "fanart": fanart
                },
                is_folder=True,
                pic=pic,
                fanart=fanart
            )
        self.adapter.end_of_directory()

    def show_line_episodes(self, params):
        """渲染某条线路下的剧集列表"""
        video_id = params.get("id")
        line_name = params.get("line")
        vod_title = params.get("vod_title") or ""
        type_id = params.get("type_id")
        detail = self.api_client.get_video_detail(video_id)
        if not detail:
            self.adapter.end_of_directory()
            return

        pic = params.get("pic") or get_pic_url(detail)
        fanart = params.get("fanart") or get_backdrop_url(detail)

        if not vod_title and detail:
            vod_title = detail.get("title") or ""

        if not type_id and detail:
            type_id = detail.get("typeId") or detail.get("type_id")

        self.adapter.set_content("episodes")

        play_list_by_line = detail.get("playListByLine") or {}
        episodes = play_list_by_line.get(line_name) or []

        for ep in episodes:
            raw_ep_name = ep.get("name") or f"第 {ep.get('episode', '?')} 集"
            ep_url = ep.get("url") or ""
            ep_num = parse_episode_number(raw_ep_name)
            
            if vod_title:
                if ep_num > 0:
                    full_title = f"{vod_title} 第{ep_num:02d}集"
                elif vod_title not in raw_ep_name:
                    full_title = f"{vod_title} {raw_ep_name}"
                else:
                    full_title = raw_ep_name
            else:
                full_title = raw_ep_name

            info = {
                "tvshowtitle": vod_title or full_title,
                "title": full_title,
                "mediatype": "episode" if ep_num > 0 else "video"
            }
            if ep_num > 0:
                info["episode"] = ep_num

            if ep_url:
                self.adapter.add_directory_item(
                    full_title,
                    {
                        "action": "play_hls", 
                        "url": ep_url, 
                        "title": full_title,
                        "vod_title": vod_title,
                        "vod_id": video_id,
                        "line": line_name,
                        "type_id": type_id,
                        "pic": pic
                    },
                    is_folder=False,
                    is_playable=True,
                    pic=pic,
                    info=info
                )

        if not episodes:
            self.adapter.add_directory_item("暂无可播放剧集", {"action": "root"}, is_folder=False)

        self.adapter.end_of_directory()

    def play_hls(self, params):
        """播放 HLS 线路的视频"""
        play_url = params.get("url") or ""
        title = params.get("title") or ""
        vod_title = params.get("vod_title") or ""
        vod_id = params.get("vod_id") or ""
        pic = params.get("pic") or ""
        fanart = params.get("fanart") or ""
        line_name = params.get("line")
        type_id = params.get("type_id")

        if not play_url:
            self.adapter.log("HLS 播放地址为空", "error")
            return

        # 尝试为多集剧集自动填充完整播放列表
        if vod_id and line_name and self.adapter.xbmc and self.adapter.xbmcgui:
            try:
                playlist = self.adapter.xbmc.PlayList(self.adapter.xbmc.PLAYLIST_VIDEO)
                need_rebuild = True
                if playlist.size() > 1:
                    try:
                        first_path = playlist[0].getPath() if hasattr(playlist[0], 'getPath') else ""
                        if vod_id and f"vod_id={vod_id}" in first_path:
                            need_rebuild = False
                    except Exception:
                        need_rebuild = True

                if need_rebuild:
                    detail = self.api_client.get_video_detail(vod_id)
                    if detail:
                        play_list_by_line = detail.get("playListByLine") or {}
                        episodes = play_list_by_line.get(line_name) or []
                        if len(episodes) > 1:
                            playlist.clear()
                            for idx, ep in enumerate(episodes):
                                raw_ep_name = ep.get("name") or f"第 {ep.get('episode', '?')} 集"
                                ep_url = ep.get("url") or ""
                                if not ep_url:
                                    continue

                                ep_num = parse_episode_number(raw_ep_name)
                                if vod_title:
                                    if ep_num > 0:
                                        full_title = f"{vod_title} 第{ep_num:02d}集"
                                    elif vod_title not in raw_ep_name:
                                        full_title = f"{vod_title} {raw_ep_name}"
                                    else:
                                        full_title = raw_ep_name
                                else:
                                    full_title = raw_ep_name

                                item_url = f"{self.adapter.base_url}?{urllib.parse.urlencode({'action': 'play_hls', 'url': ep_url, 'title': full_title, 'vod_title': vod_title, 'vod_id': vod_id, 'line': line_name, 'type_id': type_id, 'pic': pic, 'fanart': fanart})}"
                                target_path = play_url if ep_url == play_url else item_url

                                list_item = self.adapter.xbmcgui.ListItem(label=full_title, path=target_path)
                                list_item.setProperty('IsPlayable', 'true')
                                art = {}
                                if pic:
                                    art.update({'icon': pic, 'thumb': pic, 'poster': pic, 'landscape': pic, 'banner': pic})
                                if fanart:
                                    art['fanart'] = fanart
                                if art:
                                    list_item.setArt(art)

                                info = {
                                    "tvshowtitle": vod_title or full_title,
                                    "title": full_title,
                                    "mediatype": "episode" if ep_num > 0 else "video"
                                }
                                if ep_num > 0:
                                    info["episode"] = ep_num
                                list_item.setInfo('video', info)

                                playlist.add(target_path, list_item)
            except Exception as ex:
                self.adapter.log(f"自动构建 HLS 播放列表失败: {str(ex)}", "warning")

        if self.adapter.xbmcgui and self.adapter.xbmcplugin:
            ep_num = parse_episode_number(title)
            if vod_title:
                if ep_num > 0:
                    display_title = f"{vod_title} 第{ep_num:02d}集"
                elif vod_title not in title:
                    display_title = f"{vod_title} {title}"
                else:
                    display_title = title
            else:
                display_title = title

            list_item = self.adapter.xbmcgui.ListItem(label=display_title, path=play_url)
            list_item.setProperty('IsPlayable', 'true')

            art = {}
            if pic:
                art.update({'icon': pic, 'thumb': pic, 'poster': pic, 'landscape': pic, 'banner': pic})
            if fanart:
                art['fanart'] = fanart
            if art:
                list_item.setArt(art)

            info = {
                "tvshowtitle": vod_title or display_title,
                "title": display_title,
                "mediatype": "episode" if ep_num > 0 else "video"
            }
            if ep_num > 0:
                info["episode"] = ep_num
            list_item.setInfo('video', info)

            self.adapter.set_resolved_url(True, list_item)

            # 写入播放历史
            if vod_title:
                try:
                    profile_dir = self.adapter.get_profile_path()
                    history_mgr = HistoryManager(profile_dir)
                    type_id = params.get("type_id")
                    line_name = params.get("line")
                    is_folder = False
                    play_params = {
                        "action": "play_hls",
                        "url": play_url,
                        "title": title,
                        "vod_title": vod_title,
                        "vod_id": vod_id
                    }
                    
                    if type_id is not None and str(type_id) != "1" and line_name:
                        is_folder = True
                        play_params = {
                            "action": "line_episodes",
                            "id": vod_id,
                            "line": line_name,
                            "vod_title": vod_title,
                            "type_id": type_id
                        }

                    record = {
                        "vod_title": vod_title,
                        "vod_id": vod_id,
                        "item_name": title,
                        "timestamp": time.time(),
                        "is_folder": is_folder,
                        "play_params": play_params
                    }
                    history_mgr.add_record(record)
                except Exception as e:
                    self.adapter.log(f"保存HLS播放历史失败: {str(e)}", "warning")

            self.adapter.xbmcplugin.setResolvedUrl(self.adapter.handle, True, list_item)
        else:
            self.adapter.log(f"HLS Play URL: {play_url}")

    def play_all_lines(self, params):
        """将 HLS 线路全集写入 Kodi 播放列表并顺序播放"""
        video_id = params.get("id")
        line_name = params.get("line")
        vod_title = params.get("vod_title") or ""
        pic = params.get("pic") or ""
        fanart = params.get("fanart") or ""
        type_id = params.get("type_id")
        detail = self.api_client.get_video_detail(video_id)
        if not detail:
            return

        if not pic:
            pic = get_pic_url(detail)

        if not fanart:
            fanart = get_backdrop_url(detail)

        if not vod_title:
            vod_title = detail.get("title") or ""

        play_list_by_line = detail.get("playListByLine") or {}
        episodes = play_list_by_line.get(line_name) or []

        if self.adapter.xbmcgui and self.adapter.xbmc:
            playlist = self.adapter.xbmc.PlayList(self.adapter.xbmc.PLAYLIST_VIDEO)
            playlist.clear()

            for ep in episodes:
                raw_ep_name = ep.get("name") or f"第 {ep.get('episode', '?')} 集"
                ep_url = ep.get("url") or ""
                if not ep_url:
                    continue

                ep_num = parse_episode_number(raw_ep_name)
                if vod_title:
                    if ep_num > 0:
                        full_title = f"{vod_title} 第{ep_num:02d}集"
                    elif vod_title not in raw_ep_name:
                        full_title = f"{vod_title} {raw_ep_name}"
                    else:
                        full_title = raw_ep_name
                else:
                    full_title = raw_ep_name

                item_url = f"{self.adapter.base_url}?{urllib.parse.urlencode({'action': 'play_hls', 'url': ep_url, 'title': full_title, 'vod_title': vod_title, 'vod_id': video_id, 'line': line_name, 'type_id': type_id, 'pic': pic, 'fanart': fanart})}"

                list_item = self.adapter.xbmcgui.ListItem(label=full_title, path=item_url)
                list_item.setProperty('IsPlayable', 'true')
                art = {}
                if pic:
                    art.update({'icon': pic, 'thumb': pic, 'poster': pic, 'landscape': pic, 'banner': pic})
                if fanart:
                    art['fanart'] = fanart
                if art:
                    list_item.setArt(art)

                info = {
                    "tvshowtitle": vod_title or full_title,
                    "title": full_title,
                    "mediatype": "episode" if ep_num > 0 else "video"
                }
                if ep_num > 0:
                    info["episode"] = ep_num
                list_item.setInfo('video', info)

                playlist.add(item_url, list_item)

            self.adapter.xbmc.Player().play(playlist)

    def show_pan_files(self, params, progress=None):
        """渲染电视剧网盘文件夹下的集数子目录列表，未登录时提示扫码登录"""
        pan_name = params.get("pan_name") or ""
        share_url = to_share_url(params.get("url"), pan_name)
        params["url"] = share_url
        code = params.get("code") or ""
        vod_title = params.get("vod_title") or ""
        vod_id = params.get("vod_id") or ""
        link_id = params.get("link_id")
        type_id = params.get("type_id")
        pic = params.get("pic") or ""
        fanart = params.get("fanart") or ""
        link_idx = safe_int(params.get("link_idx"), 1)
        page = safe_int(params.get("page"), 1)
        page_size = safe_int(params.get("page_size"), 50)

        def get_next_link():
            safe_vod_id = str(safe_int(vod_id)) if vod_id else ""
            self.adapter.log(f"[get_next_link] 开始匹配. vod_id_raw={vod_id}, safe_vod_id={safe_vod_id}, pan_name={pan_name}, link_id={link_id}", "info")
            if safe_vod_id and pan_name and link_id:
                try:
                    detail = self.api_client.get_video_detail(safe_vod_id)
                    self.adapter.log(f"[get_next_link] API详情请求完毕. 结果存在={bool(detail)}", "info")
                    if detail:
                        pan_list = detail.get("panList") or {}
                        items = []
                        for k, v in pan_list.items():
                            if str(k) == str(pan_name):
                                items = v
                                break
                        self.adapter.log(f"[get_next_link] 匹配到网盘 items 长度={len(items)}", "info")
                        current_idx = -1
                        for idx, item in enumerate(items):
                            if str(item.get("id")) == str(link_id):
                                current_idx = idx
                                break
                        if current_idx != -1:
                            if current_idx < len(items) - 1:
                                next_item = items[current_idx + 1]
                                self.adapter.log(f"[get_next_link] 成功找到下一个链接 (基于当前索引+1): id={next_item.get('id')}", "info")
                                return next_item
                            else:
                                self.adapter.log(f"[get_next_link] 当前已经是列表最后一项", "info")
                        else:
                            # 没找到，说明当前链接可能已被后端过滤，直接尝试当前剩余列表的第一项
                            if items:
                                next_item = items[0]
                                self.adapter.log(f"[get_next_link] 链接已失效被过滤，自动尝试当前剩余的第一个链接: id={next_item.get('id')}", "info")
                                return next_item
                            else:
                                self.adapter.log(f"[get_next_link] 列表已空", "info")
                except Exception as ex:
                    self.adapter.log(f"查找下一个网盘链接失败: {str(ex)}", "warning")
            else:
                self.adapter.log(f"[get_next_link] 参数缺失，跳过匹配", "warning")
            return None

        try:
            files_data = self.api_client.list_pan_files(share_url, code, page=page, page_size=page_size)
            files = files_data.get("files") or []
            total = safe_int(files_data.get("total"), 0)
            total_page = safe_int(files_data.get("total_page"), 1)
            has_more = bool(files_data.get("has_more", False))

            if not files:
                if link_id:
                    self.api_client.mark_pan_link_invalid(link_id, "无可播放文件")
                
                next_link = get_next_link()
                if next_link:
                    next_link_idx = link_idx + 1
                    msg = f"当前链接已失效，自动尝试备用链接: 链接{next_link_idx}"
                    if not progress and self.adapter.xbmcgui:
                        try:
                            progress = self.adapter.xbmcgui.DialogProgress()
                            progress.create("失效链接自愈", msg)
                        except Exception:
                            progress = None
                    elif progress:
                        try:
                            progress.update(50, msg)
                        except Exception:
                            pass
                    else:
                        self.adapter.show_notification("链接失效", msg)

                    new_params = params.copy()
                    new_params["url"] = next_link.get("url") or ""
                    new_params["code"] = next_link.get("code") or ""
                    new_params["link_id"] = next_link.get("id")
                    new_params["link_idx"] = next_link_idx
                    return self.show_pan_files(new_params, progress=progress)

                if progress:
                    try:
                        progress.close()
                    except Exception:
                        pass
                self.adapter.add_directory_item(
                    "无可播放文件 (已自动上报失效)",
                    {"action": "root"},
                    is_folder=False
                )
                self.adapter.end_of_directory()
                return
        except Exception as e:
            err_msg = str(e)
            is_login_needed = "请先登录" in err_msg or "未登录" in err_msg
            
            # 如果是链接失效，直接提示“该链接已失效（已自动上报失效）”并返回
            if not is_login_needed and should_mark_pan_link_invalid(err_msg):
                if link_id:
                    self.api_client.mark_pan_link_invalid(link_id, err_msg)
                
                next_link = get_next_link()
                if next_link:
                    next_link_idx = link_idx + 1
                    msg = f"当前网盘链接已失效，自动尝试备用链接: 链接{next_link_idx}"
                    if not progress and self.adapter.xbmcgui:
                        try:
                            progress = self.adapter.xbmcgui.DialogProgress()
                            progress.create("失效链接自愈", msg)
                        except Exception:
                            progress = None
                    elif progress:
                        try:
                            progress.update(50, msg)
                        except Exception:
                            pass
                    else:
                        self.adapter.show_notification("链接失效", msg)

                    new_params = params.copy()
                    new_params["url"] = next_link.get("url") or ""
                    new_params["code"] = next_link.get("code") or ""
                    new_params["link_id"] = next_link.get("id")
                    new_params["link_idx"] = next_link_idx
                    return self.show_pan_files(new_params, progress=progress)

                if progress:
                    try:
                        progress.close()
                    except Exception:
                        pass
                self.adapter.add_directory_item(
                    "该链接已失效（已自动上报失效）",
                    {"action": "root"},
                    is_folder=False
                )
                self.adapter.end_of_directory()
                return

            # 请求失败（未登录 / token 过期），提示用户登录
            provider_display = CLOUD_PROVIDER_NAMES.get(str(pan_name), f"网盘{pan_name}")
            provider_key = CLOUD_PROVIDER_KEYS.get(str(pan_name))

            if progress:
                try:
                    progress.close()
                except Exception:
                    pass

            self.adapter.add_directory_item(
                f"请先登录{provider_display}",
                {"action": "root"},
                is_folder=False
            )
            self.adapter.add_directory_item(
                f"错误原因: {err_msg} (URL: {share_url})",
                {"action": "root"},
                is_folder=False
            )
            if provider_key:
                self.adapter.add_directory_item(
                    f"点击扫码登录{provider_display}",
                    {"action": "qr_login", "provider": provider_key},
                    is_folder=False
                )
            self.adapter.end_of_directory()
            return

        self.adapter.set_content("episodes")

        if page > 1:
            prev_params = params.copy()
            prev_params["page"] = page - 1
            prev_title = f"上一页 (第 {page - 1}/{total_page} 页)" if total_page > 1 else f"上一页 (第 {page - 1} 页)"
            self.adapter.add_directory_item(
                prev_title,
                prev_params,
                is_folder=True
            )

        for f in files:
            file_name = f.get("file_name")
            size_label = f.get("size_label")
            ep_num = parse_episode_number(file_name)
            
            if vod_title:
                formatted_name = f"{vod_title}：（{file_name}）"
            else:
                formatted_name = file_name

            display_name = f"[{size_label}]{formatted_name}" if size_label else formatted_name

            info = {
                "tvshowtitle": vod_title or file_name,
                "title": formatted_name,
                "mediatype": "episode" if ep_num > 0 else "video"
            }
            if ep_num > 0:
                info["episode"] = ep_num

            self.adapter.add_directory_item(
                display_name,
                {
                    "action": "pan_play", 
                    "url": share_url, 
                    "code": code, 
                    "file_id": f.get("file_id"), 
                    "pan_name": pan_name,
                    "file_name": file_name,
                    "title": formatted_name,
                    "vod_title": vod_title,
                    "vod_id": vod_id,
                    "link_id": link_id,
                    "link_idx": link_idx,
                    "type_id": type_id,
                    "pic": pic,
                    "fanart": fanart
                },
                is_folder=False,
                is_playable=True,
                pic=pic,
                fanart=fanart,
                info=info
            )

        if has_more or (total_page > 1 and page < total_page):
            next_params = params.copy()
            next_params["page"] = page + 1
            next_title = f"下一页 (第 {page + 1}/{total_page} 页)" if total_page > 1 else f"下一页 (第 {page + 1} 页)"
            self.adapter.add_directory_item(
                next_title,
                next_params,
                is_folder=True
            )

        if progress:
            try:
                progress.close()
            except Exception:
                pass
        self.adapter.end_of_directory()

    def play_all_pan_files(self, params):
        """将网盘全集文件写入 Kodi 播放列表并自动顺序播放"""
        pan_name = params.get("pan_name") or ""
        share_url = to_share_url(params.get("url"), pan_name)
        code = params.get("code") or ""
        vod_title = params.get("vod_title") or ""
        vod_id = params.get("vod_id") or ""
        link_id = params.get("link_id")
        type_id = params.get("type_id")
        pic = params.get("pic") or ""
        fanart = params.get("fanart") or ""
        link_idx = safe_int(params.get("link_idx"), 1)

        try:
            files_data = self.api_client.list_pan_files(share_url, code)
            files = files_data.get("files") or []
        except Exception:
            files = []

        if not files:
            self.adapter.show_notification("网盘播放", "无可播放文件", is_error=True)
            return

        if self.adapter.xbmcgui and self.adapter.xbmc:
            playlist = self.adapter.xbmc.PlayList(self.adapter.xbmc.PLAYLIST_VIDEO)
            playlist.clear()

            for f in files:
                file_name = f.get("file_name")
                ep_num = parse_episode_number(file_name)

                if vod_title:
                    formatted_name = f"{vod_title}：（{file_name}）"
                else:
                    formatted_name = file_name

                item_url = f"{self.adapter.base_url}?{urllib.parse.urlencode({'action': 'pan_play', 'url': share_url, 'code': code, 'file_id': f.get('file_id'), 'pan_name': pan_name, 'file_name': file_name, 'title': formatted_name, 'vod_title': vod_title, 'vod_id': vod_id, 'link_id': link_id, 'link_idx': link_idx, 'type_id': type_id, 'pic': pic, 'fanart': fanart})}"

                list_item = self.adapter.xbmcgui.ListItem(label=formatted_name, path=item_url)
                list_item.setProperty('IsPlayable', 'true')
                art = {}
                if pic:
                    art.update({'icon': pic, 'thumb': pic, 'poster': pic, 'landscape': pic, 'banner': pic})
                if fanart:
                    art['fanart'] = fanart
                if art:
                    list_item.setArt(art)

                info = {
                    "tvshowtitle": vod_title or formatted_name,
                    "title": formatted_name,
                    "mediatype": "episode" if ep_num > 0 else "video"
                }
                if ep_num > 0:
                    info["episode"] = ep_num
                list_item.setInfo('video', info)

                playlist.add(item_url, list_item)

            self.adapter.xbmc.Player().play(playlist)

    def play_pan_file(self, params):
        """点击集数进行播放直链和外挂字幕解析渲染"""
        pan_name = params.get("pan_name") or ""
        provider_key = CLOUD_PROVIDER_KEYS.get(str(pan_name), pan_name)
        share_url = to_share_url(params.get("url"), pan_name)
        code = params.get("code") or ""
        file_id = params.get("file_id") or ""
        vod_title = params.get("vod_title") or ""
        vod_id = params.get("vod_id") or ""
        file_name = params.get("file_name") or ""
        title = params.get("title") or file_name
        pic = params.get("pic") or ""
        fanart = params.get("fanart") or ""

        try:
            res = self.api_client.resolve_play(share_url, code, file_id, file_name=file_name)
        except Exception as e:
            err_msg = str(e)
            if "请先登录" in err_msg or "未登录" in err_msg:
                display_name = CLOUD_PROVIDER_NAMES.get(provider_key, provider_key)
                confirm = self.adapter.show_yes_no_dialog(
                    "登录提示",
                    f"播放此视频需要登录 {display_name}。\n是否立即进行扫码登录？"
                )
                if confirm:
                    success = self.start_qr_login(provider_key)
                    if success:
                        self.adapter.show_notification("扫码登录", f"{display_name} 登录成功，正在重新解析播放...")
                        try:
                            res = self.api_client.resolve_play(share_url, code, file_id, file_name=file_name)
                        except Exception as e2:
                            err_msg2 = str(e2)
                            self.adapter.show_notification("播放失败", err_msg2, is_error=True)
                            link_id = params.get("link_id")
                            if link_id and should_mark_pan_link_invalid(err_msg2):
                                self.api_client.mark_pan_link_invalid(link_id, err_msg2)
                            return
                    else:
                        return
                else:
                    return
            else:
                self.adapter.show_notification("播放失败", err_msg, is_error=True)
                link_id = params.get("link_id")
                if link_id and should_mark_pan_link_invalid(err_msg):
                    self.api_client.mark_pan_link_invalid(link_id, err_msg)
                return

        play_url = res.get("play_url")
        headers = res.get("headers") or {}
        subtitles = res.get("subtitles") or []

        if not play_url:
            self.adapter.log("未解析到可播放直链", "error")
            return

        # 转换为 Kodi 专有格式 URL：play_url | Headers
        if headers:
            header_parts = []
            for k, v in headers.items():
                header_parts.append(f"{k}={urllib.parse.quote(v, safe='')}")
            play_url = f"{play_url}|{'&'.join(header_parts)}"

        # 尝试为网盘多集剧集自动填充完整播放列表
        if share_url and file_id and self.adapter.xbmc and self.adapter.xbmcgui:
            try:
                playlist = self.adapter.xbmc.PlayList(self.adapter.xbmc.PLAYLIST_VIDEO)
                is_from_share = params.get("from_share") == "1" or params.get("from_push") == "1"
                is_115 = str(pan_name).upper() in ["CLOUD115", "115"]
                if is_from_share or is_115:
                    # 来自分享广场/推送的播放或 115 网盘：防止全量递归触发网盘风控，并避免 PWA 遥控器出现全量文件列表
                    playlist.clear()
                    list_item = self.adapter.xbmcgui.ListItem(label=title, path=play_url)
                    list_item.setProperty('IsPlayable', 'true')
                    art = {}
                    if pic:
                        art.update({'icon': pic, 'thumb': pic, 'poster': pic, 'landscape': pic, 'banner': pic})
                    if fanart:
                        art['fanart'] = fanart
                    if art:
                        list_item.setArt(art)
                    list_item.setInfo('video', {"tvshowtitle": vod_title or title, "title": title, "mediatype": "video"})
                    playlist.add(play_url, list_item)
                else:
                    need_rebuild = True
                    if playlist.size() > 1:
                        try:
                            first_path = playlist[0].getPath() if hasattr(playlist[0], 'getPath') else ""
                            quoted_share = urllib.parse.quote(share_url, safe='')
                            if vod_id and f"vod_id={vod_id}" in first_path:
                                need_rebuild = False
                            elif quoted_share and quoted_share in first_path:
                                need_rebuild = False
                        except Exception:
                            need_rebuild = True

                    if need_rebuild:
                        files_data = self.api_client.list_pan_files(share_url, code)
                        files = files_data.get("files") or []
                    if len(files) > 1:
                        playlist.clear()
                        target_pos = 0
                        link_id = params.get("link_id")
                        link_idx = safe_int(params.get("link_idx"), 1)
                        type_id = params.get("type_id")

                        for idx, f in enumerate(files):
                            curr_file_id = f.get("file_id")
                            file_name_item = f.get("file_name")
                            ep_num = parse_episode_number(file_name_item)

                            if vod_title:
                                formatted_name = f"{vod_title}：（{file_name_item}）"
                            else:
                                formatted_name = file_name_item

                            if curr_file_id == file_id:
                                target_path = play_url
                            else:
                                target_path = f"{self.adapter.base_url}?{urllib.parse.urlencode({'action': 'pan_play', 'url': share_url, 'code': code, 'file_id': curr_file_id, 'pan_name': pan_name, 'file_name': file_name_item, 'title': formatted_name, 'vod_title': vod_title, 'vod_id': vod_id, 'link_id': link_id, 'link_idx': link_idx, 'type_id': type_id, 'pic': pic, 'fanart': fanart})}"

                            list_item = self.adapter.xbmcgui.ListItem(label=formatted_name, path=target_path)
                            list_item.setProperty('IsPlayable', 'true')
                            art = {}
                            if pic:
                                art.update({'icon': pic, 'thumb': pic, 'poster': pic, 'landscape': pic, 'banner': pic})
                            if fanart:
                                art['fanart'] = fanart
                            if art:
                                list_item.setArt(art)

                            info = {
                                "tvshowtitle": vod_title or formatted_name,
                                "title": formatted_name,
                                "mediatype": "episode" if ep_num > 0 else "video"
                            }
                            if ep_num > 0:
                                info["episode"] = ep_num
                            list_item.setInfo('video', info)

                            playlist.add(target_path, list_item)
            except Exception as ex:
                self.adapter.log(f"自动构建网盘播放列表失败: {str(ex)}", "warning")

        if self.adapter.xbmcgui and self.adapter.xbmcplugin:
            ep_num = parse_episode_number(file_name or title)
            if vod_title:
                display_title = f"{vod_title}：（{file_name or title}）"
            else:
                display_title = title

            list_item = self.adapter.xbmcgui.ListItem(label=display_title, path=play_url)
            list_item.setProperty('IsPlayable', 'true')

            art = {}
            if pic:
                art.update({'icon': pic, 'thumb': pic, 'poster': pic, 'landscape': pic, 'banner': pic})
            if fanart:
                art['fanart'] = fanart
            if art:
                list_item.setArt(art)

            cat, _ = get_file_category(file_name or title)
            if cat == "audio":
                info = {
                    "title": display_title,
                    "artist": vod_title or display_title
                }
                list_item.setInfo('music', info)
            else:
                info = {
                    "tvshowtitle": vod_title or display_title,
                    "title": display_title,
                    "mediatype": "episode" if ep_num > 0 else "video"
                }
                if ep_num > 0:
                    info["episode"] = ep_num
                list_item.setInfo('video', info)

            # 注入外挂字幕
            sub_urls = []
            for sub in subtitles:
                if sub.get("url"):
                    sub_urls.append(sub.get("url"))
            if sub_urls:
                list_item.setSubtitles(sub_urls)

            self.adapter.set_resolved_url(True, list_item)

            # 保存播放历史
            if vod_title:
                try:
                    profile_dir = self.adapter.get_profile_path()
                    history_mgr = HistoryManager(profile_dir)
                    type_id = params.get("type_id")
                    is_folder = False
                    play_params = {
                        "action": "pan_play",
                        "url": params.get("url"),
                        "code": code,
                        "file_id": file_id,
                        "pan_name": pan_name,
                        "file_name": file_name,
                        "vod_title": vod_title,
                        "vod_id": vod_id
                    }
                    
                    if type_id is not None and str(type_id) != "1":
                        is_folder = True
                        play_params = {
                            "action": "list_files",
                            "url": params.get("url"),
                            "code": code,
                            "pan_name": pan_name,
                            "vod_title": vod_title,
                            "vod_id": vod_id,
                            "link_id": params.get("link_id"),
                            "link_idx": params.get("link_idx"),
                            "type_id": type_id
                        }

                    record = {
                        "vod_title": vod_title,
                        "vod_id": vod_id,
                        "item_name": file_name,
                        "timestamp": time.time(),
                        "is_folder": is_folder,
                        "play_params": play_params
                    }
                    history_mgr.add_record(record)
                except Exception as e:
                    self.adapter.log(f"保存网盘播放历史失败: {str(e)}", "warning")

            self.adapter.xbmcplugin.setResolvedUrl(self.adapter.handle, True, list_item)
        else:
            self.adapter.log(f"Play URL: {play_url}, Subs: {subtitles}")

    # --------------------------------------------------------------------------
    # 6. 网盘扫码登录界面浮层与状态查询
    # --------------------------------------------------------------------------
    def show_my_pans_menu(self):
        """渲染‘我的网盘’主菜单"""
        status_data = self.api_client.get_pan_status() or {}
        pan_order = self.api_client.get_pan_order()
        
        # 保持有序展示
        default_providers = ["ALIYUN", "QUARK", "UC", "BAIDU", "XUNLEI", "CLOUD123", "CLOUD115", "CHINA_MOBILE", "CHINA_TELECOM", "GUANGYA"]
        providers_order = sort_pan_keys(default_providers, pan_order)
        
        for p_key in providers_order:
            default_concurrency = 8
            if p_key == "ALIYUN":
                default_concurrency = 32
            elif p_key == "XUNLEI":
                default_concurrency = 4
            elif p_key == "CLOUD115":
                default_concurrency = 1
                
            p_status = status_data.get(p_key) or {"logged_in": False, "concurrency": default_concurrency}
            logged_in = p_status.get("logged_in", False)
            concurrency = p_status.get("concurrency", default_concurrency)
            is_vip = p_status.get("is_vip", False)
            if p_key == "CLOUD115":
                concurrency = 1
            
            display_name = CLOUD_PROVIDER_NAMES.get(p_key, p_key)
            if logged_in:
                if p_key in ["ALIYUN", "QUARK", "UC"]:
                    status_text = "[已登录-VIP]" if is_vip else "[已登录-普通]"
                else:
                    status_text = "[已登录]"
            else:
                status_text = "[未登录]"
            label = f"{status_text} {display_name} (并发数: {concurrency})"
            
            self.adapter.add_directory_item(
                label,
                {"action": "pan_detail_menu", "provider": p_key},
                is_folder=True
            )
        self.adapter.end_of_directory()

    def show_pan_detail_menu(self, provider):
        """渲染单个网盘的详情操作菜单"""
        status_data = self.api_client.get_pan_status() or {}
        default_concurrency = 8
        if provider == "ALIYUN":
            default_concurrency = 32
        elif provider == "XUNLEI":
            default_concurrency = 4
        elif provider == "CLOUD115":
            default_concurrency = 1
            
        p_status = status_data.get(provider) or {"logged_in": False, "concurrency": default_concurrency, "chunk_size": 0}
        logged_in = p_status.get("logged_in", False)
        concurrency = p_status.get("concurrency", default_concurrency)
        chunk_size = p_status.get("chunk_size", 0)
        is_vip = p_status.get("is_vip", False)
        if provider == "CLOUD115":
            concurrency = 1
        
        display_name = CLOUD_PROVIDER_NAMES.get(provider, provider)
        
        # 0. 浏览个人网盘入口
        if logged_in:
            self.adapter.add_directory_item(
                "浏览个人盘文件 (根目录)",
                {"action": "list_user_pan_files", "provider": provider, "parent_file_id": "root"},
                is_folder=True
            )

        # 1. 状态展示项
        if logged_in:
            if provider in ["ALIYUN", "QUARK", "UC"]:
                status_label = f"当前状态: 已登录 ({'会员账号' if is_vip else '普通账号'})"
            else:
                status_label = "当前状态: 已登录"
        else:
            status_label = "当前状态: 未登录"
        self.adapter.add_directory_item(status_label, {"action": "my_pans_menu"}, is_folder=True)
        
        # 2. 修改并发数项
        if provider == "CLOUD115":
            concurrency_label = f"并发连接数: {concurrency} (防风控，不可修改)"
        else:
            concurrency_label = f"并发连接数: {concurrency} (点击修改)"
        self.adapter.add_directory_item(
            concurrency_label,
            {"action": "pan_concurrency", "provider": provider},
            is_folder=False
        )
        
        # 3. 修改分片连接大小项
        if chunk_size <= 0:
            chunk_size_label = "分片连接大小: 自动 (自适应) (点击修改)"
        elif chunk_size % (1024 * 1024) == 0:
            chunk_size_label = f"分片连接大小: {chunk_size // (1024 * 1024)} MB (点击修改)"
        elif chunk_size % 1024 == 0:
            chunk_size_label = f"分片连接大小: {chunk_size // 1024} KB (点击修改)"
        else:
            chunk_size_label = f"分片连接大小: {chunk_size} 字节 (点击修改)"
        self.adapter.add_directory_item(
            chunk_size_label,
            {"action": "pan_chunk_size", "provider": provider},
            is_folder=False
        )
        
        # 3. 登录 / 退出登录项
        if logged_in:
            self.adapter.add_directory_item(
                "退出登录",
                {"action": "pan_logout", "provider": provider},
                is_folder=False
            )
        else:
            SUPPORTED_QR = ["ALIYUN", "QUARK", "UC", "BAIDU", "XUNLEI", "CLOUD123", "CLOUD115", "CHINA_MOBILE", "CHINA_TELECOM", "GUANGYA"]
            if provider in SUPPORTED_QR:
                self.adapter.add_directory_item(
                    "扫码登录",
                    {"action": "qr_login", "provider": provider},
                    is_folder=False
                )
                self.adapter.add_directory_item(
                    "网页手工输入登录",
                    {"action": "manual_login", "provider": provider},
                    is_folder=False
                )
            else:
                self.adapter.add_directory_item(
                    "此网盘暂不支持在 Kodi 扫码，请使用 Android TV 客户端登录同步",
                    {"action": "pan_detail_menu", "provider": provider},
                    is_folder=False
                )
                
        self.adapter.end_of_directory()

    def handle_pan_logout(self, provider):
        """处理退出登录逻辑"""
        display_name = CLOUD_PROVIDER_NAMES.get(provider, provider)
        confirm = self.adapter.show_yes_no_dialog("退出登录", f"确认退出 {display_name} 的登录状态吗？")
        if confirm:
            res = self.api_client.logout_pan(provider)
            if res.get("success"):
                self.adapter.show_notification("我的网盘", f"{display_name} 退出登录成功")
                self.adapter.refresh_container()
            else:
                self.adapter.show_notification("我的网盘", f"退出登录失败: {res.get('error', '未知错误')}", is_error=True)

    def handle_pan_concurrency(self, provider):
        """处理修改网盘并发数逻辑"""
        if provider == "CLOUD115":
            self.adapter.show_notification("我的网盘", "115网盘已限制为单链接，不可修改")
            return
            
        display_name = CLOUD_PROVIDER_NAMES.get(provider, provider)
        concurrency_options = ["1", "2", "4", "8", "12", "16", "24", "32", "64", "128", "256"]
        default_concurrency = 8
        if provider == "ALIYUN":
            default_concurrency = 32
        elif provider == "XUNLEI":
            default_concurrency = 4
        elif provider == "CLOUD115":
            default_concurrency = 1
            
        status_data = self.api_client.get_pan_status() or {}
        current_concurrency = str((status_data.get(provider) or {}).get("concurrency", default_concurrency))
        
        default_idx = 2
        if current_concurrency in concurrency_options:
            default_idx = concurrency_options.index(current_concurrency)
            
        idx = self.adapter.show_select_dialog(f"设置 {display_name} 的并发连接数", concurrency_options, default_idx)
        if idx >= 0:
            val = int(concurrency_options[idx])
            res = self.api_client.set_pan_concurrency(provider, val)
            if res.get("success"):
                self.adapter.show_notification("我的网盘", f"{display_name} 并发连接数已设置为 {val}")
                self.adapter.refresh_container()
            else:
                self.adapter.show_notification("我的网盘", f"修改并发数失败: {res.get('error', '未知错误')}", is_error=True)

    def handle_pan_chunk_size(self, provider):
        """处理修改网盘分片大小逻辑"""
        display_name = CLOUD_PROVIDER_NAMES.get(provider, provider)
        options = ["自动 (自适应)", "手动设置 (KB)", "手动设置 (MB)"]
        
        idx = self.adapter.show_select_dialog(f"设置 {display_name} 的分片连接大小", options, 0)
        if idx < 0:
            return
            
        if idx == 0:
            res = self.api_client.set_pan_chunk_size(provider, 0)
            if res.get("success"):
                self.adapter.show_notification("我的网盘", f"{display_name} 分片大小已设置为 自动")
                self.adapter.refresh_container()
            else:
                self.adapter.show_notification("我的网盘", f"设置失败: {res.get('error', '未知错误')}", is_error=True)
        elif idx == 1:
            status_data = self.api_client.get_pan_status() or {}
            curr_size = (status_data.get(provider) or {}).get("chunk_size", 0)
            default_kb = ""
            if curr_size > 0 and curr_size % 1024 == 0:
                default_kb = str(curr_size // 1024)
            val_str = self.adapter.show_numeric_dialog(f"输入分片大小(KB) [128 - 4096]", default_kb)
            if not val_str:
                return
            try:
                val = int(val_str)
                if val < 128 or val > 4096:
                    self.adapter.show_notification("输入越界", "KB模式分片限制在 128 ~ 4096 之间", is_error=True)
                    return
                bytes_val = val * 1024
                res = self.api_client.set_pan_chunk_size(provider, bytes_val)
                if res.get("success"):
                    self.adapter.show_notification("我的网盘", f"{display_name} 分片大小已设置为 {val} KB")
                    self.adapter.refresh_container()
                else:
                    self.adapter.show_notification("我的网盘", f"设置失败: {res.get('error', '未知错误')}", is_error=True)
            except ValueError:
                self.adapter.show_notification("输入错误", "请输入有效的正整数", is_error=True)
        elif idx == 2:
            status_data = self.api_client.get_pan_status() or {}
            curr_size = (status_data.get(provider) or {}).get("chunk_size", 0)
            default_mb = ""
            if curr_size > 0 and curr_size % (1024 * 1024) == 0:
                default_mb = str(curr_size // (1024 * 1024))
            val_str = self.adapter.show_numeric_dialog(f"输入分片大小(MB) [1 - 64]", default_mb)
            if not val_str:
                return
            try:
                val = int(val_str)
                if val < 1 or val > 64:
                    self.adapter.show_notification("输入越界", "MB模式分片限制在 1 ~ 64 之间", is_error=True)
                    return
                bytes_val = val * 1024 * 1024
                res = self.api_client.set_pan_chunk_size(provider, bytes_val)
                if res.get("success"):
                    self.adapter.show_notification("我的网盘", f"{display_name} 分片大小已设置为 {val} MB")
                    self.adapter.refresh_container()
                else:
                    self.adapter.show_notification("我的网盘", f"设置失败: {res.get('error', '未知错误')}", is_error=True)
            except ValueError:
                self.adapter.show_notification("输入错误", "请输入有效的正整数", is_error=True)

    def list_user_pan_files(self, params):
        """渲染个人网盘的文件和文件夹列表"""
        provider = params.get("provider") or ""
        parent_file_id = params.get("parent_file_id") or ""
        dir_name = params.get("dir_name") or CLOUD_PROVIDER_NAMES.get(provider, provider)

        try:
            res_data = self.api_client.get_user_files(provider, parent_file_id)
            files = res_data.get("files") or []

            if not files:
                self.adapter.add_directory_item("(空文件夹)", {"action": "root"}, is_folder=False)
                self.adapter.end_of_directory()
                return

            self.adapter.set_content("episodes")

            for file in files:
                file_id = file.get("file_id") or ""
                file_name = file.get("file_name") or ""
                is_folder = file.get("is_folder", False)
                size_label = file.get("size_label") or ""

                if is_folder:
                    label = f"[目录] {file_name}"
                    self.adapter.add_directory_item(
                        label,
                        {
                            "action": "list_user_pan_files",
                            "provider": provider,
                            "parent_file_id": file_id,
                            "dir_name": file_name,
                        },
                        is_folder=True
                    )
                else:
                    cat, ext = get_file_category(file_name)
                    if cat in ["video", "audio", "image"]:
                        tag = "[视频]" if cat == "video" else ("[音频]" if cat == "audio" else "[图片]")
                        label = f"{tag} {file_name}"
                        if size_label:
                            label += f" [{size_label}]"

                        ep_num = parse_episode_number(file_name) if cat == "video" else 0
                        info = {
                            "tvshowtitle": file_name,
                            "title": file_name,
                            "mediatype": "episode" if ep_num > 0 else ("song" if cat == "audio" else "video")
                        }
                        if ep_num > 0:
                            info["episode"] = ep_num

                        self.adapter.add_directory_item(
                            label,
                            {
                                "action": "play_user_pan",
                                "provider": provider,
                                "file_id": file_id,
                                "file_name": file_name,
                                "vod_title": file_name,
                            },
                            is_folder=False,
                            is_playable=True,
                            info=info
                        )
                    else:
                        label = f"[文件] {file_name}"
                        if size_label:
                            label += f" [{size_label}]"

                        self.adapter.add_directory_item(
                            label,
                            {
                                "action": "unsupported_file",
                                "file_name": file_name,
                                "ext": ext,
                            },
                            is_folder=False,
                            is_playable=False
                        )

            self.adapter.end_of_directory()
        except Exception as e:
            err_msg = str(e)
            self.adapter.show_notification(f"浏览{CLOUD_PROVIDER_NAMES.get(provider, provider)}失败", err_msg, is_error=True)
            self.adapter.add_directory_item(f"获取列表失败: {err_msg}", {"action": "root"}, is_folder=False)
            self.adapter.end_of_directory()

    def handle_unsupported_file(self, params):
        """处理点击非媒体格式文件的提示"""
        file_name = params.get("file_name") or "文件"
        ext = params.get("ext") or ""
        self.adapter.show_notification("无法播放", f"{file_name}\n为非媒体格式 ({ext})，无法播放", is_error=True)

    def play_user_pan(self, params):
        """处理个人盘文件解析与播放"""
        self.adapter.log(f"=== PLAY USER PAN TRIGGERED: {params} ===")
        provider = params.get("provider") or ""
        file_id = params.get("file_id") or ""
        file_name = params.get("file_name") or ""
        vod_title = params.get("vod_title") or file_name

        try:
            res = self.api_client.resolve_user_play(provider, file_id, file_name)
            play_url = res.get("play_url")
            if not play_url:
                raise Exception("未获取到播放链接")

            headers = res.get("headers") or {}
            if headers:
                header_parts = []
                for k, v in headers.items():
                    header_parts.append(f"{k}={urllib.parse.quote(v, safe='')}")
                play_url = f"{play_url}|{'&'.join(header_parts)}"

            if vod_title:
                try:
                    profile_dir = self.adapter.get_profile_path()
                    history_mgr = HistoryManager(profile_dir)
                    history_record = {
                        "title": vod_title,
                        "play_params": {
                            "action": "play_user_pan",
                            "provider": provider,
                            "file_id": file_id,
                            "url": play_url
                        },
                        "time": int(time.time())
                    }
                    history_mgr.add_record(history_record)
                except Exception as ex:
                    self.adapter.log(f"保存个人盘播放历史失败: {str(ex)}", "warning")

            list_item = self.adapter.xbmcgui.ListItem(vod_title, "", play_url)
            list_item.setProperty('IsPlayable', 'true')
            list_item.setPath(play_url)
            
            cat, _ = get_file_category(file_name or vod_title)
            if cat == "audio":
                info = {
                    "title": vod_title,
                    "artist": vod_title
                }
                list_item.setInfo('music', info)
            else:
                info = {
                    "tvshowtitle": vod_title,
                    "title": vod_title,
                    "mediatype": "video"
                }
                list_item.setInfo('video', info)

            self.adapter.set_resolved_url(True, list_item)
        except Exception as e:
            err_msg = str(e)
            self.adapter.log(f"=== PLAY USER PAN ERROR: {err_msg} ===", "error")
            self.adapter.show_notification("播放失败", err_msg, is_error=True)
            list_item = self.adapter.xbmcgui.ListItem(vod_title, "", "")
            self.adapter.set_resolved_url(False, list_item)

    def start_qr_login(self, provider):
        """弹出扫码登录浮层对话框"""
        try:
            from qrcode_dialog import QrCodeLoginDialog
            dialog = QrCodeLoginDialog(provider, self.api_client)
            success = dialog.start()
            if success:
                import time
                time.sleep(0.3)
                self.adapter.refresh_container()
            return success
        except Exception as e:
            self.adapter.log(f"启动扫码登录失败: {str(e)}", "error")
            return False

    def start_manual_login(self, provider):
        """弹出网页手工输入登录浮层对话框"""
        try:
            from qrcode_dialog import QrCodeLoginDialog
            dialog = QrCodeLoginDialog(provider, self.api_client, manual_mode=True)
            success = dialog.start()
            if success:
                import time
                time.sleep(0.3)
                self.adapter.refresh_container()
            return success
        except Exception as e:
            self.adapter.log(f"启动网页手工输入登录失败: {str(e)}", "error")
            return False

    # --------------------------------------------------------------------------
    # 辅助方法
    # --------------------------------------------------------------------------
    def _render_video_item(self, v):
        title = v.get("title")
        remark = v.get("remark")
        label = f"{title} [{remark}]" if remark else title
        pic = get_pic_url(v)
        fanart = get_backdrop_url(v)
        info = {
            "title": title,
            "plot": v.get("content"),
            "year": v.get("year"),
            "genre": v.get("tags"),
            "director": v.get("director"),
            "cast": [cast.strip() for cast in (v.get("starring") or "").split(",") if cast.strip()]
        }
        self.adapter.add_directory_item(
            label,
            {"action": "detail", "id": v.get("id")},
            is_folder=True,
            icon=pic,
            pic=pic,
            fanart=fanart,
            info=info
        )

    # --------------------------------------------------------------------------
    # 播放历史记录交互方法
    # --------------------------------------------------------------------------
    def show_history(self):
        """显示播放历史记录"""
        profile_dir = self.adapter.get_profile_path()
        history_mgr = HistoryManager(profile_dir)
        records = history_mgr.load_history()

        # 顶部展示“清空播放记录”
        if records:
            self.adapter.add_directory_item(
                "❌ 清空播放记录",
                {"action": "clear_history"},
                is_folder=False
            )

        for rec in records:
            vod_title = rec.get("vod_title")
            item_name = rec.get("item_name")
            label = f"{vod_title} - {item_name}"
            play_params = rec.get("play_params") or {}
            
            # 使用播放记录中的实际播放动作及参数（可能已被重写为多集目录动作）
            is_folder = rec.get("is_folder", False)
            self.adapter.add_directory_item(
                label,
                play_params,
                is_folder=is_folder,
                is_playable=not is_folder
            )
            
        if not records:
            self.adapter.add_directory_item("暂无播放记录", {"action": "root"}, is_folder=False)
            
        self.adapter.end_of_directory()

    def clear_history(self):
        """清空所有播放历史"""
        confirm = self.adapter.show_yes_no_dialog("清空历史", "确认清空所有播放记录吗？")
        if confirm:
            profile_dir = self.adapter.get_profile_path()
            history_mgr = HistoryManager(profile_dir)
            history_mgr.clear_history()
            self.adapter.show_notification("播放记录", "播放历史已成功清空")
            self.adapter.refresh_container()

    # --------------------------------------------------------------------------
    # 网页推送分享播放逻辑
    # --------------------------------------------------------------------------
    def show_push_menu(self):
        """显示推送主菜单与推送历史记录"""
        self.adapter.add_directory_item("新增推送...", {"action": "new_push"}, is_folder=False)

        profile_dir = self.adapter.get_profile_path()
        push_mgr = PushHistoryManager(profile_dir)
        records = push_mgr.load_history()

        if records:
            self.adapter.add_directory_item("❌ 清理历史记录", {"action": "clear_push_history"}, is_folder=False)

        for rec in records:
            url = rec.get("url")
            code = rec.get("code") or ""
            title = rec.get("title") or url
            pan_name = rec.get("pan_name") or ""
            provider_display = CLOUD_PROVIDER_NAMES.get(pan_name, pan_name or "未知网盘")
            
            label = f"[{provider_display}] {title}"
            self.adapter.add_directory_item(
                label,
                {
                    "action": "list_files",
                    "url": url,
                    "code": code,
                    "pan_name": pan_name,
                    "vod_title": title
                },
                is_folder=True
            )

        if not records:
            self.adapter.add_directory_item("暂无推送历史记录", {"action": "push_menu"}, is_folder=False)

        self.adapter.end_of_directory()

    def start_push_dialog(self):
        """启动网页推送的 QR 浮层对话框"""
        try:
            from qrcode_dialog import PushQrDialog
            dialog = PushQrDialog(self.api_client)
            dialog.start()
            # 推送成功后可能由背景服务直接拉起播放，也可能在此手动刷新容器
            self.adapter.refresh_container()
            return True
        except Exception as e:
            self.adapter.log(f"启动推送浮层失败: {str(e)}", "error")
            return False

    def clear_push_history(self):
        """清理所有网页推送历史记录"""
        confirm = self.adapter.show_yes_no_dialog("清空历史", "确认清空所有推送历史记录吗？")
        if confirm:
            profile_dir = self.adapter.get_profile_path()
            push_mgr = PushHistoryManager(profile_dir)
            push_mgr.clear_history()
            self.adapter.show_notification("推送历史", "推送历史记录已清空")
            self.adapter.refresh_container()

    def _guess_provider_key(self, url):
        lower = url.lower()
        if "aliyundrive" in lower or "alipan" in lower:
            return "ALIYUN"
        if "quark.cn" in lower:
            return "QUARK"
        if "uc.cn" in lower:
            return "UC"
        if "baidu.com" in lower:
            return "BAIDU"
        if "xunlei" in lower:
            return "XUNLEI"
        if "123pan" in lower or "123684" in lower or "123865" in lower or "123278" in lower:
            return "CLOUD123"
        if "115" in lower:
            return "CLOUD115"
        if "139.com" in lower:
            return "CHINA_MOBILE"
        if "189.cn" in lower:
            return "CHINA_TELECOM"
        if "guangyapan" in lower:
            return "GUANGYA"
        return ""

    def _clean_title(self, text):
        """清洗标题中的高位 Emoji (码点 > 0xFFFF) 与 Dingbats/符号图标 (0x2600-0x27BF, 0x2B00-0x2BFF)"""
        if not text:
            return ""
        cleaned = "".join(
            c for c in text
            if not (
                0x2600 <= ord(c) <= 0x27BF or
                0x2B00 <= ord(c) <= 0x2BFF or
                0x1F000 <= ord(c) <= 0x1FFFF or
                ord(c) > 0xFFFF or
                ord(c) in (0xFE0F, 0xFE0E)
            )
        )
        return cleaned.strip()

    def show_pansou_menu(self):
        """渲染盘搜二级菜单"""
        self.adapter.add_directory_item("新增搜索", {"action": "pansou_search"}, is_folder=True)
        self.adapter.add_directory_item("设置", {"action": "pansou_settings"}, is_folder=True)
        self.adapter.end_of_directory()

    def show_pansou_results(self, query):
        """显示盘搜搜索结果"""
        keyword = query
        if not keyword:
            keyword = self.adapter.show_keyboard_input("", "请输入盘搜关键词")
            if not keyword:
                self.adapter.end_of_directory()
                return
            keyword = keyword.strip()

        try:
            res = self.api_client.search_pansou(keyword)
            merged_by_type = res.get("merged_by_type") or {}
            
            pan_name_mapping = {
                "aliyun": "阿里云盘",
                "quark": "夸克网盘",
                "uc": "UC网盘",
                "baidu": "百度网盘",
                "xunlei": "迅雷网盘",
                "123": "123云盘",
                "115": "115网盘",
                "mobile": "移动云盘",
                "tianyi": "天翼云盘",
                "guangya": "光鸭云盘"
            }

            has_items = False
            for pan_type, items in merged_by_type.items():
                if not items:
                    continue
                pan_label = pan_name_mapping.get(pan_type.lower(), pan_type)
                for item in items:
                    note = item.get("note") or ""
                    cleaned_note = self._clean_title(note)
                    title = f"[{pan_label}] {cleaned_note}"
                    share_url = item.get("url") or ""
                    code = item.get("password") or ""
                    
                    pan_provider_key = self._guess_provider_key(share_url)
                    
                    self.adapter.add_directory_item(
                        title,
                        {
                            "action": "list_files",
                            "url": share_url,
                            "code": code,
                            "pan_name": pan_provider_key,
                            "vod_title": cleaned_note,
                            "vod_id": "pansou"
                        },
                        is_folder=True
                    )
                    has_items = True
            
            if not has_items:
                self.adapter.show_notification("盘搜", "未找到任何相关资源")
            
            self.adapter.end_of_directory()
        except Exception as e:
            self.adapter.show_notification("盘搜", f"搜索出错: {str(e)}", is_error=True)
            self.adapter.end_of_directory()

    def show_pansou_settings_page(self, params):
        """显示盘搜设置列表页"""
        if "addr" not in params:
            config = self.api_client.get_pansou_config()
            addr = config.get("addr") or "https://so.252035.xyz"
            username = config.get("username") or ""
            password = config.get("password") or ""
            pan_types = ",".join(config.get("pan_types") or [])
        else:
            addr = params.get("addr") or "https://so.252035.xyz"
            username = params.get("username") or ""
            password = params.get("password") or ""
            pan_types = params.get("pan_types") or ""

        edit_field = params.get("edit_field")
        if edit_field == "addr":
            new_val = self.adapter.show_keyboard_input(addr, "设置盘搜服务器地址")
            if new_val is not None:
                new_val = new_val.strip()
                addr = new_val if new_val else "https://so.252035.xyz"

        elif edit_field == "username":
            new_val = self.adapter.show_keyboard_input(username, "设置账号 (可选)")
            if new_val is not None:
                username = new_val.strip()

        elif edit_field == "password":
            new_val = self.adapter.show_keyboard_input(password, "设置密码 (可选)", hidden=True)
            if new_val is not None:
                password = new_val.strip()

        elif edit_field == "pan_types":
            ALL_PANS_KODI = [
                ("aliyun", "阿里云盘"),
                ("quark", "夸克网盘"),
                ("uc", "UC网盘"),
                ("baidu", "百度网盘"),
                ("xunlei", "迅雷网盘"),
                ("123", "123云盘"),
                ("115", "115网盘"),
                ("mobile", "移动云盘"),
                ("tianyi", "天翼云盘"),
                ("guangya", "光鸭云盘")
            ]
            current_selected = [p for p in pan_types.split(",") if p]
            preselect = []
            options = [item[1] for item in ALL_PANS_KODI]
            for idx, item in enumerate(ALL_PANS_KODI):
                if len(current_selected) == 0 or item[0] in current_selected:
                    preselect.append(idx)

            selected_indices = self.adapter.show_multiselect_dialog("选择需要搜索的网盘 (默认全选)", options, preselect=preselect)
            if selected_indices is not None and len(selected_indices) > 0:
                selected_pans = [ALL_PANS_KODI[idx][0] for idx in selected_indices]
                pan_types = ",".join(selected_pans)

        display_pass = "******" if password else "未设置"
        selected_pans = [p for p in pan_types.split(",") if p]
        display_pans = f"已选择 {len(selected_pans)} 个网盘" if selected_pans else "全选"

        common_params = {
            "addr": addr,
            "username": username,
            "password": password,
            "pan_types": pan_types
        }

        addr_params = common_params.copy()
        addr_params.update({"action": "pansou_settings", "edit_field": "addr"})
        self.adapter.add_directory_item(f"盘搜地址：{addr}", addr_params, is_folder=True)

        user_params = common_params.copy()
        user_params.update({"action": "pansou_settings", "edit_field": "username"})
        self.adapter.add_directory_item(f"账号：{username or '未设置'}", user_params, is_folder=True)

        pass_params = common_params.copy()
        pass_params.update({"action": "pansou_settings", "edit_field": "password"})
        self.adapter.add_directory_item(f"密码：{display_pass}", pass_params, is_folder=True)

        pans_params = common_params.copy()
        pans_params.update({"action": "pansou_settings", "edit_field": "pan_types"})
        self.adapter.add_directory_item(f"搜索网盘：{display_pans}", pans_params, is_folder=True)

        save_params = common_params.copy()
        save_params.update({"action": "pansou_settings_save"})
        self.adapter.add_directory_item(">> [ 保存并应用设置 ]", save_params, is_folder=True)

        self.adapter.end_of_directory()

    def save_pansou_settings(self, params):
        """保存盘搜配置"""
        addr = params.get("addr") or "https://so.252035.xyz"
        username = params.get("username") or ""
        password = params.get("password") or ""
        pan_types = params.get("pan_types") or ""

        pan_types_list = [p for p in pan_types.split(",") if p]
        if not pan_types_list:
            pan_types_list = ["aliyun", "quark", "uc", "baidu", "xunlei", "123", "115", "mobile", "tianyi", "guangya"]

        try:
            self.api_client.save_pansou_config(addr, username, password, pan_types_list)
            self.adapter.show_notification("设置", "盘搜配置保存成功")
            self.adapter.update_container({"action": "pansou_menu"}, replace=True)
        except Exception as e:
            self.adapter.show_notification("设置", f"保存失败: {str(e)}", is_error=True)

        self.adapter.end_of_directory()

    def show_share_square(self, params):
        """渲染分享广场页面"""
        page = safe_int(params.get("page"), 1)
        res = self.api_client.get_shares(page=page, page_size=20)
        shares_list = res.get("list") or []
        total = res.get("total") or 0
        page_size = res.get("pageSize") or 20

        # 顶部固定: 我的收藏
        self.adapter.add_directory_item("[我的收藏]", {"action": "share_favorites"}, is_folder=True)

        pan_label_mapping = {
            "ALIYUN": "阿里云盘", "QUARK": "夸克网盘", "UC": "UC网盘",
            "BAIDU": "百度网盘", "XUNLEI": "迅雷网盘", "CLOUD123": "123云盘",
            "CLOUD115": "115网盘", "CHINA_MOBILE": "移动云盘",
            "CHINA_TELECOM": "天翼云盘", "GUANGYA": "光鸭云盘"
        }

        for s in shares_list:
            s_id = s.get("id")
            s_url = s.get("url")
            s_title = self._clean_title(s.get("title") or "")
            s_sharer = self._clean_title(s.get("sharer") or "匿名")
            s_pan_type = s.get("panType")
            s_pwd = s.get("pwd") or ""
            s_created_at = (s.get("createdAt") or "")[:10]
            is_fav = s.get("isFavorited", False)

            pan_name = pan_label_mapping.get(s_pan_type, s_pan_type)
            fav_prefix = "[已收藏] " if is_fav else ""
            display_title = f"{fav_prefix}[{pan_name}] {s_title} (分享者: {s_sharer} | {s_created_at})"

            self.adapter.add_directory_item(
                display_title,
                {
                    "action": "share_detail",
                    "share_id": s_id,
                    "url": s_url,
                    "title": s_title,
                    "sharer": s_sharer,
                    "pan_name": s_pan_type,
                    "code": s_pwd,
                    "is_favorited": "1" if is_fav else "0"
                },
                is_folder=True
            )

        total_pages = (total + page_size - 1) // page_size if page_size > 0 else 1
        if page < total_pages:
            self.adapter.add_directory_item(
                f"下一页 (第 {page + 1} / {total_pages} 页)",
                {"action": "share_square", "page": page + 1},
                is_folder=True
            )
        if page > 1:
            self.adapter.add_directory_item(
                f"上一页 (第 {page - 1} / {total_pages} 页)",
                {"action": "share_square", "page": page - 1},
                is_folder=True
            )

        self.adapter.end_of_directory()

    def show_share_favorites(self, params):
        """渲染我的收藏列表页面"""
        page = safe_int(params.get("page"), 1)
        res = self.api_client.get_share_favorites(page=page, page_size=20)
        shares_list = res.get("list") or []
        total = res.get("total") or 0
        page_size = res.get("pageSize") or 20

        pan_label_mapping = {
            "ALIYUN": "阿里云盘", "QUARK": "夸克网盘", "UC": "UC网盘",
            "BAIDU": "百度网盘", "XUNLEI": "迅雷网盘", "CLOUD123": "123云盘",
            "CLOUD115": "115网盘", "CHINA_MOBILE": "移动云盘",
            "CHINA_TELECOM": "天翼云盘", "GUANGYA": "光鸭云盘"
        }

        if not shares_list:
            self.adapter.add_directory_item("暂无收藏的分享", {"action": "share_square"}, is_folder=False)

        for s in shares_list:
            s_id = s.get("id")
            s_url = s.get("url")
            s_title = self._clean_title(s.get("title") or "")
            s_sharer = self._clean_title(s.get("sharer") or "匿名")
            s_pan_type = s.get("panType")
            s_pwd = s.get("pwd") or ""
            s_created_at = (s.get("createdAt") or "")[:10]

            pan_name = pan_label_mapping.get(s_pan_type, s_pan_type)
            display_title = f"[已收藏] [{pan_name}] {s_title} (分享者: {s_sharer} | {s_created_at})"

            self.adapter.add_directory_item(
                display_title,
                {
                    "action": "share_detail",
                    "share_id": s_id,
                    "url": s_url,
                    "title": s_title,
                    "sharer": s_sharer,
                    "pan_name": s_pan_type,
                    "code": s_pwd,
                    "is_favorited": "1"
                },
                is_folder=True
            )

        total_pages = (total + page_size - 1) // page_size if page_size > 0 else 1
        if page < total_pages:
            self.adapter.add_directory_item(
                f"下一页 (第 {page + 1} / {total_pages} 页)",
                {"action": "share_favorites", "page": page + 1},
                is_folder=True
            )
        if page > 1:
            self.adapter.add_directory_item(
                f"上一页 (第 {page - 1} / {total_pages} 页)",
                {"action": "share_favorites", "page": page - 1},
                is_folder=True
            )

        self.adapter.end_of_directory()

    def show_share_detail(self, params):
        """点入分享后的详情与网盘目录树 (支持按层级下钻与分页)"""
        share_id = params.get("share_id") or ""
        is_fav = params.get("is_favorited") == "1"
        s_title = self._clean_title(params.get("title") or "")
        raw_url = params.get("url") or ""
        s_pwd = params.get("code") or ""
        pan_name = params.get("pan_name") or ""
        parent_file_id = params.get("parent_file_id") or ""
        page = safe_int(params.get("page"), 1)
        page_size = safe_int(params.get("page_size"), 50)

        # 使用 to_share_url 自动还原为带完整域名的标准 share_url
        share_url = to_share_url(raw_url, pan_name)

        # 仅在根层级 (parent_file_id 为空) 且第 1 页时展示顶部“收藏/取消收藏”按钮
        if not parent_file_id and page == 1:
            if is_fav:
                self.adapter.add_directory_item(
                    "[取消收藏]",
                    {"action": "share_toggle_favorite", "share_id": share_id, "op": "unfavorite"},
                    is_folder=False
                )
            else:
                self.adapter.add_directory_item(
                    "[收藏本分享]",
                    {"action": "share_toggle_favorite", "share_id": share_id, "op": "favorite"},
                    is_folder=False
                )

        try:
            # 调取分享专用的单层列表 (flat=0)
            res_data = self.api_client.list_pan_files(share_url, s_pwd, parent_file_id=parent_file_id, flat="0", page=page, page_size=page_size)
            files = res_data.get("files") or []
            total = safe_int(res_data.get("total"), 0)
            total_page = safe_int(res_data.get("total_page"), 1)
            has_more = bool(res_data.get("has_more", False))

            if not files:
                self.adapter.add_directory_item("(无可播放文件)", {"action": "root"}, is_folder=False)
                self.adapter.end_of_directory()
                return

            self.adapter.set_content("episodes")

            if page > 1:
                prev_params = params.copy()
                prev_params["page"] = page - 1
                prev_title = f"上一页 (第 {page - 1}/{total_page} 页)" if total_page > 1 else f"上一页 (第 {page - 1} 页)"
                self.adapter.add_directory_item(
                    prev_title,
                    prev_params,
                    is_folder=True
                )

            for file in files:
                file_id = file.get("file_id") or ""
                file_name = self._clean_title(file.get("file_name") or "")
                is_folder = file.get("is_folder", False)
                size_label = file.get("size_label") or ""

                if is_folder:
                    label = f"[目录] {file_name}"
                    self.adapter.add_directory_item(
                        label,
                        {
                            "action": "share_detail",
                            "share_id": share_id,
                            "is_favorited": "1" if is_fav else "0",
                            "title": s_title,
                            "url": share_url,
                            "code": s_pwd,
                            "pan_name": pan_name,
                            "parent_file_id": file_id,
                        },
                        is_folder=True
                    )
                else:
                    cat, ext = get_file_category(file_name)
                    if cat in ["video", "audio"]:
                        label = file_name
                        if size_label:
                            label += f" [{size_label}]"

                        ep_num = parse_episode_number(file_name) if cat == "video" else 0
                        info = {
                            "tvshowtitle": s_title or file_name,
                            "title": file_name,
                            "mediatype": "episode" if ep_num > 0 else ("song" if cat == "audio" else "video")
                        }
                        if ep_num > 0:
                            info["episode"] = ep_num

                        play_params = {
                            "action": "pan_play",
                            "url": share_url,
                            "code": s_pwd,
                            "file_id": file_id,
                            "pan_name": pan_name,
                            "file_name": file_name,
                            "vod_title": s_title,
                            "vod_id": f"share_{share_id}"
                        }

                        self.adapter.add_directory_item(
                            label,
                            play_params,
                            is_folder=False,
                            info=info
                        )
                    else:
                        tag = "[字幕]" if cat == "subtitle" else ("[文档]" if cat == "document" else ("[压缩包]" if cat == "archive" else ("[图片]" if cat == "image" else "[文件]")))
                        plain_label = f"{tag} {file_name}"
                        if size_label:
                            plain_label += f" [{size_label}]"
                        label = f"[COLOR grey]{plain_label}[/COLOR]"
                        self.adapter.add_directory_item(
                            label,
                            {
                                "action": "ignore"
                            },
                            is_folder=False
                        )

            if has_more or (total_page > 1 and page < total_page):
                next_params = params.copy()
                next_params["page"] = page + 1
                next_title = f"下一页 (第 {page + 1}/{total_page} 页)" if total_page > 1 else f"下一页 (第 {page + 1} 页)"
                self.adapter.add_directory_item(
                    next_title,
                    next_params,
                    is_folder=True
                )

            self.adapter.end_of_directory()

        except Exception as e:
            err_msg = str(e)
            if "请先登录" in err_msg or "未登录" in err_msg:
                provider_key = CLOUD_PROVIDER_KEYS.get(str(pan_name), pan_name)
                display_name = CLOUD_PROVIDER_NAMES.get(provider_key, provider_key)
                confirm = self.adapter.show_yes_no_dialog(
                    "登录提示",
                    f"查看此分享内容需要登录 {display_name}。\n是否立即进行扫码登录？"
                )
                if confirm:
                    success = self.start_qr_login(provider_key)
                    if success:
                        self.show_share_detail(params)
                        return
            self.adapter.show_notification("网盘解析错误", err_msg, is_error=True)
            self.adapter.end_of_directory()

    def handle_share_toggle_favorite(self, params):
        """处理收藏/取消收藏动作"""
        share_id = params.get("share_id")
        op = params.get("op")
        try:
            if op == "favorite":
                res = self.api_client.favorite_share(share_id)
                if res and res.get("code") == "SUCCESS":
                    self.adapter.show_notification("分享广场", "收藏成功")
                else:
                    self.adapter.show_notification("分享广场", f"收藏失败: {res.get('error', '未知错误')}", is_error=True)
            else:
                res = self.api_client.unfavorite_share(share_id)
                if res and res.get("code") == "SUCCESS":
                    self.adapter.show_notification("分享广场", "已取消收藏")
                else:
                    self.adapter.show_notification("分享广场", f"操作失败: {res.get('error', '未知错误')}", is_error=True)
            self.adapter.refresh_container()
        except Exception as e:
            self.adapter.show_notification("分享广场", f"操作失败: {str(e)}", is_error=True)

