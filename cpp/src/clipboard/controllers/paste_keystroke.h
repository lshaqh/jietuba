#pragma once
#include "clipboard/controllers/foreground_tracker.h"
#include <functional>

// Gives focus back to the target window, then presses Ctrl+V. Simulated keys have no
// recipient: they land wherever focus is at that moment, so activation comes first,
// retried on a timer because SetForegroundWindow returning does not mean it happened.
namespace jietuba::clipboard {
// Brings hwnd to the foreground, attaching to its input queue for the call.
bool set_foreground_window(Hwnd hwnd);
// Releases modifiers the user still holds from the hotkey that opened the window.
void release_modifiers();
bool send_ctrl_v();
// Activates hwnd and pastes; with no target (closed window) pastes into the current
// foreground unless that is this application. focus is the child control that had
// the keyboard focus while hwnd was still in front; a window that loses activation
// often reports no focus at all, so it has to be remembered beforehand.
void paste_to_target(Hwnd hwnd, Hwnd focus = 0);
#ifdef _WIN32
// The child control that owns the focus inside hwnd, or 0 when focus is on hwnd
// itself. Explorer's address bar is a child control: activating the top-level
// window puts focus on the file list, so simulated Ctrl+V has to be directed
// back to the control the user was typing in. Also used by the regression test.
Hwnd focused_child_control(Hwnd hwnd);
// A visible Edit inside an Explorer window. The address bar only owns an Edit
// while it is in edit mode; losing activation hides it and clears the thread's
// focus, so neither remembered focus nor simulated keys can reach it.
Hwnd visible_address_bar_edit(Hwnd hwnd);
bool is_explorer_window(Hwnd hwnd);
#endif
// Hands every paste_to_target call to handler instead of pressing keys; empty restores.
void set_paste_to_target_for_test(std::function<void(Hwnd)> handler);
}
