"""Inject text into the Kimi Code window via clipboard paste.

Finds the Kimi Code top-level window by title regex, brings it to the
foreground, pastes the text through the clipboard, and optionally presses
Enter.  pywin32 is imported lazily so the module stays importable off-Windows.
"""

from __future__ import annotations

import logging
import re
import time
from typing import Optional

log = logging.getLogger(__name__)


class WindowNotFoundError(RuntimeError):
    """Raised when no window matches the configured title regex."""


class KimiInjector:
    def __init__(self, title_regex: str = r"Kimi",
                 auto_enter: bool = True,
                 retries: int = 5,
                 retry_interval: float = 0.5,
                 settle_delay: float = 0.15,
                 universal: bool = False) -> None:
        self._pattern = re.compile(title_regex)
        self.auto_enter = auto_enter
        self.retries = retries
        self.retry_interval = retry_interval
        self.settle_delay = settle_delay
        # universal 模式:不找特定窗口,直接粘贴到当前焦点窗口(任意应用通用)
        self.universal = universal

    # ------------------------------------------------------------------
    def _find_hwnd(self):
        import win32api
        import win32gui
        import win32process
        matches = []

        def cb(hwnd, _):
            if not win32gui.IsWindowVisible(hwnd):
                return
            title = win32gui.GetWindowText(hwnd) or ""
            if title and self._pattern.search(title):
                matches.append(hwnd)

        win32gui.EnumWindows(cb, None)
        if not matches:
            return None

        def title_of(h):
            return win32gui.GetWindowText(h) or ""

        def exe_of(h):
            try:
                _, pid = win32process.GetWindowThreadProcessId(h)
                hp = win32api.OpenProcess(0x1000, False, pid)
                return win32process.GetModuleFileNameEx(hp, 0).lower()
            except Exception:
                return ""

        if len(matches) > 1:
            log.info("窗口候选:%s",
                     "; ".join(f"[{title_of(h)}]({h})" for h in matches))

        # 1) 精确标题 "Kimi Code" 优先
        for h in matches:
            if title_of(h) == "Kimi Code":
                return h
        # 2) 进程 exe 名匹配(Kimi Code.exe / kimi-code 等)
        for h in matches:
            exe = exe_of(h)
            if "kimi code" in exe or "kimi-code" in exe or "kimicode" in exe:
                return h
        # 3) 排除悬浮层(KimiCU Overlay)与工具窗,取第一个正常窗口
        import win32con
        for h in matches:
            t = title_of(h).lower()
            if "overlay" in t or "kimicu" in t:
                continue
            ex = win32gui.GetWindowLong(h, win32con.GWL_EXSTYLE)
            if ex & win32con.WS_EX_TOOLWINDOW:
                continue
            return h
        return matches[0]

    @staticmethod
    def _set_foreground(hwnd) -> None:
        import win32con
        import win32gui
        try:
            if win32gui.GetForegroundWindow() == hwnd:
                return
        except Exception:
            pass
        try:
            win32gui.SetForegroundWindow(hwnd)
            return
        except Exception:
            pass
        # 前台锁:先送一个 Alt 键输入解除系统限制,再激活
        try:
            import win32api
            win32api.keybd_event(win32con.VK_MENU, 0, 0, 0)
            time.sleep(0.02)
            win32api.keybd_event(win32con.VK_MENU, 0,
                                 win32con.KEYEVENTF_KEYUP, 0)
            time.sleep(0.05)
            win32gui.SetForegroundWindow(hwnd)
            return
        except Exception:
            pass
        # 最后一招:AttachThreadInput(全程防抛,失败静默,
        # 由 inject 里的前台校验给出警告)
        try:
            import win32api
            import win32process
            foreground = win32gui.GetForegroundWindow()
            cur_thread = win32api.GetCurrentThreadId()
            fg_thread, _ = win32process.GetWindowThreadProcessId(foreground)
            remote_thread, _ = win32process.GetWindowThreadProcessId(hwnd)
            win32process.AttachThreadInput(cur_thread, fg_thread, True)
            try:
                win32process.AttachThreadInput(cur_thread, remote_thread,
                                               True)
                win32gui.SetForegroundWindow(hwnd)
            finally:
                win32process.AttachThreadInput(cur_thread, remote_thread,
                                               False)
                win32process.AttachThreadInput(cur_thread, fg_thread, False)
        except Exception:
            pass

    @staticmethod
    def _send_key(vk: int) -> None:
        import win32api
        import win32con
        win32api.keybd_event(vk, 0, 0, 0)
        time.sleep(0.02)
        win32api.keybd_event(vk, 0, win32con.KEYEVENTF_KEYUP, 0)

    def _paste_clipboard(self) -> None:
        import win32con
        import win32api
        win32api.keybd_event(win32con.VK_CONTROL, 0, 0, 0)
        time.sleep(0.02)
        self._send_key(0x56)  # 'V'
        time.sleep(0.02)
        win32api.keybd_event(win32con.VK_CONTROL, 0,
                             win32con.KEYEVENTF_KEYUP, 0)

    # ------------------------------------------------------------------
    def inject(self, text: str) -> None:
        """Paste ``text`` (optionally + Enter).

        universal 模式:粘贴到当前前台窗口,不找特定窗口;否则定位
        Kimi Code 窗口并激活后粘贴。
        """
        import pyperclip

        if self.universal:
            import win32gui
            hwnd = win32gui.GetForegroundWindow()
            if hwnd == 0:
                raise WindowNotFoundError("no foreground window")
            title = win32gui.GetWindowText(hwnd) or ""
            pyperclip.copy(text)
            self._paste_clipboard()
            time.sleep(0.05)
            if self.auto_enter:
                import win32con
                self._send_key(win32con.VK_RETURN)
            log.info("已注入前台窗口 [%s](%d 字符,回车=%s)", title,
                     len(text), self.auto_enter)
            return

        hwnd: Optional[int] = None
        for attempt in range(1, self.retries + 1):
            hwnd = self._find_hwnd()
            if hwnd is not None:
                break
            log.warning("未找到 Kimi Code 窗口(第 %d/%d 次)", attempt,
                        self.retries)
            time.sleep(self.retry_interval)
        if hwnd is None:
            raise WindowNotFoundError(
                f"no window matching /{self._pattern.pattern}/ after "
                f"{self.retries} tries")

        import win32gui as _g
        target_title = _g.GetWindowText(hwnd) or ""
        self._set_foreground(hwnd)
        time.sleep(self.settle_delay)
        fg = _g.GetForegroundWindow()
        if fg != hwnd:
            log.warning("激活失败:目标 [%s](hwnd=%d),实际前台 [%s](hwnd=%d),"
                        "文本可能落到错误窗口", target_title, hwnd,
                        _g.GetWindowText(fg) or "-", fg)
        pyperclip.copy(text)
        self._paste_clipboard()
        time.sleep(0.05)
        if self.auto_enter:
            import win32con
            self._send_key(win32con.VK_RETURN)
        log.info("已注入文本到 [%s](%d 字符,回车=%s)", target_title,
                 len(text), self.auto_enter)
