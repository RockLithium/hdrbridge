#include <Windows.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>

namespace {
HWND wait_for_window(const wchar_t* class_name, DWORD process_id,
                     bool visible_only, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, class_name, nullptr))) {
      DWORD owner = 0;
      GetWindowThreadProcessId(window, &owner);
      if (owner == process_id && (!visible_only || IsWindowVisible(window))) return window;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return nullptr;
}

HWND child_with_text(HWND parent, const wchar_t* expected) {
  struct Search { const wchar_t* expected; HWND result = nullptr; } search{expected};
  EnumChildWindows(parent, [](HWND child, LPARAM parameter) -> BOOL {
    auto* search = reinterpret_cast<Search*>(parameter);
    wchar_t text[128]{};
    GetWindowTextW(child, text, 128);
    if (std::wstring(text) == search->expected) {
      search->result = child;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.result;
}

HWND child_with_class(HWND parent, const wchar_t* expected) {
  struct Search { const wchar_t* expected; HWND result = nullptr; } search{expected};
  EnumChildWindows(parent, [](HWND child, LPARAM parameter) -> BOOL {
    auto* search = reinterpret_cast<Search*>(parameter);
    wchar_t cls[128]{};
    GetClassNameW(child, cls, 128);
    if (std::wstring(cls) == search->expected) {
      search->result = child;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.result;
}

void click_button(HWND parent, HWND button) {
  SendMessageW(parent, WM_COMMAND,
               MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED),
               reinterpret_cast<LPARAM>(button));
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) return 2;
  std::wstring command = L"\"" + std::wstring(argv[1]) + L"\" \"" + argv[2] + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process)) return 3;
  CloseHandle(process.hThread);
  const auto finish = [&] {
    HWND main = wait_for_window(L"HDRBridgeMainWindow", process.dwProcessId, false, 250);
    if (main) PostMessageW(main, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) {
      TerminateProcess(process.hProcess, 9);
      WaitForSingleObject(process.hProcess, 1000);
    }
    CloseHandle(process.hProcess);
  };

  HWND main = wait_for_window(L"HDRBridgeMainWindow", process.dwProcessId, true, 8000);
  HWND preview = wait_for_window(L"HDRBridgePreviewWindow", process.dwProcessId, false, 8000);
  if (!main || !preview || IsWindowVisible(preview)) { finish(); return 4; }
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  if (IsWindowVisible(preview)) { finish(); return 5; }
  HWND preview_button = child_with_text(main, L"Preview");
  if (!preview_button) { finish(); return 6; }
  SetWindowPos(main, nullptr, 40, 40, 1040, 760, SWP_NOZORDER);
  click_button(main, preview_button);
  preview = wait_for_window(L"HDRBridgePreviewWindow", process.dwProcessId, true, 3000);
  if (!preview) { finish(); return 7; }
  if (GetWindow(preview, GW_OWNER) != nullptr) { finish(); return 21; }
  RECT main_rect{}, initial_preview_rect{};
  GetWindowRect(main, &main_rect);
  GetWindowRect(preview, &initial_preview_rect);
  const int initial_height = initial_preview_rect.bottom - initial_preview_rect.top;
  const int initial_width = initial_preview_rect.right - initial_preview_rect.left;
  if (std::abs(initial_preview_rect.left - main_rect.right - 8) > 4 ||
      std::abs(initial_height - (main_rect.bottom - main_rect.top)) > 8 ||
      std::abs(initial_width / static_cast<double>(initial_height) - 0.75) > 0.03) {
    finish(); return 18;
  }
  const LONG_PTR style = GetWindowLongPtrW(preview, GWL_STYLE);
  if ((style & (WS_MAXIMIZEBOX | WS_THICKFRAME | WS_SYSMENU)) !=
      (WS_MAXIMIZEBOX | WS_THICKFRAME | WS_SYSMENU)) { finish(); return 8; }
  HWND zoom = child_with_class(preview, L"ComboBox");
  HWND fullscreen = child_with_class(preview, L"Button");
  if (!zoom || SendMessageW(zoom, CB_GETCURSEL, 0, 0) != 0 || !fullscreen) {
    finish(); return 9;
  }
  bool decoded = false;
  for (int attempt = 0; attempt < 300; ++attempt) {
    wchar_t title[256]{};
    GetWindowTextW(preview, title, 256);
    const std::wstring value(title);
    if (value.find(L"6960 × 4640") != std::wstring::npos) { decoded = true; break; }
    if (value.find(L"unavailable") != std::wstring::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!decoded) { finish(); return 13; }
  RECT client{};
  GetClientRect(preview, &client);
  POINT anchor{client.right / 2, client.bottom / 2};
  ClientToScreen(preview, &anchor);
  SendMessageW(preview, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA),
               MAKELPARAM(anchor.x, anchor.y));
  if (SendMessageW(zoom, CB_GETCURSEL, 0, 0) != CB_ERR) {
    finish(); return 14;
  }
  wchar_t zoom_before_drag[32]{}, zoom_after_drag[32]{};
  GetWindowTextW(zoom, zoom_before_drag, 32);
  SendMessageW(preview, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 200));
  SendMessageW(preview, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(230, 220));
  SendMessageW(preview, WM_LBUTTONUP, 0, MAKELPARAM(230, 220));
  GetWindowTextW(zoom, zoom_after_drag, 32);
  if (std::wstring(zoom_before_drag) != zoom_after_drag ||
      std::wstring(zoom_after_drag) == L"Fit") {
    finish(); return 20;
  }
  ScreenToClient(preview, &anchor);
  SendMessageW(preview, WM_LBUTTONDBLCLK, MK_LBUTTON,
               MAKELPARAM(anchor.x, anchor.y));
  if (SendMessageW(zoom, CB_GETCURSEL, 0, 0) != 0) {
    finish(); return 15;
  }
  SendMessageW(preview, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 200));
  SendMessageW(preview, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(230, 220));
  SendMessageW(preview, WM_LBUTTONUP, 0, MAKELPARAM(230, 220));
  click_button(preview, fullscreen);
  if ((GetWindowLongPtrW(preview, GWL_STYLE) & WS_OVERLAPPEDWINDOW) != 0) {
    finish(); return 16;
  }
  SetFocus(zoom);
  PostMessageW(zoom, WM_KEYDOWN, VK_ESCAPE, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if ((GetWindowLongPtrW(preview, GWL_STYLE) & WS_OVERLAPPEDWINDOW) !=
      WS_OVERLAPPEDWINDOW) {
    finish(); return 17;
  }
  SetWindowPos(main, nullptr, 50, 50, 1320, 800, SWP_NOZORDER);
  HWND reset = child_with_text(main, L"Reset layout");
  if (!reset) { finish(); return 10; }
  click_button(main, reset);
  RECT preview_rect{};
  GetWindowRect(preview, &preview_rect);
  const int width = preview_rect.right - preview_rect.left;
  const int height = preview_rect.bottom - preview_rect.top;
  if (std::abs(height - 800) > 8 ||
      std::abs(width / static_cast<double>(height) - 0.75) > 0.03) {
    finish(); return 11;
  }
  SendMessageW(preview, WM_CLOSE, 0, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  if (IsWindowVisible(preview) || !child_with_text(main, L"Preview")) {
    finish(); return 12;
  }
  SetWindowPos(preview, nullptr, 20, 20, 500, 500, SWP_NOZORDER);
  SetWindowPos(main, nullptr, 80, 60, 980, 700, SWP_NOZORDER);
  click_button(main, child_with_text(main, L"Preview"));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  RECT reopened_main{}, reopened_preview{};
  GetWindowRect(main, &reopened_main);
  GetWindowRect(preview, &reopened_preview);
  const int reopened_height = reopened_preview.bottom - reopened_preview.top;
  const int reopened_width = reopened_preview.right - reopened_preview.left;
  if (!IsWindowVisible(preview) ||
      std::abs(reopened_preview.left - reopened_main.right - 8) > 4 ||
      std::abs(reopened_height - (reopened_main.bottom - reopened_main.top)) > 8 ||
      std::abs(reopened_width / static_cast<double>(reopened_height) - 0.75) > 0.03) {
    finish(); return 19;
  }
  finish();
  std::cout << "preview UI smoke passed\n";
  return 0;
}
