#include "clipboard/controllers/paste_keystroke.h"
#include "core/logger.h"
#include <QTimer>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwchar>

namespace jietuba::clipboard {
namespace {
// About 200 ms in all. A window that still refuses activation (an elevated one, say)
// is not going to accept it; paste into the current focus instead of waiting longer.
constexpr int ACTIVATE_RETRY_MS = 20;
constexpr int ACTIVATE_MAX_ATTEMPTS = 10;
constexpr BYTE MODIFIER_KEYS[] = {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN};
std::function<void(Hwnd)> &test_handler() {
    static std::function<void(Hwnd)> handler;
    return handler;
}

bool foreground_is_own_process() {
    const Hwnd hwnd = get_foreground_hwnd();
    if (!hwnd) return false;
    return get_window_pid(hwnd) == get_current_pid();
}

HWND as_hwnd(Hwnd hwnd) { return reinterpret_cast<HWND>(hwnd); }

// The target is more than a top-level window: Explorer's address bar, a search box
// and a chat input are child controls. Activating the top-level window can move
// focus to its default control (the file list), and simulated keys have no
// recipient of their own, so Ctrl+V would land there instead of where the user
// was typing.
struct PasteTarget {
    Hwnd top = 0;
    HWND focus = nullptr;
};

PasteTarget resolve_target(Hwnd hwnd, Hwnd remembered_focus) {
    PasteTarget target{hwnd, nullptr};
    if (hwnd) target.focus = as_hwnd(focused_child_control(hwnd));
    // Losing activation usually clears the target thread's focus window; fall
    // back to the control recorded while the window was still in front.
    if (!target.focus && remembered_focus && IsWindow(as_hwnd(remembered_focus)))
        target.focus = as_hwnd(remembered_focus);
    return target;
}

// Native edit controls handle WM_PASTE directly, with no need for focus or a
// simulated keystroke. Win10 Explorer's address bar is an EDIT child of a
// ComboBoxEx32; this also covers the plain Edit controls in dialogs.
HWND edit_control(HWND focus) {
    if (!focus || !IsWindow(focus)) return nullptr;
    wchar_t cls[64] = {};
    if (GetClassNameW(focus, cls, 64) == 0) return nullptr;
    if (std::wcscmp(cls, L"Edit") == 0) return focus;
    if (std::wcscmp(cls, L"ComboBoxEx32") == 0 || std::wcscmp(cls, L"ComboBox") == 0)
        return FindWindowExW(focus, nullptr, L"Edit", nullptr);
    return nullptr;
}

// SetFocus only works on a window owned by the calling thread unless the input
// queues are attached. Keep them attached until the keystroke has gone out, or
// detaching can hand focus back to the default control.
void send_paste(const PasteTarget &target) {
    if (HWND edit = edit_control(target.focus)) {
        release_modifiers();
        // A hung target must not freeze our UI while waiting for the message.
        SendMessageTimeoutW(edit, WM_PASTE, 0, 0, SMTO_ABORTIFHUNG, 300, nullptr);
        return;
    }
    DWORD target_thread = 0;
    bool attached = false;
    if (target.focus && IsWindow(target.focus) && target.top) {
        target_thread = GetWindowThreadProcessId(as_hwnd(target.top), nullptr);
        if (target_thread && target_thread != GetCurrentThreadId())
            attached = AttachThreadInput(GetCurrentThreadId(), target_thread, TRUE) != 0;
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(target_thread, &info) || info.hwndFocus != target.focus)
            SetFocus(target.focus);
    }
    release_modifiers();
    send_ctrl_v();
    if (attached) AttachThreadInput(GetCurrentThreadId(), target_thread, FALSE);
}

void activate_then_paste(PasteTarget target, int attempt) {
    if (get_foreground_hwnd() == target.top) {
        send_paste(target);
        return;
    }
    if (attempt >= ACTIVATE_MAX_ATTEMPTS) {
        // Not sent blindly while this process is still in front: this app's own shortcut handlers would take the keys.
        if (foreground_is_own_process()) {
            log_debug(T("目标窗口 {hwnd} 未切到前台，放弃本次补按键", {{"hwnd", static_cast<qulonglong>(target.top)}}), "Clipboard");
            return;
        }
        log_debug(T("目标窗口 {hwnd} 未切到前台，按当前焦点粘贴", {{"hwnd", static_cast<qulonglong>(target.top)}}), "Clipboard");
        send_paste(target);
        return;
    }
    set_foreground_window(target.top);
    QTimer::singleShot(ACTIVATE_RETRY_MS, [target, attempt] { activate_then_paste(target, attempt + 1); });
}
}

Hwnd focused_child_control(Hwnd hwnd) {
    if (!hwnd) return 0;
    const HWND root = reinterpret_cast<HWND>(hwnd);
    DWORD root_pid = 0;
    const DWORD thread = GetWindowThreadProcessId(root, &root_pid);
    if (!thread) return 0;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    // Each thread keeps its own focus window even while another window is in
    // front, so the address bar is still recorded after our window opens.
    if (!GetGUIThreadInfo(thread, &info) || !info.hwndFocus) return 0;
    const HWND focus = info.hwndFocus;
    if (focus == root || !IsWindow(focus)) return 0;
    DWORD focus_pid = 0;
    GetWindowThreadProcessId(focus, &focus_pid);
    if (!focus_pid || focus_pid != root_pid) return 0;
    if (!IsChild(root, focus) && GetAncestor(focus, GA_ROOT) != root) return 0;
    return reinterpret_cast<Hwnd>(focus);
}

bool set_foreground_window(Hwnd target) {
    if (!target) return false;
    const auto hwnd = reinterpret_cast<HWND>(target);
    // SetForegroundWindow does not restore a minimized window, so the keys would go nowhere.
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    const DWORD current_thread = GetCurrentThreadId();
    const DWORD target_thread = GetWindowThreadProcessId(hwnd, nullptr);
    // Attaching to the target's input queue lifts the foreground lock; the queues share
    // focus state while attached, so detach right after.
    const bool attached = target_thread && target_thread != current_thread && AttachThreadInput(current_thread, target_thread, TRUE);
    BringWindowToTop(hwnd);
    const bool result = SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(current_thread, target_thread, FALSE);
    return result;
}

void release_modifiers() {
    for (BYTE vk : MODIFIER_KEYS)
        if (GetAsyncKeyState(vk) & 0x8000) keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
}

bool send_ctrl_v() {
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event('V', 0, 0, 0);
    keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    return true;
}

void paste_to_target(Hwnd hwnd, Hwnd focus) {
    if (test_handler()) {
        test_handler()(hwnd);
        return;
    }
    const PasteTarget target = resolve_target(hwnd, focus);
    if (!hwnd) {
        if (!foreground_is_own_process()) send_paste(target);
        return;
    }
    activate_then_paste(target, 0);
}

void set_paste_to_target_for_test(std::function<void(Hwnd)> handler) { test_handler() = std::move(handler); }
}
