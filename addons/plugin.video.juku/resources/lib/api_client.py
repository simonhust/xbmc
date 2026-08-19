# -*- coding: utf-8 -*-
import urllib.request
import urllib.parse
import urllib.error
import json

# 绕过系统代理：为防止用户系统代理设置导致访问本地回环 127.0.0.1 失败，
# 全局为 urllib.request 注册一个无代理（Empty ProxyHandler）的 opener。
try:
    _no_proxy_handler = urllib.request.ProxyHandler({})
    _no_proxy_opener = urllib.request.build_opener(_no_proxy_handler)
    urllib.request.install_opener(_no_proxy_opener)
except Exception:
    pass

class ApiClient:
    """
    对接本地 Go 服务反向代理主站的 Python 封装，使用 urllib 代替 requests。
    """
    def __init__(self, session=None):
        # 无参构造时，默认 base_url 为本地 Go 服务反代理路由，daemon_url 为本地守护进程根路径
        self.base_url = "http://127.0.0.1:17654/api/mv"
        self.daemon_url = "http://127.0.0.1:17654"

    def _request_json(self, url, params=None, timeout=10):
        if params:
            # 过滤掉 None 值，避免 urlencode 处理成 'None' 字符串
            filtered_params = {k: v for k, v in params.items() if v is not None}
            query_string = urllib.parse.urlencode(filtered_params)
            if '?' in url:
                url = f"{url}&{query_string}"
            else:
                url = f"{url}?{query_string}"
        
        try:
            req = urllib.request.Request(url)
            with urllib.request.urlopen(req, timeout=timeout) as response:
                data = response.read().decode('utf-8')
                return json.loads(data)
        except Exception as e:
            err_msg = str(e)
            # 尝试解析 HTTPError 的响应体以获取后端具体错误
            if isinstance(e, urllib.error.HTTPError):
                try:
                    body = e.read().decode('utf-8')
                    err_data = json.loads(body)
                    if err_data.get("error"):
                        err_msg = err_data["error"]
                except Exception:
                    pass

            try:
                import xbmc
                xbmc.log(f"MvPlugin ApiClient request error on {url}: {err_msg}", 3) # LOGERROR
            except ImportError:
                print(f"MvPlugin ApiClient request error on {url}: {err_msg}")
            raise Exception(err_msg)

    def get_home_data(self):
        """获取首页板块数据 (getHomeData)"""
        url = f"{self.base_url}/videos/getHomeData"
        try:
            res_json = self._request_json(url, timeout=10)
            if res_json.get("success"):
                return res_json.get("data") or {}
        except Exception:
            pass
        return {}

    def get_recommendations(self):
        """获取个性化推荐"""
        url = f"{self.base_url}/videos/recommend"
        try:
            res_json = self._request_json(url, timeout=10)
            if res_json.get("success"):
                return res_json.get("data") or []
        except Exception:
            pass
        return []

    def get_playlist_categories(self):
        """获取所有片单分类"""
        url = f"{self.base_url}/lists/categories"
        try:
            res_json = self._request_json(url, timeout=10)
            if res_json.get("success"):
                return res_json.get("data") or []
        except Exception:
            pass
        return []

    def get_playlist_detail(self, slug):
        """获取片单详情"""
        url = f"{self.base_url}/lists/{urllib.parse.quote(slug)}"
        try:
            res_json = self._request_json(url, timeout=10)
            if res_json.get("success"):
                return res_json.get("data") or {}
        except Exception:
            pass
        return {}

    def search_videos(self, query, page=1, page_size=20):
        """关键字搜索视频"""
        url = f"{self.base_url}/videos/search"
        params = {"q": query, "page": page, "pageSize": page_size}
        try:
            res_json = self._request_json(url, params=params, timeout=10)
            if res_json.get("success"):
                return res_json.get("data") or {}
        except Exception:
            pass
        return {"items": [], "pagination": {"page": 1, "pageSize": page_size, "total": 0, "totalPages": 1}}

    def get_video_detail(self, video_id):
        """获取视频详情"""
        url = f"{self.base_url}/videos/{int(video_id)}"
        try:
            res_json = self._request_json(url, timeout=10)
            if res_json.get("success"):
                return res_json.get("data") or {}
        except Exception:
            pass
        return {}

    def get_tags(self):
        """获取视频标签 (getTags)"""
        url = f"{self.base_url}/videos/tags"
        try:
            res_json = self._request_json(url, timeout=10)
            if res_json.get("success"):
                return res_json.get("data") or []
        except Exception:
            pass
        return []

    def get_category_videos(self, type_id, year=None, area=None, tag_id=0, page=1, page_size=20, genre=None):
        """分类筛选视频 list"""
        url = f"{self.base_url}/videos"
        params = {"typeId": type_id, "page": page, "pageSize": page_size}
        if year:
            params["year"] = year
        if area:
            params["area"] = area
        if tag_id > 0:
            params["tagId"] = tag_id
        if genre:
            params["genre"] = genre
        try:
            res_json = self._request_json(url, params=params, timeout=10)
            if res_json.get("success"):
                return res_json.get("data") or {}
        except Exception:
            pass
        return {"items": [], "pagination": {"page": 1, "pageSize": page_size, "total": 0, "totalPages": 1}}

    # ========================== 对接本地 Go Daemon 服务 ==========================

    def list_pan_files(self, share_url, code="", parent_file_id="", flat="", page=1, page_size=50):
        """获取网盘文件列表"""
        url = f"{self.daemon_url}/api/list"
        params = {"share_url": share_url, "code": code}
        if parent_file_id:
            params["parent_file_id"] = parent_file_id
        if flat:
            params["flat"] = flat
        if page:
            params["page"] = page
        if page_size:
            params["page_size"] = page_size
        return self._request_json(url, params=params, timeout=10)

    def resolve_play(self, share_url, code="", file_id="", file_name=""):
        """获取视频播放直链和字幕"""
        url = f"{self.daemon_url}/api/play"
        params = {"share_url": share_url, "code": code, "file_id": file_id, "file_name": file_name}
        return self._request_json(url, params=params, timeout=30)

    def get_qrcode(self, provider):
        """开始扫码登录，获取二维码"""
        url = f"{self.daemon_url}/api/qrcode"
        params = {"provider": provider}
        return self._request_json(url, params=params, timeout=10)

    def get_manual_login_info(self, provider):
        """获取手工输入登录所需的网页URL与二维码"""
        url = f"{self.daemon_url}/api/manual_input/url"
        params = {"provider": provider}
        return self._request_json(url, params=params, timeout=10)

    def check_qrcode_status(self, provider, key):
        """检查扫码状态"""
        url = f"{self.daemon_url}/api/qrcode/status"
        params = {"provider": provider, "key": key}
        try:
            return self._request_json(url, params=params, timeout=10)
        except Exception:
            return {"status": "pending"}

    def get_pan_status(self):
        """获取所有网盘的登录状态与并发数"""
        url = f"{self.daemon_url}/api/pan/status"
        try:
            return self._request_json(url, timeout=10)
        except Exception:
            return {}

    def logout_pan(self, provider):
        """退出某网盘登录"""
        url = f"{self.daemon_url}/api/pan/logout"
        try:
            return self._request_json(url, params={"provider": provider}, timeout=10)
        except Exception:
            return {"error": "request failed"}

    def set_pan_concurrency(self, provider, value):
        """设置某网盘的并发数"""
        url = f"{self.daemon_url}/api/pan/concurrency"
        try:
            return self._request_json(url, params={"provider": provider, "value": value}, timeout=10)
        except Exception:
            return {"error": "request failed"}

    def set_pan_chunk_size(self, provider, value):
        """设置某网盘的分片大小（字节数）"""
        url = f"{self.daemon_url}/api/pan/chunk_size"
        try:
            return self._request_json(url, params={"provider": provider, "value": value}, timeout=10)
        except Exception:
            return {"error": "request failed"}

    def get_pan_order(self):
        """获取网盘排序规则"""
        url = f"{self.daemon_url}/api/pan/order"
        try:
            res = self._request_json(url, timeout=5)
            if isinstance(res, dict) and res.get("success") and isinstance(res.get("order"), list):
                return res.get("order")
        except Exception:
            pass
        return ["7", "4", "2", "3", "11", "6", "9", "8", "5", "1", "10"]

    def get_push_url_info(self):
        """获取网页推送所需的网页URL与二维码"""
        url = f"{self.daemon_url}/api/push/url"
        return self._request_json(url, timeout=10)

    def poll_pushed_link(self):
        """轮询是否有新的网页推送播放请求"""
        url = f"{self.daemon_url}/api/push/poll"
        try:
            return self._request_json(url, timeout=3)
        except Exception:
            return {"has_new": False}

    def _post_json(self, url, body_data, timeout=10):
        try:
            data = json.dumps(body_data).encode('utf-8')
            req = urllib.request.Request(
                url, 
                data=data, 
                headers={'Content-Type': 'application/json'}
            )
            with urllib.request.urlopen(req, timeout=timeout) as response:
                res_data = response.read().decode('utf-8')
                return json.loads(res_data)
        except Exception as e:
            err_msg = str(e)
            if isinstance(e, urllib.error.HTTPError):
                try:
                    body = e.read().decode('utf-8')
                    err_data = json.loads(body)
                    if err_data.get("error"):
                        err_msg = err_data["error"]
                except Exception:
                    pass
            try:
                import xbmc
                xbmc.log(f"MvPlugin ApiClient POST error on {url}: {err_msg}", 3) # LOGERROR
            except ImportError:
                print(f"MvPlugin ApiClient POST error on {url}: {err_msg}")
            raise Exception(err_msg)

    def mark_pan_link_invalid(self, link_id, reason):
        """上报失效的网盘分享链接"""
        url = f"{self.base_url}/videos/pan-links/{int(link_id)}/invalid"
        body = {"invalidReason": reason}
        try:
            return self._post_json(url, body, timeout=10)
        except Exception:
            pass
        return None

    def get_pansou_config(self):
        """获取盘搜设置"""
        url = f"{self.daemon_url}/api/pansou/config"
        try:
            return self._request_json(url, timeout=10)
        except Exception:
            return {}

    def save_pansou_config(self, addr, username, password, pan_types):
        """保存盘搜设置"""
        url = f"{self.daemon_url}/api/pansou/config"
        body = {
            "addr": addr,
            "username": username,
            "password": password,
            "pan_types": pan_types
        }
        return self._post_json(url, body, timeout=10)

    def search_pansou(self, kw):
        """发起盘搜搜索"""
        url = f"{self.daemon_url}/api/pansou/search"
        return self._request_json(url, params={"kw": kw}, timeout=20)

    def get_user_files(self, provider, parent_file_id="", token=None):
        """获取个人网盘某文件夹下的文件与子目录列表"""
        url = f"{self.daemon_url}/api/user_files"
        params = {
            "provider": provider,
            "parent_file_id": parent_file_id,
        }
        if token:
            params["token"] = token
        return self._request_json(url, params=params, timeout=15)

    def resolve_user_play(self, provider, file_id, file_name="", token=None):
        """获取个人网盘视频文件的播放直链/本地代理包装地址"""
        url = f"{self.daemon_url}/api/user_play"
        params = {
            "provider": provider,
            "file_id": file_id,
            "file_name": file_name,
        }
        if token:
            params["token"] = token
        return self._request_json(url, params=params, timeout=15)

    def get_shares(self, page=1, page_size=20, keyword=None):
        """获取分享广场列表"""
        url = f"{self.base_url}/shares"
        params = {"page": page, "pageSize": page_size}
        if keyword:
            params["keyword"] = keyword
        try:
            res = self._request_json(url, params=params, timeout=10)
            if res.get("code") == "SUCCESS":
                return res.get("data") or {}
        except Exception:
            pass
        return {"list": [], "total": 0, "page": page, "pageSize": page_size}

    def get_share_favorites(self, page=1, page_size=20):
        """获取当前设备的收藏分享列表"""
        url = f"{self.base_url}/shares/favorites"
        params = {"page": page, "pageSize": page_size}
        try:
            res = self._request_json(url, params=params, timeout=10)
            if res.get("code") == "SUCCESS":
                return res.get("data") or {}
        except Exception:
            pass
        return {"list": [], "total": 0, "page": page, "pageSize": page_size}

    def add_share(self, share_url, title, sharer="匿名", pwd=None):
        """新增分享"""
        url = f"{self.base_url}/shares"
        body = {
            "url": share_url,
            "title": title,
            "sharer": sharer,
            "pwd": pwd
        }
        return self._post_json(url, body, timeout=10)

    def favorite_share(self, share_id):
        """收藏分享"""
        url = f"{self.base_url}/shares/{int(share_id)}/favorite"
        return self._post_json(url, {}, timeout=10)

    def unfavorite_share(self, share_id):
        """取消收藏分享"""
        url = f"{self.base_url}/shares/{int(share_id)}/unfavorite"
        return self._post_json(url, {}, timeout=10)



