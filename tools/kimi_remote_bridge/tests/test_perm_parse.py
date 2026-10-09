"""Permission-dialog parsing tests with a FakeBackend-injected control tree."""

import pytest

from perm_watch import (
    ROLE_ALLOW,
    ROLE_DENY,
    Control,
    FakeBackend,
    PermWatcher,
    click_choice,
    compile_patterns,
    find_dialog,
)


def make_dialog_tree():
    """A Kimi Code main window containing a permission dialog."""
    return Control("Kimi Code", "Window", [
        Control("编辑器区", "Pane", [
            Control("SomeClass.py - Kimi Code", "Edit", []),
        ]),
        Control("权限确认", "Pane", [
            Control("允许 Kimi 执行终端命令吗？", "Text", []),
            Control("", "Pane", [
                Control("总是允许", "Button", []),
                Control("允许一次", "Button", []),
                Control("拒绝", "Button", []),
            ]),
        ]),
    ])


@pytest.fixture()
def patterns():
    return compile_patterns()


def test_detect_dialog(patterns):
    tree = make_dialog_tree()
    dialog = find_dialog(tree, *patterns)
    assert dialog is not None
    assert "权限确认" in dialog.text
    assert "允许 Kimi 执行终端命令吗？" in dialog.text
    # Document order, roles assigned by pattern.
    assert [o.label for o in dialog.options] == ["总是允许", "允许一次", "拒绝"]
    assert [o.role for o in dialog.options] == [ROLE_ALLOW, ROLE_ALLOW, ROLE_DENY]
    # 新语义:选中最小的"按钮组"容器,其父级即弹窗面板
    container = tree.resolve(dialog.path)
    labels = [c.name for _, c in container.walk() if c.control_type == "Button"]
    assert set(labels) == {"总是允许", "允许一次", "拒绝"}
    parent = tree.resolve(dialog.path[:-1])
    assert parent.name == "权限确认"


def test_no_dialog_when_no_buttons(patterns):
    tree = Control("Kimi Code", "Window", [
        Control("普通面板", "Pane", [Control("hello", "Text", [])]),
    ])
    assert find_dialog(tree, *patterns) is None


def test_no_dialog_for_unrelated_buttons(patterns):
    tree = Control("Kimi Code", "Window", [
        Control("工具栏", "Pane", [
            Control("保存", "Button", []),
            Control("刷新", "Button", []),
        ]),
    ])
    assert find_dialog(tree, *patterns) is None


def test_english_buttons(patterns):
    tree = Control("Kimi Code", "Window", [
        Control("Confirm", "Pane", [
            Control("Run this command?", "Text", []),
            Control("Allow", "Button", []),
            Control("Deny", "Button", []),
        ]),
    ])
    dialog = find_dialog(tree, *patterns)
    assert [o.role for o in dialog.options] == [ROLE_ALLOW, ROLE_DENY]


def test_click_choice_by_index(patterns):
    backend = FakeBackend(make_dialog_tree())
    dialog = find_dialog(backend.snapshot(), *patterns)
    click_choice(backend, dialog, 1)      # 允许一次
    assert backend.clicks == [dialog.options[1].path]
    assert backend.esc_pressed == 0


def test_click_choice_minus_one_clicks_deny(patterns):
    backend = FakeBackend(make_dialog_tree())
    dialog = find_dialog(backend.snapshot(), *patterns)
    click_choice(backend, dialog, -1)
    deny = [o for o in dialog.options if o.role == ROLE_DENY][0]
    assert backend.clicks == [deny.path]
    assert backend.esc_pressed == 0


def test_no_dialog_without_deny_role(patterns):
    # 新语义:必须同时含允许与拒绝按钮才算权限弹窗,避免误报普通确认框
    tree = Control("Kimi Code", "Window", [
        Control("确认", "Pane", [
            Control("继续吗？", "Text", []),
            Control("是", "Button", []),
        ]),
    ])
    backend = FakeBackend(tree)
    assert find_dialog(backend.snapshot(), *patterns) is None


def test_click_choice_minus_one_without_deny_presses_esc(patterns):
    # 含双角色但拒绝按钮失效时按 Esc(防御路径)
    tree = Control("Kimi Code", "Window", [
        Control("确认", "Pane", [
            Control("继续吗？", "Text", []),
            Control("否", "Button", []),
            Control("是", "Button", []),
        ]),
    ])
    backend = FakeBackend(tree)
    dialog = find_dialog(backend.snapshot(), *patterns)
    assert dialog is not None
    # 人为移除 deny 选项,模拟点不到拒绝的场景
    dialog.options = [o for o in dialog.options if o.role != ROLE_DENY]
    click_choice(backend, dialog, -1)
    assert backend.clicks == []
    assert backend.esc_pressed == 1


def test_click_choice_out_of_range(patterns):
    backend = FakeBackend(make_dialog_tree())
    dialog = find_dialog(backend.snapshot(), *patterns)
    with pytest.raises(IndexError):
        click_choice(backend, dialog, 99)


def test_watcher_skips_dialog_while_in_flight(patterns):
    backend = FakeBackend(make_dialog_tree())
    watcher = PermWatcher(backend, poll_interval=0.01)
    seen = []

    def on_dialog(d):
        seen.append(d)
        watcher.stop()

    # While a request is in flight the dialog must be skipped; the loop is
    # stopped externally after 3 polls.
    polls = [0]

    def sleepy(_):
        polls[0] += 1
        if polls[0] >= 3:
            watcher.stop()

    watcher.in_flight = True  # a request is already pending
    watcher.watch(on_dialog, sleep=sleepy)
    assert seen == []
    assert polls[0] == 3

    # With nothing in flight the first poll fires on_dialog, which stops the
    # loop, so the dialog is delivered exactly once.
    watcher.in_flight = False
    watcher.watch(on_dialog, sleep=sleepy)
    assert len(seen) == 1
