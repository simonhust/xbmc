# -*- coding: utf-8 -*-
import os
import urllib.parse

class VfsAdapter:
    """
    负责将 VFS 目录项目交付给 Kodi UI。
    通过动态 import 规避单元测试环境缺少 xbmc 模块的问题。
    """
    def __init__(self, handle, base_url):
        self.handle = handle
        self.base_url = base_url
        try:
            import xbmc
            import xbmcgui
            import xbmcplugin
            import xbmcvfs
            self.xbmc = xbmc
            self.xbmcgui = xbmcgui
            self.xbmcplugin = xbmcplugin
            self.xbmcvfs = xbmcvfs
        except ImportError:
            self.xbmc = None
            self.xbmcgui = None
            self.xbmcplugin = None
            self.xbmcvfs = None

    def add_directory_item(self, label, params, is_folder=True, icon=None, fanart=None, info=None, is_playable=False, pic=None):
        url = f"{self.base_url}?{urllib.parse.urlencode(params)}"
        if self.xbmcgui and self.xbmcplugin:
            list_item = self.xbmcgui.ListItem(label, "", url)
            art = {}
            target_pic = pic or icon
            if target_pic:
                art.update({
                    'icon': target_pic, 
                    'thumb': target_pic, 
                    'poster': target_pic, 
                    'landscape': target_pic,
                    'banner': target_pic
                })
            if fanart:
                art['fanart'] = fanart

            if art:
                list_item.setArt(art)
            
            # 为可播放项目自动注入简易 info，确保 mediatype="video" 强制唤起内置播放器
            if is_playable and not info:
                info = {"title": label, "mediatype": "video"}
                
            if info:
                list_item.setInfo('video', info)
                
            if is_playable:
                list_item.setProperty('IsPlayable', 'true')
                # 显式指定 path 为插件调用 url，解决部分 Kodi 19/20 底层在双击时因 path 为空忽略执行的问题
                list_item.setPath(url)
                
            self.xbmcplugin.addDirectoryItem(self.handle, url, list_item, isFolder=is_folder)
        else:
            # 单元测试环境占位，供 Mock 断言使用
            pass

    def set_content(self, content_type):
        if self.xbmcplugin:
            self.xbmcplugin.setContent(self.handle, content_type)

    def set_resolved_url(self, succeeded, list_item):
        if self.xbmcplugin:
            if self.xbmc:
                self.xbmc.log(f"=== SET RESOLVED URL CALLED: succeeded={succeeded}, path={list_item.getPath()} ===", self.xbmc.LOGINFO)
            self.xbmcplugin.setResolvedUrl(self.handle, succeeded, list_item)

    def end_of_directory(self):
        if self.xbmcplugin:
            self.xbmcplugin.endOfDirectory(self.handle)

    def show_select_dialog(self, heading, choices, default_idx=0) -> int:
        if self.xbmcgui:
            dialog = self.xbmcgui.Dialog()
            return dialog.select(heading, choices, preselect=default_idx)
        return -1

    def show_numeric_dialog(self, heading, default_val="") -> str:
        if self.xbmcgui:
            dialog = self.xbmcgui.Dialog()
            return dialog.numeric(0, heading, default_val)
        return ""

    def update_container(self, params, replace=False):
        url = f"{self.base_url}?{urllib.parse.urlencode(params)}"
        if self.xbmc:
            cmd = f"Container.Update({url}, replace)" if replace else f"Container.Update({url})"
            self.xbmc.executebuiltin(cmd)

    def refresh_container(self):
        if self.xbmc:
            self.xbmc.executebuiltin("Container.Refresh")

    def get_profile_path(self) -> str:
        if self.xbmcvfs:
            profile_path = self.xbmcvfs.translatePath("special://profile/addon_data/plugin.video.juku/")
            if not os.path.exists(profile_path):
                try:
                    os.makedirs(profile_path)
                except Exception:
                    pass
            return profile_path
        else:
            import tempfile
            temp_dir = os.path.join(tempfile.gettempdir(), 'plugin.video.juku')
            if not os.path.exists(temp_dir):
                os.makedirs(temp_dir)
            return temp_dir

    def log(self, msg, level="info"):
        if self.xbmc:
            log_level = self.xbmc.LOGINFO
            if level == "warning":
                log_level = self.xbmc.LOGWARNING
            elif level == "error":
                log_level = self.xbmc.LOGERROR
            self.xbmc.log(f"[VfsRouter] {msg}", log_level)
        else:
            print(f"[{level.upper()}] [VfsRouter] {msg}")

    def show_keyboard_input(self, default_val="", heading="", hidden=False) -> str:
        if self.xbmc:
            import xbmc
            keyboard = xbmc.Keyboard(default_val, heading, hidden)
            keyboard.doModal()
            if keyboard.isConfirmed():
                return keyboard.getText()
            return None
        return ""

    def show_multiselect_dialog(self, heading, choices, preselect=None) -> list:
        if self.xbmcgui:
            dialog = self.xbmcgui.Dialog()
            return dialog.multiselect(heading, choices, preselect=preselect)
        return []

    def show_yes_no_dialog(self, heading, message) -> bool:
        if self.xbmcgui:
            dialog = self.xbmcgui.Dialog()
            return dialog.yesno(heading, message)
        return True

    def show_notification(self, heading, message, is_error=False):
        if self.xbmcgui:
            dialog = self.xbmcgui.Dialog()
            icon = "error" if is_error else "info"
            dialog.notification(heading, message, icon, 3000)
        else:
            print(f"[Notification] {heading}: {message}")
