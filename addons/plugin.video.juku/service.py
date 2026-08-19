# -*- coding: utf-8 -*-
import sys
import os
import time
import urllib.parse
import xbmc

# 将 resources/lib 追加至运行期路径
project_root = os.path.dirname(os.path.abspath(__file__))
lib_path = os.path.join(project_root, 'resources', 'lib')
if lib_path not in sys.path:
    sys.path.insert(0, lib_path)

from daemon import ensure_daemon_started, stop_daemon
from api_client import ApiClient
from router import PushHistoryManager

class MvServiceMonitor(xbmc.Monitor):
    def __init__(self):
        super(MvServiceMonitor, self).__init__()

    def onAbortRequested(self):
        xbmc.log("MvPlugin Service: Abort requested, stopping daemon", xbmc.LOGINFO)
        stop_daemon()

def main():
    xbmc.log("MvPlugin Service: Starting background daemon", xbmc.LOGINFO)
    ensure_daemon_started()

    # 诊断本地联通性
    xbmc.log("MvPlugin Service: Diagnosing connection to 127.0.0.1:17654", xbmc.LOGINFO)
    import urllib.request
    import json
    try:
        req = urllib.request.Request("http://127.0.0.1:17654/api/pan/status")
        with urllib.request.urlopen(req, timeout=5) as response:
            data = response.read().decode('utf-8')
            res = json.loads(data)
            xbmc.log(f"MvPlugin Service Connection Test (status): SUCCESS, response: {res}", xbmc.LOGINFO)
            
        # 测试设备注册与主站 API 转发
        xbmc.log("MvPlugin Service: Testing API forwarding to getHomeData", xbmc.LOGINFO)
        req_api = urllib.request.Request("http://127.0.0.1:17654/api/mv/videos/getHomeData")
        with urllib.request.urlopen(req_api, timeout=10) as response_api:
            data_api = response_api.read().decode('utf-8')
            res_api = json.loads(data_api)
            xbmc.log(f"MvPlugin Service Connection Test (api): SUCCESS, response: {res_api}", xbmc.LOGINFO)
    except Exception as e:
        xbmc.log(f"MvPlugin Service Connection Test: FAILED: {e}", xbmc.LOGERROR)

    # 初始化 API 客户端和推送历史记录管理器
    api_client = ApiClient()
    
    import xbmcaddon
    import xbmcvfs
    addon = xbmcaddon.Addon()
    profile_dir = xbmcvfs.translatePath(addon.getAddonInfo('profile'))
    push_mgr = PushHistoryManager(profile_dir)

    monitor = MvServiceMonitor()
    # 循环等待退出信号与网页推送轮询
    while not monitor.abortRequested():
        # 自愈重启机制：主动检测 daemon 端口 17654，如果未运行则重新拉起
        from daemon import is_port_in_use
        if not is_port_in_use(17654):
            xbmc.log("MvPlugin Service: Detect daemon is down (port 17654 not in use). Restarting...", xbmc.LOGWARNING)
            try:
                ensure_daemon_started()
            except Exception as ree:
                xbmc.log(f"MvPlugin Service self-healing failed to restart daemon: {ree}", xbmc.LOGWARNING)

        # 轮询 Go 后台是否有新的推送链接
        try:
            res = api_client.poll_pushed_link()
            if res and res.get("has_new"):
                item = res.get("item")
                cmd = item.get("cmd")
                
                if cmd:
                    xbmc.log(f"MvPlugin Service: Pushed command detected! cmd={cmd}", xbmc.LOGINFO)
                    xbmc.executebuiltin(cmd)
                else:
                    url = item.get("url")
                    code = item.get("code") or ""
                    title = item.get("title") or ""
                    pan_name = item.get("pan_name") or ""
                    pic = item.get("pic") or item.get("coverUrl") or ""
                    fanart = item.get("backdropPath") or item.get("backdrop_path") or item.get("fanart") or ""
                    vod_id = item.get("vod_id") or item.get("id") or ""

                    xbmc.log(f"MvPlugin Service: Pushed link detected! url={url}, title={title}, pic={pic}, fanart={fanart}, vod_id={vod_id}", xbmc.LOGINFO)
                    
                    # 写入推送历史记录
                    record = {
                        "url": url,
                        "code": code,
                        "title": title,
                        "pan_name": pan_name,
                        "timestamp": int(time.time())
                    }
                    push_mgr.add_record(record)

                    # 在后台获取文件列表
                    files = []
                    try:
                        files_data = api_client.list_pan_files(url, code)
                        files = files_data.get("files") or []
                    except Exception as list_err:
                        xbmc.log(f"MvPlugin Service: failed to list files in background: {list_err}", xbmc.LOGWARNING)

                    # 如果只有一个视频文件，则使用 PlayMedia 播放，否则使用 ActivateWindow 打开文件目录列表
                    if len(files) == 1:
                        f = files[0]
                        # 拼装 pan_play 插件链接直接播放
                        cmd_to_run = 'PlayMedia("plugin://plugin.video.juku/?action=pan_play&url={}&code={}&file_id={}&pan_name={}&file_name={}&vod_title={}&pic={}&fanart={}&vod_id={}&from_share=1")'.format(
                            urllib.parse.quote(url),
                            urllib.parse.quote(code),
                            urllib.parse.quote(f.get("file_id") or ""),
                            urllib.parse.quote(pan_name),
                            urllib.parse.quote(f.get("file_name") or ""),
                            urllib.parse.quote(title or f.get("file_name") or ""),
                            urllib.parse.quote(pic),
                            urllib.parse.quote(fanart),
                            urllib.parse.quote(str(vod_id))
                        )
                    else:
                        # 拼装 list_files 目录展示页面
                        cmd_to_run = 'ActivateWindow(Videos, "plugin://plugin.video.juku/?action=list_files&url={}&code={}&pan_name={}&vod_title={}&pic={}&fanart={}&vod_id={}")'.format(
                            urllib.parse.quote(url),
                            urllib.parse.quote(code),
                            urllib.parse.quote(pan_name),
                            urllib.parse.quote(title),
                            urllib.parse.quote(pic),
                            urllib.parse.quote(fanart),
                            urllib.parse.quote(str(vod_id))
                        )
                    
                    xbmc.log(f"MvPlugin Service: Triggering push playback cmd: {cmd_to_run}", xbmc.LOGINFO)
                    xbmc.executebuiltin(cmd_to_run)
        except Exception as e:
            xbmc.log(f"MvPlugin Service polling error: {str(e)}", xbmc.LOGDEBUG if hasattr(xbmc, 'LOGDEBUG') else xbmc.LOGINFO)
            # 双重保险：如果异常没有被吞，也在 except 中拉起
            try:
                ensure_daemon_started()
            except Exception as ree:
                xbmc.log(f"MvPlugin Service self-healing failed to restart daemon in except block: {ree}", xbmc.LOGWARNING)

        if monitor.waitForAbort(2):
            break

    xbmc.log("MvPlugin Service: Exiting service loop", xbmc.LOGINFO)
    stop_daemon()

if __name__ == '__main__':
    main()
