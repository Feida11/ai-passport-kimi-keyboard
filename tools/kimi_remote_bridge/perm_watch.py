"""Permission-dialog detection and click-through for Kimi Code windows.

The production backend adapts the ``uiautomation`` tree; tests inject a
``FakeBackend`` with a fixed control tree.  All detection logic works on the
plain :class:`Control` tree so it is fully testable without a desktop.

Heuristic (patterns are configurable regexes):

* A dialog is a container (Button/Control subtree, any depth) holding at
  least one button whose name matches the allow pattern and/or the deny
  pattern.
* Options are the matching buttons enumerated in document order.
* The dialog text is the container name plus the text of its non-button
  text descendants.
"""

from __future__ import annotations

import abc
import logging
import re
import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Pattern, Tuple

DEFAULT_ALLOW_PATTERN = r"允许|批准|Allow|Approve|Yes|是"
DEFAULT_DENY_PATTERN = r"拒绝|取消|Deny|Reject|No|否"

ROLE_ALLOW = "allow"
ROLE_DENY = "deny"

# Path from a tree root to a control: child indexes at each level.
Path = Tuple[int, ...]

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Plain control tree (backend-independent)
# ---------------------------------------------------------------------------

@dataclass
class Control:
    name: str = ""
    control_type: str = "Control"   # "Window", "Pane", "Button", "Text", ...
    children: List["Control"] = field(default_factory=list)

    def walk(self) -> "Tuple[Path, Control]":
        """Yield ``(path, control)`` in document order (depth-first)."""
        yield (), self
        yield from self._walk_children(())

    def _walk_children(self, base: Path) -> "Tuple[Path, Control]":
        for i, child in enumerate(self.children):
            path = base + (i,)
            yield path, child
            yield from child._walk_children(path)

    def resolve(self, path: Path) -> "Control":
        node = self
        for i in path:
            node = node.children[i]
        return node

    def find(self, pred: Callable[["Control"], bool]) -> List["Control"]:
        return [c for _, c in self.walk() if pred(c)]


@dataclass
class DialogOption:
    label: str
    role: str                       # ROLE_ALLOW / ROLE_DENY
    path: Path = ()                 # path of the button within the tree
    native: object = None           # 原生 UIA 元素(服务端查找命中时直接携带)


@dataclass
class PermDialog:
    text: str
    options: List[DialogOption]
    path: Path                      # path of the dialog container in the tree


# ---------------------------------------------------------------------------
# Detection logic (pure — testable without any backend)
# ---------------------------------------------------------------------------

def compile_patterns(allow_pattern: str = DEFAULT_ALLOW_PATTERN,
                     deny_pattern: str = DEFAULT_DENY_PATTERN
                     ) -> Tuple[Pattern[str], Pattern[str]]:
    return (re.compile(allow_pattern, re.IGNORECASE),
            re.compile(deny_pattern, re.IGNORECASE))


def match_role(name: str, allow_re: Pattern[str],
               deny_re: Pattern[str]) -> Optional[str]:
    if allow_re.search(name):
        return ROLE_ALLOW
    if deny_re.search(name):
        return ROLE_DENY
    return None


def _subtree_size(node: Control) -> int:
    return sum(1 for _ in node.walk())


def find_dialog(root: Control,
                allow_re: Pattern[str],
                deny_re: Pattern[str]) -> Optional[PermDialog]:
    """Return the smallest qualifying dialog container, or ``None``.

    真实弹窗(Kimi Code 为 Electron 应用)深嵌在 WebView 的无障碍树里,
    外层容器(如整个窗口)也包含允许/拒绝按钮,因此选取"同时含允许与拒绝
    按钮的最小子树"作为弹窗,避免把整窗文本当请求内容。
    """
    best: Optional[Tuple[int, Path, Control, List[Tuple[Path, Control, str]]]] = None
    for path, node in root.walk():
        if not path:
            continue  # the root window itself is never the dialog
        buttons: List[Tuple[Path, Control, str]] = []
        for bpath, btn in node.walk():
            if btn.control_type != "Button":
                continue
            role = match_role(btn.name, allow_re, deny_re)
            if role is not None:
                buttons.append((path + bpath, btn, role))
        if not buttons:
            continue
        roles = {r for _, _, r in buttons}
        if ROLE_ALLOW not in roles or ROLE_DENY not in roles:
            continue  # 必须同时存在允许与拒绝,才是权限弹窗
        size = _subtree_size(node)
        if best is None or size < best[0]:
            best = (size, path, node, buttons)
    if best is None:
        return None

    _, dpath, container, buttons = best
    # 文本:取自弹窗容器;若容器本身无文本(按钮组常见),向上回溯祖先补取
    texts: List[str] = []
    seen = set()
    for k in range(len(dpath), 0, -1):
        node = root.resolve(dpath[:k])
        candidates = [node.name] if node.name else []
        for _, sub in node.walk():
            if sub.control_type in ("Text", "Edit") and sub.name:
                candidates.append(sub.name)
        added = False
        for t in candidates:
            if t not in seen:
                seen.add(t)
                texts.append(t)
                added = True
        if added and k < len(dpath):
            break  # 已向祖先补到文本,停止上溯
    # 选项排序:允许类在前、拒绝类次之、其他最后(默认选中第一项=允许)
    rank = {ROLE_ALLOW: 0, ROLE_DENY: 1}
    buttons.sort(key=lambda b: rank.get(b[2], 2))
    options = [DialogOption(label=btn.name, role=role, path=bpath)
               for bpath, btn, role in buttons]
    return PermDialog(text=" ".join(texts), options=options, path=dpath)


def click_choice(backend: "DialogBackend", dialog: PermDialog,
                 choice: int) -> None:
    """Act on a dialog for the given device choice.

    ``choice`` is the option index in document order; ``-1`` (cancel/timeout)
    clicks the deny button if there is one, otherwise presses Esc.
    """
    if choice < 0:
        for opt in dialog.options:
            if opt.role == ROLE_DENY:
                backend.click_path(opt.path)
                return
        backend.press_esc()
        return
    if choice >= len(dialog.options):
        raise IndexError(f"choice {choice} out of range "
                         f"({len(dialog.options)} options)")
    opt = dialog.options[choice]
    if opt.native is not None and backend.click_native(opt.native):
        return
    backend.click_path(opt.path)


# ---------------------------------------------------------------------------
# Backends
# ---------------------------------------------------------------------------

class DialogBackend(abc.ABC):
    """Abstract UI backend used by :class:`PermWatcher`."""

    @abc.abstractmethod
    def snapshot(self) -> Control:
        """Return the current UI tree rooted at the main window."""

    @abc.abstractmethod
    def click_path(self, path: Path) -> None:
        """Click the button at ``path`` in the last snapshot tree."""

    @abc.abstractmethod
    def press_esc(self) -> None:
        """Send an Esc keypress to the foreground window."""

    def click_native(self, native: object) -> bool:
        """点击服务端查找直接命中的原生元素;不支持返回 False。"""
        return False


class FakeBackend(DialogBackend):
    """Test backend with a fixed control tree; clicks are recorded."""

    def __init__(self, tree: Control) -> None:
        self.tree = tree
        self.clicks: List[Path] = []
        self.esc_pressed = 0

    def set_tree(self, tree: Control) -> None:
        self.tree = tree

    def snapshot(self) -> Control:
        return self.tree

    def click_path(self, path: Path) -> None:
        node = self.tree.resolve(path)
        assert node.control_type == "Button", f"not a button: {node}"
        self.clicks.append(path)

    def press_esc(self) -> None:
        self.esc_pressed += 1


APPROVAL_CLASS = "appr dock-approval"


class UiaBackend(DialogBackend):
    """Production backend built on the ``uiautomation`` library."""

    def __init__(self, window_title_regex: str,
                 search_timeout: float = 2.0) -> None:
        self._pattern = re.compile(window_title_regex)
        self._timeout = search_timeout
        self._last_tree: Optional[Control] = None
        self._uia = None

    def _automation(self):
        if self._uia is None:
            import comtypes.client
            core = comtypes.client.GetModule('UIAutomationCore.dll')
            self._uia = comtypes.client.CreateObject(
                '{ff48dba4-60ef-4201-aa87-54103eef594e}',
                interface=core.IUIAutomation)
        return self._uia

    def find_approval_element(self):
        """服务端按 ClassName 直接定位审批容器(毫秒级);未找到返回 None。"""
        win = self._find_main_window()
        if win is None:
            return None
        uia = self._automation()
        cond = uia.CreatePropertyCondition(30012, APPROVAL_CLASS)
        try:
            return win.Element.FindFirst(5, cond)  # TreeScope_Descendants
        except Exception:
            return None

    def click_native(self, native: object) -> bool:
        try:
            invoke = native.GetCurrentPattern(10000)  # InvokePattern
            invoke.Invoke()
            return True
        except Exception:
            return False

    def _find_main_window(self):
        import uiautomation as auto
        root = auto.GetRootControl()
        fallback = None
        for win in root.GetChildren():
            try:
                name = win.Name or ""
                cls = win.ClassName or ""
            except Exception:
                continue
            if not self._pattern.search(name):
                continue
            # 精确标题优先,避免匹配到 "KimiCU Overlay" 这类同名前缀覆盖层
            if name == "Kimi Code" or "Chrome_WidgetWin" in cls:
                return win
            if fallback is None:
                fallback = win
        return fallback

    def snapshot(self) -> Control:
        import uiautomation as auto
        win = self._find_main_window()
        if win is None:
            raise LookupError("Kimi Code main window not found")

        def convert(ctrl, depth: int = 0) -> Control:
            try:
                name = ctrl.Name or ""
            except Exception:
                name = ""
            try:
                ctype = ctrl.ControlTypeName or "Control"
            except Exception:
                ctype = "Control"
            node = Control(name=name, control_type=ctype)
            try:
                children = ctrl.GetChildren()
            except Exception:
                children = []
            # 聊天历史列表可能含数千条目,是抓树耗时主因;权限弹窗不在 List 子树,
            # 对大列表只取前 5 个孩子守住结构,其余剪掉。
            if ctype == "List" and len(children) > 20:
                children = children[:5]
            if depth < 40:  # Electron 应用的弹窗深嵌在 WebView 树中(实测约 18-20 层)
                for ch in children:
                    node.children.append(convert(ch, depth + 1))
            return node

        self._last_tree = convert(win)
        return self._last_tree

    def click_path(self, path: Path) -> None:
        import uiautomation as auto
        win = self._find_main_window()
        if win is None:
            raise LookupError("Kimi Code main window not found")
        node = win
        for i in path:
            node = node.GetChildren()[i]
        if not node.IsEnabled():
            raise RuntimeError("button is not enabled")
        node.Click(simulateMove=False)

    def press_esc(self) -> None:
        import uiautomation as auto
        auto.SendKeys("{Esc}")


# ---------------------------------------------------------------------------
# Polling watcher
# ---------------------------------------------------------------------------

class PermWatcher:
    """Polls the backend for permission dialogs.

    Only one permission request may be in flight at a time: while
    :attr:`in_flight` is set, new dialogs are skipped (the device keeps its
    own prompt pending).
    """

    def __init__(self, backend: DialogBackend,
                 allow_pattern: str = DEFAULT_ALLOW_PATTERN,
                 deny_pattern: str = DEFAULT_DENY_PATTERN,
                 poll_interval: float = 0.6) -> None:
        self.backend = backend
        self.allow_re, self.deny_re = compile_patterns(allow_pattern,
                                                       deny_pattern)
        self.poll_interval = poll_interval
        self.in_flight = False
        self._running = False

    def poll_once(self) -> Optional[PermDialog]:
        """Take one snapshot and detect a dialog (``None`` on any failure)."""
        try:
            tree = self.backend.snapshot()
        except Exception as exc:
            log.warning("perm watcher snapshot failed: %s", exc)
            return None
        try:
            return find_dialog(tree, self.allow_re, self.deny_re)
        except Exception as exc:  # pragma: no cover - defensive
            log.warning("perm dialog parse failed: %s", exc)
            return None

    def stop(self) -> None:
        self._running = False

    def watch(self, on_dialog: Callable[[PermDialog], None],
              sleep: Callable[[float], None] = time.sleep) -> None:
        """Blocking poll loop; call from a worker thread or executor."""
        self._running = True
        while self._running:
            dialog = self.poll_once()
            if dialog is not None and not self.in_flight:
                on_dialog(dialog)
            sleep(self.poll_interval)
