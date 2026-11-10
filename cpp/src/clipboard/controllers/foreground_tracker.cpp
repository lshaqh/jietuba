#include "clipboard/controllers/foreground_tracker.h"
#include "clipboard/controllers/paste_keystroke.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include <QSet>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba::clipboard {
namespace {
// Taskbar, notification overflow, desktop: pasting there makes no sense.
const QSet<QString> &shell_classes() {
    static const QSet<QString> classes{"Shell_TrayWnd", "NotifyIconOverflowWindow", "TrayNotifyWnd", "Progman", "WorkerW"};
    return classes;
}

const QString WATCHER = "clipboard";

HWND as_hwnd(Hwnd hwnd) { return reinterpret_cast<HWND>(hwnd); }

// Windows 8+ places Start / Search in ZBID_IMMERSIVE_MOGO and
// ZBID_IMMERSIVE_SEARCH. Third-party windows are always in ZBID_DESKTOP, so
// WS_EX_TOPMOST cannot put the picker above them: bands never mix.
const QSet<QString> &immersive_classes() {
    static const QSet<QString> classes{
        "Windows.UI.Core.CoreWindow",
        "Windows.UI.Input.InputSite.WindowClass",
        "XamlExplorerHostIslandWindow"};
    return classes;
}

const QSet<QString> &shell_hosts() {
    static const QSet<QString> hosts{
        "StartMenuExperienceHost.exe", "SearchHost.exe",
        "ShellExperienceHost.exe", "SearchApp.exe"};
    return hosts;
}

QString process_base_name(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t image[MAX_PATH] = {};
    DWORD length = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, image, &length);
    CloseHandle(process);
    if (!ok) return {};
    const QString path = QString::fromWCharArray(image, static_cast<int>(length));
    const int slash = qMax(path.lastIndexOf(QLatin1Char('\\')), path.lastIndexOf(QLatin1Char('/')));
    return slash >= 0 ? path.mid(slash + 1) : path;
}

bool is_shell_flyout(HWND hwnd) {
    if (!hwnd) return false;
    if (!immersive_classes().contains(get_window_class(reinterpret_cast<Hwnd>(hwnd)))) return false;
    return shell_hosts().contains(process_base_name(hwnd));
}

HWND find_desktop_window() {
    const DWORD self = GetCurrentProcessId();
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetNextWindow(hwnd, GW_HWNDNEXT)) {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid || pid == self) continue;
        const QString cls = get_window_class(reinterpret_cast<Hwnd>(hwnd));
        if (shell_classes().contains(cls) || immersive_classes().contains(cls)) continue;
        if (cls == QLatin1String("Progman") || cls == QLatin1String("WorkerW") ||
            cls == QLatin1String("Shell_SecondaryTrayWnd")) continue;
        RECT rect = {};
        if (GetWindowRect(hwnd, &rect) && rect.right - rect.left < 160) continue;
        return hwnd;
    }
    // Nothing ordinary is open: activating the desktop still dismisses Start.
    HWND desktop = FindWindowW(L"Progman", nullptr);
    return desktop ? desktop : GetShellWindow();
}
}

Hwnd get_foreground_hwnd() { return reinterpret_cast<Hwnd>(GetForegroundWindow()); }

bool is_alive(Hwnd hwnd) { return hwnd && IsWindow(as_hwnd(hwnd)) && IsWindowVisible(as_hwnd(hwnd)); }

std::optional<quint32> get_window_pid(Hwnd hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(as_hwnd(hwnd), &pid);
    if (!pid) return std::nullopt;
    return pid;
}

QString get_window_class(Hwnd hwnd) {
    wchar_t buffer[256] = {};
    const int length = GetClassNameW(as_hwnd(hwnd), buffer, 256);
    if (length <= 0) return {};
    return QString::fromWCharArray(buffer, length);
}

bool can_take_focus(Hwnd hwnd) { return !(GetWindowLongW(as_hwnd(hwnd), GWL_EXSTYLE) & WS_EX_NOACTIVATE); }

quint32 get_current_pid() { return GetCurrentProcessId(); }

Hwnd dismiss_shell_flyout() {
    if (!is_shell_flyout(GetForegroundWindow())) return 0;
    const HWND target = find_desktop_window();
    if (!target) return 0;
    // No synthetic Escape here: jietuba's shortcut handler owns Escape and
    // would close the picker we are trying to show. Handing a normal window
    // the foreground is enough to make the shell flyout light-dismiss.
    set_foreground_window(reinterpret_cast<Hwnd>(target));
    return reinterpret_cast<Hwnd>(target);
}

void ensure_topmost(Hwnd hwnd) {
    if (!hwnd) return;
    const HWND handle = reinterpret_cast<HWND>(hwnd);
    SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const LONG_PTR ex = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    if ((ex & WS_EX_TOPMOST) == 0) {
        SetWindowLongPtrW(handle, GWL_EXSTYLE, ex | WS_EX_TOPMOST);
        SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

Hwnd ForegroundWindowTracker::target_hwnd() const { return is_alive(target_hwnd_) ? target_hwnd_ : 0; }

Hwnd ForegroundWindowTracker::target_focus() const {
    if (!target_hwnd()) return 0;
    return target_focus_;
}

void ForegroundWindowTracker::start() {
    sample();
    if (watching_) return;
    auto *hub = input_hub();
    connection_ = connect(hub, &InputHub::foreground, this, [this](quintptr hwnd) { on_foreground(hwnd); }, Qt::QueuedConnection);
    hub->native->watch_foreground(WATCHER);
    watching_ = true;
}

void ForegroundWindowTracker::stop() {
    if (!watching_) return;
    watching_ = false;
    if (auto *hub = existing_input_hub()) hub->native->unwatch_foreground(WATCHER);
    disconnect(connection_);
}

bool ForegroundWindowTracker::sample() { return consider(get_foreground_hwnd()); }

void ForegroundWindowTracker::on_foreground(Hwnd hwnd) {
    // Switches delivered after stop no longer count.
    if (watching_) consider(hwnd);
}

bool ForegroundWindowTracker::consider(Hwnd hwnd) {
    if (!eligible(hwnd)) return false;
    // Remember the focused child while the window is still in front. Explorer's
    // address bar is a child control; once the window loses activation the thread
    // usually reports no focus at all, so it cannot be recovered later.
    const Hwnd focus = focused_child_control(hwnd);
    const bool target_changed = hwnd != target_hwnd_;
    if (!target_changed && focus == target_focus_) return false;
    target_hwnd_ = hwnd;
    target_focus_ = focus;
    log_debug(T("粘贴目标更新为 {hwnd} ({cls})", {{"hwnd", static_cast<qulonglong>(hwnd)}, {"cls", get_window_class(hwnd)}}), "Clipboard");
    return target_changed;
}

bool ForegroundWindowTracker::eligible(Hwnd hwnd) const {
    if (!is_alive(hwnd)) return false;
    if (!can_take_focus(hwnd)) return false;
    if (shell_classes().contains(get_window_class(hwnd))) return false;
    return !is_excluded_ || !is_excluded_(hwnd);
}
}
