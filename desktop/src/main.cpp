#include "hdrbridge_core.h"
#include "output_naming.h"

#include <Windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <atomic>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"HDRBridgeMainWindow";
constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_SUCCESS = WM_APP + 2;
constexpr UINT WM_APP_ERROR = WM_APP + 3;
constexpr UINT WM_APP_OPEN_SOURCE = WM_APP + 4;
constexpr UINT WM_APP_SPLITTER_MOVE = WM_APP + 5;
constexpr UINT WM_APP_SPLITTER_END = WM_APP + 6;
constexpr UINT WM_APP_ITEM_STATUS = WM_APP + 7;
constexpr UINT_PTR kProgressResetTimer = 1;
constexpr wchar_t kLayoutRegistryPath[] = L"Software\\HDR Bridge\\Desktop";

enum ControlId {
  IDC_OPEN = 100,
  IDC_SOURCE_PATH,
  IDC_INSPECTOR,
  IDC_FORMAT,
  IDC_LOSSLESS,
  IDC_COPY_EXIF,
  IDC_COPY_XMP,
  IDC_QUALITY,
  IDC_QUALITY_VALUE,
  IDC_CONVERT,
  IDC_CANCEL,
  IDC_REVEAL,
  IDC_OPEN_OUTPUT,
  IDC_PROGRESS,
  IDC_STATUS,
  IDC_OUTPUT_PATH,
  IDC_LOG,
  IDC_FORMAT_NOTE
  ,IDC_GAMUT
  ,IDC_TRANSFER
  ,IDC_GAINMAP_RESOLUTION
  ,IDC_GAINMAP_CHANNELS
  ,IDC_PEAK
  ,IDC_SOURCE_FOLDER
  ,IDC_CHOOSE_FOLDER
  ,IDC_FOLDER_PATH
  ,IDC_BASE_NAME
  ,IDC_NAME_SUFFIX
  ,IDC_COLLISION
  ,IDC_QUEUE
  ,IDC_REMOVE
  ,IDC_CLEAR
  ,IDC_QUEUE_LABEL
  ,IDC_OUTPUT_LABEL
  ,IDC_INSPECTOR_LABEL
  ,IDC_ACTIVITY_LABEL
  ,IDC_TITLE
  ,IDC_RESET_LAYOUT
  ,IDC_VERTICAL_SPLITTER
  ,IDC_HORIZONTAL_SPLITTER
};

enum class QueueStatus { pending, success, skipped, failed };

struct QueueItemStatus {
  QueueStatus status = QueueStatus::pending;
  std::wstring reason;
};

struct ItemStatusPayload {
  size_t index = 0;
  QueueStatus status = QueueStatus::pending;
  std::wstring label;
  std::wstring reason;
};

struct SuccessPayload {
  std::vector<hdrbridge::ConversionResult> results;
  size_t succeeded = 0;
  size_t skipped = 0;
  size_t failed = 0;
};

struct AppState {
  HWND window = nullptr;
  HWND open = nullptr;
  HWND source_path = nullptr;
  HWND inspector = nullptr;
  HWND format = nullptr;
  HWND lossless = nullptr;
  HWND copy_exif = nullptr;
  HWND copy_xmp = nullptr;
  HWND quality = nullptr;
  HWND quality_value = nullptr;
  HWND convert = nullptr;
  HWND cancel_button = nullptr;
  HWND reveal = nullptr;
  HWND open_output = nullptr;
  HWND progress = nullptr;
  HWND status = nullptr;
  HWND output_path = nullptr;
  HWND log = nullptr;
  HWND format_note = nullptr;
  HWND gamut = nullptr;
  HWND transfer = nullptr;
  HWND gainmap_resolution = nullptr;
  HWND gainmap_channels = nullptr;
  HWND peak = nullptr;
  HWND source_folder = nullptr;
  HWND choose_folder = nullptr;
  HWND folder_path = nullptr;
  HWND base_name = nullptr;
  HWND name_suffix = nullptr;
  HWND collision = nullptr;
  HWND queue = nullptr;
  HWND remove = nullptr;
  HWND clear = nullptr;
  HWND queue_label = nullptr;
  HWND output_label = nullptr;
  HWND inspector_label = nullptr;
  HWND activity_label = nullptr;
  HWND title = nullptr;
  HWND reset_layout = nullptr;
  HWND vertical_splitter = nullptr;
  HWND horizontal_splitter = nullptr;
  HFONT font = nullptr;
  HFONT title_font = nullptr;
  HBRUSH background_brush = nullptr;
  HBRUSH surface_brush = nullptr;
  COLORREF background_color = RGB(244, 246, 248);
  COLORREF surface_color = RGB(255, 255, 255);
  COLORREF text_color = RGB(28, 35, 43);
  COLORREF muted_text_color = RGB(92, 104, 116);
  std::filesystem::path input;
  std::filesystem::path output;
  // Output policy is session state, independent of the current queue/source.
  // selected_folder is the last explicitly chosen custom folder and must never
  // be overwritten while inspecting a source.
  bool use_source_folder = true;
  std::filesystem::path selected_folder;
  std::vector<std::filesystem::path> inputs;
  std::vector<std::optional<hdrbridge::SourceInfo>> input_infos;
  std::vector<QueueItemStatus> input_statuses;
  std::atomic_bool cancel{false};
  bool running = false;
  bool dark = false;
  int right_panel_width = 560;
  int activity_panel_height = 230;
  int last_format_index = -1;
  int last_transfer_index = -1;
};

std::wstring widen(const std::string& input) {
  if (input.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
  std::wstring output(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), count);
  return output;
}

std::string narrow(const std::wstring& input) {
  if (input.empty()) return {};
  const int count = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
  std::string output(static_cast<size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), count, nullptr, nullptr);
  return output;
}

void layout(AppState* state, int width, int height);

bool font_available(const wchar_t* family) {
  LOGFONTW query{};
  query.lfCharSet = DEFAULT_CHARSET;
  wcsncpy_s(query.lfFaceName, family, _TRUNCATE);
  bool found = false;
  HDC dc = GetDC(nullptr);
  EnumFontFamiliesExW(dc, &query,
      [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM parameter) -> int {
        *reinterpret_cast<bool*>(parameter) = true;
        return 0;
      }, reinterpret_cast<LPARAM>(&found), 0);
  ReleaseDC(nullptr, dc);
  return found;
}

bool system_prefers_dark() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
    return false;
  }
  return value == 0;
}

void apply_fonts(AppState* state) {
  const wchar_t* family = font_available(L"Ubuntu") ? L"Ubuntu" : L"Segoe UI";
  const UINT dpi = GetDpiForWindow(state->window);
  const int pixels = -MulDiv(10, dpi, 72);
  HFONT next = CreateFontW(pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           family);
  HFONT next_title = CreateFontW(-MulDiv(12, dpi, 72), 0, 0, 0, FW_SEMIBOLD,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 family);
  if (!next || !next_title) {
    if (next) DeleteObject(next);
    if (next_title) DeleteObject(next_title);
    return;
  }
  EnumChildWindows(state->window, [](HWND child, LPARAM parameter) -> BOOL {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(parameter), TRUE);
    return TRUE;
  }, reinterpret_cast<LPARAM>(next));
  SendMessageW(state->title, WM_SETFONT, reinterpret_cast<WPARAM>(next_title), TRUE);
  if (state->font) DeleteObject(state->font);
  if (state->title_font) DeleteObject(state->title_font);
  state->font = next;
  state->title_font = next_title;
  RECT rect{};
  GetClientRect(state->window, &rect);
  layout(state, rect.right, rect.bottom);
  InvalidateRect(state->window, nullptr, TRUE);
}

void apply_theme(AppState* state) {
  state->dark = system_prefers_dark();
  state->background_color = state->dark ? RGB(24, 27, 31) : RGB(244, 246, 248);
  state->surface_color = state->dark ? RGB(35, 39, 45) : RGB(255, 255, 255);
  state->text_color = state->dark ? RGB(235, 239, 243) : RGB(28, 35, 43);
  state->muted_text_color = state->dark ? RGB(166, 176, 186) : RGB(92, 104, 116);
  if (state->background_brush) DeleteObject(state->background_brush);
  if (state->surface_brush) DeleteObject(state->surface_brush);
  state->background_brush = CreateSolidBrush(state->background_color);
  state->surface_brush = CreateSolidBrush(state->surface_color);
  const BOOL dark_title = state->dark ? TRUE : FALSE;
  DwmSetWindowAttribute(state->window, 20, &dark_title, sizeof(dark_title));
  EnumChildWindows(state->window, [](HWND child, LPARAM parameter) -> BOOL {
    const bool dark = parameter != 0;
    SetWindowTheme(child, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    return TRUE;
  }, state->dark ? 1 : 0);
  SendMessageW(state->progress, PBM_SETBARCOLOR, 0,
               state->dark ? RGB(92, 151, 224) : RGB(45, 112, 190));
  InvalidateRect(state->window, nullptr, TRUE);
}

std::optional<DWORD> read_layout_value(const wchar_t* name) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kLayoutRegistryPath, name,
                   RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return value;
}

void write_layout_value(HKEY key, const wchar_t* name, DWORD value) {
  RegSetValueExW(key, name, 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void load_split_layout(AppState* state) {
  if (const auto value = read_layout_value(L"RightPanelWidth")) {
    state->right_panel_width = static_cast<int>(*value);
  }
  if (const auto value = read_layout_value(L"ActivityPanelHeight")) {
    state->activity_panel_height = static_cast<int>(*value);
  }
}

void save_layout(AppState* state) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kLayoutRegistryPath, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  write_layout_value(key, L"RightPanelWidth", static_cast<DWORD>(state->right_panel_width));
  write_layout_value(key, L"ActivityPanelHeight", static_cast<DWORD>(state->activity_panel_height));
  WINDOWPLACEMENT placement{sizeof(placement)};
  if (GetWindowPlacement(state->window, &placement)) {
    const RECT& rect = placement.rcNormalPosition;
    write_layout_value(key, L"WindowLeft", static_cast<DWORD>(rect.left));
    write_layout_value(key, L"WindowTop", static_cast<DWORD>(rect.top));
    write_layout_value(key, L"WindowWidth", static_cast<DWORD>(rect.right - rect.left));
    write_layout_value(key, L"WindowHeight", static_cast<DWORD>(rect.bottom - rect.top));
  }
  RegCloseKey(key);
}

void restore_window_bounds(HWND window) {
  const auto left_value = read_layout_value(L"WindowLeft");
  const auto top_value = read_layout_value(L"WindowTop");
  const auto width_value = read_layout_value(L"WindowWidth");
  const auto height_value = read_layout_value(L"WindowHeight");
  if (!left_value || !top_value || !width_value || !height_value) return;
  int width = std::max(980, static_cast<int>(*width_value));
  int height = std::max(760, static_cast<int>(*height_value));
  int left = static_cast<LONG>(*left_value);
  int top = static_cast<LONG>(*top_value);
  RECT requested{left, top, left + width, top + height};
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromRect(&requested, MONITOR_DEFAULTTONEAREST), &monitor);
  const int work_left = static_cast<int>(monitor.rcWork.left);
  const int work_top = static_cast<int>(monitor.rcWork.top);
  const int work_right = static_cast<int>(monitor.rcWork.right);
  const int work_bottom = static_cast<int>(monitor.rcWork.bottom);
  width = std::min(width, work_right - work_left);
  height = std::min(height, work_bottom - work_top);
  left = std::clamp(left, work_left, work_right - width);
  top = std::clamp(top, work_top, work_bottom - height);
  SetWindowPos(window, nullptr, left, top, width, height,
               SWP_NOACTIVATE | SWP_NOZORDER);
}

std::wstring queue_summary(const std::filesystem::path& path,
                           const hdrbridge::SourceInfo& info) {
  std::wostringstream out;
  out << path.filename().wstring() << L"    " << widen(info.format)
      << L" · " << info.width << L"×" << info.height
      << L" · " << info.bit_depth << L"-bit";
  if (info.transfer == 16) out << L" PQ";
  else if (info.transfer == 18) out << L" HLG";
  if (info.gain_map_present) out << L" · Gain map";
  return out.str();
}

LRESULT CALLBACK queue_subclass(HWND control, UINT message, WPARAM wparam,
                                LPARAM lparam, UINT_PTR, DWORD_PTR) {
  const bool select_all =
      (message == WM_KEYDOWN && wparam == L'A' &&
       (GetKeyState(VK_CONTROL) & 0x8000) != 0) ||
      (message == WM_CHAR && wparam == 1);
  if (select_all) {
    const int count = static_cast<int>(SendMessageW(control, LB_GETCOUNT, 0, 0));
    for (int index = 0; index < count; ++index) {
      SendMessageW(control, LB_SETSEL, TRUE, index);
    }
    if (count > 0) SendMessageW(control, LB_SETCARETINDEX, 0, FALSE);
    InvalidateRect(control, nullptr, TRUE);
    return 0;
  }
  if (message == WM_KEYDOWN && wparam == VK_DELETE) {
    SendMessageW(GetParent(control), WM_COMMAND,
                 MAKEWPARAM(IDC_REMOVE, BN_CLICKED),
                 reinterpret_cast<LPARAM>(control));
    return 0;
  }
  return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK splitter_subclass(HWND control, UINT message, WPARAM wparam,
                                   LPARAM lparam, UINT_PTR, DWORD_PTR kind) {
  if (message == WM_SETCURSOR) {
    SetCursor(LoadCursorW(nullptr, kind == 1 ? IDC_SIZEWE : IDC_SIZENS));
    return TRUE;
  }
  if (message == WM_LBUTTONDOWN) {
    SetCapture(control);
    return 0;
  }
  if (message == WM_MOUSEMOVE && GetCapture() == control) {
    POINT point{};
    GetCursorPos(&point);
    ScreenToClient(GetParent(control), &point);
    SendMessageW(GetParent(control), WM_APP_SPLITTER_MOVE,
                 static_cast<WPARAM>(kind), MAKELPARAM(point.x, point.y));
    return 0;
  }
  if (message == WM_LBUTTONUP && GetCapture() == control) {
    ReleaseCapture();
    SendMessageW(GetParent(control), WM_APP_SPLITTER_END,
                 static_cast<WPARAM>(kind), 0);
    return 0;
  }
  return DefSubclassProc(control, message, wparam, lparam);
}

void append_log(AppState* state, const std::wstring& line) {
  const int length = GetWindowTextLengthW(state->log);
  std::wstring text(static_cast<size_t>(length), L'\0');
  if (length) GetWindowTextW(state->log, text.data(), length + 1);
  text += (length ? L"\r\n" : L"") + line;
  SendMessageW(state->log, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
  SendMessageW(state->log, EM_SETSEL, text.size(), text.size());
  SendMessageW(state->log, EM_SCROLLCARET, 0, 0);
}

std::wstring metadata_status_label(const std::string& status) {
  if (status == "present") return L"Present";
  if (status == "absent") return L"Absent";
  if (status == "read-error") return L"Read error";
  return L"Unsupported";
}

std::wstring primaries_label(uint16_t value) {
  if (value == 9) return L"BT.2020";
  if (value == 12) return L"Display P3 / P3-D65";
  if (value == 1) return L"BT.709 / sRGB primaries";
  return L"Unspecified / other primaries";
}

std::wstring transfer_label(uint16_t value) {
  if (value == 16) return L"PQ / ST 2084";
  if (value == 18) return L"HLG / BT.2100";
  if (value == 13) return L"sRGB transfer";
  if (value == 8) return L"Linear";
  return L"Unspecified / other transfer";
}

std::wstring matrix_label(uint16_t value) {
  if (value == 0) return L"RGB / identity";
  if (value == 9) return L"BT.2020 non-constant luminance";
  return L"Matrix " + std::to_wstring(value);
}

std::wstring source_summary(const hdrbridge::SourceInfo& i) {
  std::wostringstream out;
  out << L"IMAGE\r\n" << i.width << L" × " << i.height
      << L"\r\n" << widen(i.format) << L"  •  " << widen(i.asset_kind)
      << L"\r\n\r\nCONTAINER\r\n" << widen(i.container_brand)
      << L"\r\n" << widen(i.codec);

  if (i.gain_map_present) {
    out << L"\r\n\r\nBASE RENDITION\r\n"
        << i.base_width << L" × " << i.base_height << L"  •  "
        << i.base_bit_depth << L"-bit  •  " << widen(i.base_codec)
        << L"  •  " << i.base_channels << L" channels"
        << L"\r\n" << widen(i.base_color_space) << L"  •  "
        << widen(i.base_transfer) << L"  •  "
        << (!i.range_known ? L"Unknown range" : i.full_range ? L"Full range" : L"Limited range")
        << L"\r\nSource color  " << primaries_label(i.primaries)
        << L"  •  " << transfer_label(i.transfer)
        << L"  •  " << matrix_label(i.matrix)
        << L"\r\n\r\nGAIN MAP\r\nFamily  " << widen(i.gain_map_family)
        << L"\r\nMap  " << i.gain_map_width << L" × " << i.gain_map_height
        << L"  •  " << (i.gain_map_channels == 3 ? L"RGB (3 channels)" :
                          i.gain_map_channels == 1 ? L"Mono (1 channel)" : L"Unknown channels")
        << L"\r\nScale  " << i.gain_map_scale_x << L"× × "
        << i.gain_map_scale_y << L"×"
        << L"\r\nCapacity  " << i.hdr_capacity_min << L" – "
        << i.hdr_capacity_max << L"×  (log2 " << i.base_hdr_headroom
        << L" – " << i.alternate_hdr_headroom << L")"
        << L"\r\nGain min/max  R " << i.gain_map_min[0] << L" / " << i.gain_map_max[0]
        << L"  G " << i.gain_map_min[1] << L" / " << i.gain_map_max[1]
        << L"  B " << i.gain_map_min[2] << L" / " << i.gain_map_max[2]
        << L"\r\nGamma  " << i.gain_map_gamma[0] << L" / "
        << i.gain_map_gamma[1] << L" / " << i.gain_map_gamma[2];
    if (i.base_item_id || i.gain_map_item_id || i.tone_map_item_id) {
      out << L"\r\nItems  base " << i.base_item_id << L" / map "
          << i.gain_map_item_id << L" / tmap " << i.tone_map_item_id;
    }
    if (!i.auxiliary_type.empty()) {
      out << L"\r\nAuxiliary type  " << widen(i.auxiliary_type);
    }
    out << L"\r\n\r\nRECONSTRUCTED HDR\r\n"
        << i.width << L" × " << i.height << L"  •  "
        << widen(i.reconstructed_color_space) << L"  •  "
        << widen(i.reconstructed_transfer) << L"  •  "
        << widen(i.reconstructed_precision);
  } else {
    out << L"\r\n\r\nPRIMARY ITEM\r\n";
    if (i.is_grid) {
      out << L"Grid " << i.grid_columns << L" × " << i.grid_rows
          << L"\r\nTile " << i.tile_width << L" × " << i.tile_height;
    } else {
      out << L"Single image";
    }
    out << L"\r\n\r\nSOURCE SIGNAL\r\n" << i.bit_depth << L"-bit  "
        << widen(i.pixel_format) << L"  •  "
        << (!i.range_known ? L"Unknown range" : i.full_range ? L"Full range" : L"Limited range")
        << L"\r\n" << widen(i.color_signal_kind);
    if (i.color_signal_kind.find("WIC pixel format") == std::string::npos) {
      out << L"  " << i.primaries << L" / " << i.transfer << L" / " << i.matrix;
    }
    out << L"\r\nPrimaries  " << primaries_label(i.primaries)
        << L"\r\nTransfer   " << transfer_label(i.transfer)
        << L"\r\nMatrix     " << matrix_label(i.matrix);
  }
  out << L"\r\n\r\nSOURCE METADATA\r\nExif  " << metadata_status_label(i.exif_status)
      << L"\r\nXMP   " << metadata_status_label(i.xmp_status)
      << L"\r\nICC   " << metadata_status_label(i.icc_status)
      << L"\r\nOrientation  " << metadata_status_label(i.orientation_status)
      << L"  •  source " << static_cast<int>(i.original_orientation)
      << L" -> canonical " << static_cast<int>(
          i.orientation_normalized ? 1 : i.original_orientation);
  return out.str();
}

std::wstring mode_for_index(int index) {
  switch (index) {
    case 1: return L"png-pq16";
    case 2: return L"jxl-pq16";
    case 3: return L"jxr-scrgb-fp16";
    case 4: return L"avif-pq10";
    case 5: return L"tiff-pq16";
    case 6: return L"jxr-rgb10-experimental";
    default: return L"ultrahdr";
  }
}

std::wstring suffix_for_mode(const std::wstring& mode, bool hlg = false) {
  if (mode == L"jxr-scrgb-fp16") return L"_scrgb-fp16.jxr";
  if (mode == L"ultrahdr") return L"_ultrahdr.jpg";
  if (mode == L"png-pq16") return hlg ? L"_hdr-hlg.png" : L"_hdr-pq16.png";
  if (mode == L"tiff-pq16") return L"_hdr-pq16.tif";
  if (mode == L"avif-pq10") return hlg ? L"_direct-hlg10.avif" : L"_direct-pq10.avif";
  if (mode == L"jxr-rgb10-experimental") return L"_rgb10-experimental.jxr";
  return hlg ? L"_hlg16.jxl" : L"_pq16.jxl";
}

std::wstring control_text(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<size_t>(length) + 1u, L'\0');
  if (length) GetWindowTextW(control, value.data(), length + 1);
  value.resize(static_cast<size_t>(length));
  return value;
}

void update_output_preview(AppState* state) {
  if (state->input.empty()) return;
  try {
    hdrbridge::desktop::NamingRequest request;
    request.source = state->input;
    request.selected_folder = state->selected_folder;
    request.use_source_folder = state->use_source_folder;
    request.base_name = state->inputs.size() > 1u
        ? state->input.stem().wstring() : control_text(state->base_name);
    request.suffix = control_text(state->name_suffix);
    request.mode = mode_for_index(static_cast<int>(SendMessageW(state->format, CB_GETCURSEL, 0, 0)));
    request.collision = SendMessageW(state->collision, CB_GETCURSEL, 0, 0) == 1
        ? hdrbridge::desktop::CollisionPolicy::overwrite
        : hdrbridge::desktop::CollisionPolicy::auto_number;
    state->output = hdrbridge::desktop::resolve_output_path(request);
    SetWindowTextW(state->output_path, state->output.c_str());
  } catch (const std::exception& error) {
    SetWindowTextW(state->output_path, widen(error.what()).c_str());
  }
}

void update_mode_ui(AppState* state) {
  const int index = static_cast<int>(SendMessageW(state->format, CB_GETCURSEL, 0, 0));
  const int transfer_index = static_cast<int>(SendMessageW(state->transfer, CB_GETCURSEL, 0, 0));
  const bool hlg = transfer_index == 1;
  std::wstring note;
  if (index == 0) note = L"Faithful / Auto • SDR base + ISO gain map";
  else if (index == 1) note = hlg ? L"16-bit HLG • cICP 9/18/0/1" : L"16-bit PQ • cICP 9/16/0/1 • Rec.2100 PQ ICC";
  else if (index == 2) note = hlg ? L"RGB16 HLG • edit / master" : L"RGB16 PQ • edit / master";
  else if (index == 3) note = L"FP16 linear scRGB • edit / master";
  else if (index == 4) note = hlg ? L"10-bit HLG 4:4:4 • compact delivery" : L"10-bit PQ 4:4:4 • compact delivery";
  else if (index == 5) note = L"RGB16 PQ • advanced interchange";
  else note = L"Packed RGB10 • experimental";
  SetWindowTextW(state->format_note, note.c_str());
  const bool transfer_output = index == 1 || index == 2 || index == 4;
  const bool ultrahdr_output = index == 0;
  ShowWindow(state->transfer, transfer_output ? SW_SHOW : SW_HIDE);
  ShowWindow(state->peak, ultrahdr_output ? SW_SHOW : SW_HIDE);
  ShowWindow(state->gainmap_resolution, ultrahdr_output ? SW_SHOW : SW_HIDE);
  ShowWindow(state->gainmap_channels, ultrahdr_output ? SW_SHOW : SW_HIDE);
  ShowWindow(state->gamut, ultrahdr_output ? SW_HIDE : SW_SHOW);
  if (hlg && transfer_output) SendMessageW(state->gamut, CB_SETCURSEL, 0, 0);
  const bool pq_gamut = (index == 1 || index == 2 || index == 4 || index == 5) &&
                        (!transfer_output || !hlg);
  EnableWindow(state->gamut, pq_gamut);
  EnableWindow(state->transfer, transfer_output);
  EnableWindow(state->gainmap_resolution, ultrahdr_output);
  EnableWindow(state->gainmap_channels, ultrahdr_output);
  EnableWindow(state->lossless, index == 2 || index == 3 || index == 6);
  const bool lossy = index == 0 || index == 4 ||
                     SendMessageW(state->lossless, BM_GETCHECK, 0, 0) != BST_CHECKED;
  EnableWindow(state->quality, lossy);
  EnableWindow(state->quality_value, lossy);
  if (state->last_format_index != index || state->last_transfer_index != transfer_index) {
    const auto mode = mode_for_index(index);
    const auto complete_suffix = suffix_for_mode(mode, hlg && transfer_output);
    const auto extension = hdrbridge::desktop::extension_for_mode(mode);
    SetWindowTextW(state->name_suffix,
                   complete_suffix.substr(0, complete_suffix.size() - extension.size()).c_str());
    state->last_format_index = index;
    state->last_transfer_index = transfer_index;
  }
  update_output_preview(state);
}

void layout(AppState* s, int width, int height) {
  const int margin = 24, header = 42, gap = 24;
  const int content_top = header + 8;
  const int max_activity_height = std::max(160, height - (content_top + 450));
  s->activity_panel_height = std::clamp(s->activity_panel_height, 160, max_activity_height);
  const int split_y = height - s->activity_panel_height;
  const int content_bottom = split_y - 10;
  const int max_right_width = std::max(470, width - margin * 2 - gap - 400);
  s->right_panel_width = std::clamp(s->right_panel_width, 470, max_right_width);
  const int right_width = s->right_panel_width;
  const int left_width = width - margin * 2 - gap - right_width;
  const int right_x = margin + left_width + gap;
  MoveWindow(s->title, margin, 7, 180, 30, TRUE);
  constexpr int reset_layout_width = 124;
  MoveWindow(s->reset_layout, width - margin - reset_layout_width, 7,
             reset_layout_width, 30, TRUE);
  MoveWindow(s->vertical_splitter, margin + left_width + 8, content_top,
             8, std::max(40, content_bottom - content_top), TRUE);
  MoveWindow(s->horizontal_splitter, margin, split_y, width - margin * 2, 6, TRUE);
  MoveWindow(s->queue_label, margin, content_top + 3, 120, 26, TRUE);
  constexpr int queue_button_width = 86;
  constexpr int queue_button_gap = 8;
  const int queue_actions_width = queue_button_width * 3 + queue_button_gap * 2;
  const int queue_actions_x = margin + left_width - queue_actions_width;
  MoveWindow(s->open, queue_actions_x, content_top, queue_button_width, 32, TRUE);
  MoveWindow(s->remove, queue_actions_x + queue_button_width + queue_button_gap,
             content_top, queue_button_width, 32, TRUE);
  MoveWindow(s->clear, queue_actions_x + (queue_button_width + queue_button_gap) * 2,
             content_top, queue_button_width, 32, TRUE);
  const int available_left = std::max(300, content_bottom - content_top - 86);
  const int queue_height = std::max(150, available_left * 52 / 100);
  MoveWindow(s->queue, margin, content_top + 38, left_width, queue_height, TRUE);
  MoveWindow(s->source_path, margin, content_top + 44 + queue_height, left_width, 26, TRUE);
  MoveWindow(s->inspector_label, margin, content_top + 74 + queue_height, left_width, 26, TRUE);
  const int inspector_y = content_top + 104 + queue_height;
  MoveWindow(s->inspector, margin, inspector_y, left_width,
             std::max(110, content_bottom - inspector_y), TRUE);

  MoveWindow(s->output_label, right_x, content_top + 3, right_width, 26, TRUE);
  MoveWindow(s->format, right_x, content_top + 36, right_width, 220, TRUE);
  MoveWindow(s->format_note, right_x, content_top + 72, right_width, 24, TRUE);
  MoveWindow(s->gamut, right_x, content_top + 100, right_width, 150, TRUE);
  MoveWindow(s->peak, right_x, content_top + 100, right_width, 170, TRUE);
  MoveWindow(s->transfer, right_x, content_top + 134, right_width, 170, TRUE);
  const int gainmap_option_gap = 8;
  const int gainmap_option_width = (right_width - gainmap_option_gap) / 2;
  MoveWindow(s->gainmap_resolution, right_x, content_top + 134,
             gainmap_option_width, 150, TRUE);
  MoveWindow(s->gainmap_channels,
             right_x + gainmap_option_width + gainmap_option_gap,
             content_top + 134, gainmap_option_width, 150, TRUE);
  const int option_gap = 8;
  const int option_width = (right_width - option_gap * 2) / 3;
  MoveWindow(s->lossless, right_x, content_top + 170, option_width, 28, TRUE);
  MoveWindow(s->copy_exif, right_x + option_width + option_gap, content_top + 170,
             option_width, 28, TRUE);
  MoveWindow(s->copy_xmp, right_x + (option_width + option_gap) * 2, content_top + 170,
             option_width, 28, TRUE);
  MoveWindow(s->quality, right_x, content_top + 202, right_width - 62, 24, TRUE);
  MoveWindow(s->quality_value, right_x + right_width - 54, content_top + 202, 54, 24, TRUE);
  MoveWindow(s->source_folder, right_x, content_top + 234, 160, 28, TRUE);
  MoveWindow(s->choose_folder, right_x + 168, content_top + 232, 104, 32, TRUE);
  MoveWindow(s->folder_path, right_x + 282, content_top + 236, right_width - 282, 24, TRUE);
  MoveWindow(s->base_name, right_x, content_top + 270, right_width * 57 / 100, 30, TRUE);
  MoveWindow(s->name_suffix, right_x + right_width * 59 / 100, content_top + 270,
             right_width * 41 / 100, 30, TRUE);
  MoveWindow(s->collision, right_x, content_top + 306, right_width, 110, TRUE);
  const int action_gap = 10;
  const int action_width = (right_width - action_gap) / 2;
  constexpr int action_height = 38;
  MoveWindow(s->convert, right_x, content_top + 344, action_width, action_height, TRUE);
  MoveWindow(s->cancel_button, right_x + action_width + action_gap, content_top + 344,
             action_width, action_height, TRUE);
  MoveWindow(s->reveal, right_x, content_top + 390, action_width, action_height, TRUE);
  MoveWindow(s->open_output, right_x + action_width + action_gap, content_top + 390,
             action_width, action_height, TRUE);

  const int status_y = split_y + 12;
  MoveWindow(s->progress, margin, status_y, width - margin * 2, 8, TRUE);
  MoveWindow(s->status, margin, status_y + 16, 220, 26, TRUE);
  MoveWindow(s->output_path, margin + 228, status_y + 16, width - margin * 2 - 228, 26, TRUE);
  MoveWindow(s->activity_label, margin, status_y + 48, width - margin * 2, 24, TRUE);
  MoveWindow(s->log, margin, status_y + 76, width - margin * 2,
             height - (status_y + 76) - margin, TRUE);
}

bool supported_source(const std::filesystem::path& path) {
  const auto extension = path.extension().wstring();
  constexpr const wchar_t* extensions[] = {L".hif", L".heic", L".heif", L".avif", L".jxl", L".jxr", L".wdp", L".hdp", L".png", L".tif", L".tiff", L".jpg", L".jpeg", L".jpe"};
  return std::any_of(std::begin(extensions), std::end(extensions),
                     [&](const wchar_t* value) { return _wcsicmp(extension.c_str(), value) == 0; });
}

std::wstring pending_queue_summary(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  if (!extension.empty() && extension.front() == L'.') extension.erase(extension.begin());
  std::transform(extension.begin(), extension.end(), extension.begin(), towupper);
  return path.filename().wstring() + L"    " + extension + L" • Metadata pending";
}

std::wstring queue_result_summary(const std::filesystem::path& path,
                                  QueueStatus status) {
  if (status == QueueStatus::success) {
    return path.filename().wstring() + L"    Converted";
  }
  if (status == QueueStatus::skipped) {
    return path.filename().wstring() + L"    Skipped \u2014 No HDR data";
  }
  if (status == QueueStatus::failed) {
    return path.filename().wstring() + L"    Failed \u2014 Decode error";
  }
  return pending_queue_summary(path);
}

void replace_queue_summary(HWND queue, int index, const std::wstring& text) {
  const int count = static_cast<int>(SendMessageW(queue, LB_GETCOUNT, 0, 0));
  if (index < 0 || index >= count) return;
  std::vector<bool> selected(static_cast<size_t>(count));
  for (int item = 0; item < count; ++item) {
    selected[static_cast<size_t>(item)] = SendMessageW(queue, LB_GETSEL, item, 0) > 0;
  }
  const int caret = static_cast<int>(SendMessageW(queue, LB_GETCARETINDEX, 0, 0));
  const int top = static_cast<int>(SendMessageW(queue, LB_GETTOPINDEX, 0, 0));
  SendMessageW(queue, LB_DELETESTRING, index, 0);
  SendMessageW(queue, LB_INSERTSTRING, index, reinterpret_cast<LPARAM>(text.c_str()));
  for (int item = 0; item < count; ++item) {
    if (selected[static_cast<size_t>(item)]) SendMessageW(queue, LB_SETSEL, TRUE, item);
  }
  if (caret != LB_ERR) SendMessageW(queue, LB_SETCARETINDEX, caret, FALSE);
  if (top != LB_ERR) SendMessageW(queue, LB_SETTOPINDEX, top, 0);
}

void load_source(AppState* state, const std::filesystem::path& path) {
  try {
    if (!supported_source(path)) {
      throw std::runtime_error("Select a supported HDR HIF/HEIF, UHDR, PNG, TIFF, AVIF, JXL, or JXR asset.");
    }
    SetWindowTextW(state->status, L"Inspecting source…");
    hdrbridge::SourceInfo info;
    const auto found = std::find(state->inputs.begin(), state->inputs.end(), path);
    const size_t cached_index = found == state->inputs.end()
        ? state->inputs.size() : static_cast<size_t>(found - state->inputs.begin());
    if (cached_index < state->input_infos.size() && state->input_infos[cached_index]) {
      info = *state->input_infos[cached_index];
    } else {
      info = hdrbridge::inspect(path);
      if (cached_index < state->input_infos.size()) {
        state->input_infos[cached_index] = info;
        replace_queue_summary(state->queue, static_cast<int>(cached_index),
                              queue_summary(path, info));
      }
    }
    state->input = path;
    const std::wstring path_text = path.wstring();
    const std::wstring inspector_text = source_summary(info);
    SendMessageW(state->source_path, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path_text.c_str()));
    SendMessageW(state->inspector, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(inspector_text.c_str()));
    SetWindowTextW(state->status, L"Source ready");
    SetWindowTextW(state->base_name,
                   state->inputs.size() > 1u ? L"Per-source base names" : path.stem().c_str());
    if (state->use_source_folder) {
      SetWindowTextW(state->folder_path, path.parent_path().c_str());
    } else if (!state->selected_folder.empty()) {
      SetWindowTextW(state->folder_path, state->selected_folder.c_str());
    }
    update_mode_ui(state);
    EnableWindow(state->convert, TRUE);
    append_log(state, L"Source inspected: " + std::to_wstring(info.width) + L"×" + std::to_wstring(info.height) +
                      L", " + widen(info.chroma) + L", " + std::to_wstring(info.bit_depth) + L"-bit, CICP " +
                      std::to_wstring(info.primaries) + L"/" + std::to_wstring(info.transfer) + L"/" + std::to_wstring(info.matrix));
  } catch (const std::exception& e) {
    SetWindowTextW(state->status, L"Inspection failed");
    MessageBoxW(state->window, widen(e.what()).c_str(), L"HDR Bridge", MB_OK | MB_ICONERROR);
  }
}

void add_sources(AppState* state, const std::vector<std::filesystem::path>& paths) {
  if (state->running) return;
  size_t first_added = state->inputs.size();
  for (const auto& path : paths) {
    if (!supported_source(path)) {
      append_log(state, L"Skipped unsupported file: " + path.wstring());
      continue;
    }
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    if (std::find(state->inputs.begin(), state->inputs.end(), absolute) != state->inputs.end()) continue;
    state->inputs.push_back(absolute);
    state->input_infos.emplace_back(std::nullopt);
    state->input_statuses.emplace_back();
    const auto label = pending_queue_summary(absolute);
    SendMessageW(state->queue, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  if (state->inputs.empty()) return;
  if (first_added >= state->inputs.size()) first_added = 0;
  SendMessageW(state->queue, LB_SETSEL, FALSE, -1);
  SendMessageW(state->queue, LB_SETSEL, TRUE, first_added);
  SendMessageW(state->queue, LB_SETCARETINDEX, first_added, FALSE);
  load_source(state, state->inputs[first_added]);
  EnableWindow(state->remove, TRUE);
  EnableWindow(state->clear, TRUE);
  SetWindowTextW(state->convert, state->inputs.size() > 1u
      ? L"Convert all" : L"Convert");
  append_log(state, L"Batch queue: " + std::to_wstring(state->inputs.size()) + L" file(s), sequential processing");
}

std::filesystem::path shell_item_path(IShellItem* item) {
  PWSTR value = nullptr;
  const HRESULT result = item->GetDisplayName(SIGDN_FILESYSPATH, &value);
  if (FAILED(result) || !value) return {};
  std::filesystem::path path(value);
  CoTaskMemFree(value);
  return path;
}

void set_dialog_initial_folder(IFileDialog* dialog, const std::filesystem::path& path) {
  if (path.empty()) return;
  IShellItem* folder = nullptr;
  if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr,
                                            IID_PPV_ARGS(&folder)))) {
    dialog->SetFolder(folder);
    folder->Release();
  }
}

void choose_source(AppState* state) {
  IFileOpenDialog* dialog = nullptr;
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog));
  if (FAILED(result)) {
    MessageBoxW(state->window, L"The Windows file picker could not be created.",
                L"HDR Bridge", MB_OK | MB_ICONERROR);
    return;
  }
  constexpr COMDLG_FILTERSPEC filters[] = {
      {L"All supported HDR images", L"*.avif;*.heif;*.heic;*.hif;*.jpg;*.jpeg;*.jpe;*.jxl;*.jxr;*.wdp;*.hdp;*.png;*.tif;*.tiff"},
      {L"AVIF (*.avif)", L"*.avif"},
      {L"HEIF (*.heif;*.heic;*.hif)", L"*.heif;*.heic;*.hif"},
      {L"JPEG (*.jpg;*.jpeg;*.jpe)", L"*.jpg;*.jpeg;*.jpe"},
      {L"JPEG XL (*.jxl)", L"*.jxl"},
      {L"JPEG XR (*.jxr;*.wdp;*.hdp)", L"*.jxr;*.wdp;*.hdp"},
      {L"PNG (*.png)", L"*.png"},
      {L"TIFF (*.tif;*.tiff)", L"*.tif;*.tiff"},
      {L"All files (*.*)", L"*.*"}};
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST |
                     FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
  dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
  dialog->SetFileTypeIndex(1);
  dialog->SetTitle(L"Add HDR images");
  set_dialog_initial_folder(dialog, state->input.empty()
      ? state->selected_folder : state->input.parent_path());
  result = dialog->Show(state->window);
  if (SUCCEEDED(result)) {
    IShellItemArray* results = nullptr;
    if (SUCCEEDED(dialog->GetResults(&results)) && results) {
      DWORD count = 0;
      results->GetCount(&count);
      std::vector<std::filesystem::path> paths;
      paths.reserve(count);
      for (DWORD index = 0; index < count; ++index) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(results->GetItemAt(index, &item)) && item) {
          auto path = shell_item_path(item);
          if (!path.empty()) paths.push_back(std::move(path));
          item->Release();
        }
      }
      results->Release();
      add_sources(state, paths);
    }
  } else if (result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    MessageBoxW(state->window, L"The Windows file picker failed.",
                L"HDR Bridge", MB_OK | MB_ICONERROR);
  }
  dialog->Release();
}

void choose_output_folder(AppState* state) {
  IFileOpenDialog* dialog = nullptr;
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog));
  if (FAILED(result)) {
    MessageBoxW(state->window, L"The Windows folder picker could not be created.",
                L"HDR Bridge", MB_OK | MB_ICONERROR);
    return;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(L"Choose output folder");
  const auto initial = !state->selected_folder.empty() ? state->selected_folder :
      (!state->input.empty() ? state->input.parent_path() : std::filesystem::path{});
  set_dialog_initial_folder(dialog, initial);
  result = dialog->Show(state->window);
  if (SUCCEEDED(result)) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item) {
      const auto path = shell_item_path(item);
      item->Release();
      if (!path.empty()) {
        state->selected_folder = path;
        state->use_source_folder = false;
        SetWindowTextW(state->folder_path, path.c_str());
        SendMessageW(state->source_folder, BM_SETCHECK, BST_UNCHECKED, 0);
        update_output_preview(state);
      }
    }
  } else if (result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    MessageBoxW(state->window, L"The Windows folder picker failed.",
                L"HDR Bridge", MB_OK | MB_ICONERROR);
  }
  dialog->Release();
}

void remove_selected_source(AppState* state) {
  if (state->running) return;
  const int selection_count = static_cast<int>(SendMessageW(state->queue, LB_GETSELCOUNT, 0, 0));
  if (selection_count <= 0) return;
  std::vector<int> selected(static_cast<size_t>(selection_count));
  if (SendMessageW(state->queue, LB_GETSELITEMS, selection_count,
                   reinterpret_cast<LPARAM>(selected.data())) == LB_ERR) {
    return;
  }
  const int first_removed = selected.front();
  for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
    const int index = *it;
    if (index < 0 || static_cast<size_t>(index) >= state->inputs.size()) continue;
    state->inputs.erase(state->inputs.begin() + index);
    if (static_cast<size_t>(index) < state->input_infos.size()) {
      state->input_infos.erase(state->input_infos.begin() + index);
    }
    if (static_cast<size_t>(index) < state->input_statuses.size()) {
      state->input_statuses.erase(state->input_statuses.begin() + index);
    }
    SendMessageW(state->queue, LB_DELETESTRING, index, 0);
  }
  if (state->inputs.empty()) {
    state->input.clear();
    state->output.clear();
    SetWindowTextW(state->source_path, L"Drop or add supported HDR still images");
    SetWindowTextW(state->inspector, L"SOURCE INSPECTOR\r\n\r\nNo image selected");
    SetWindowTextW(state->output_path, L"No output yet");
    SetWindowTextW(state->status, L"Ready");
    EnableWindow(state->convert, FALSE);
    EnableWindow(state->remove, FALSE);
    EnableWindow(state->clear, FALSE);
    return;
  }
  const int next = std::min(first_removed, static_cast<int>(state->inputs.size()) - 1);
  SendMessageW(state->queue, LB_SETSEL, TRUE, next);
  SendMessageW(state->queue, LB_SETCARETINDEX, next, FALSE);
  load_source(state, state->inputs[static_cast<size_t>(next)]);
  SetWindowTextW(state->convert, state->inputs.size() > 1u
      ? L"Convert all" : L"Convert");
}

void clear_sources(AppState* state) {
  if (state->running) return;
  state->inputs.clear();
  state->input_infos.clear();
  state->input_statuses.clear();
  state->input.clear();
  state->output.clear();
  SendMessageW(state->queue, LB_RESETCONTENT, 0, 0);
  SetWindowTextW(state->source_path, L"Drop or add supported HDR still images");
  SetWindowTextW(state->inspector, L"SOURCE INSPECTOR\r\n\r\nNo image selected");
  SetWindowTextW(state->output_path, L"No output yet");
  SetWindowTextW(state->status, L"Ready");
  SetWindowTextW(state->convert, L"Convert");
  EnableWindow(state->convert, FALSE);
  EnableWindow(state->remove, FALSE);
  EnableWindow(state->clear, FALSE);
}

void set_running(AppState* state, bool running) {
  state->running = running;
  EnableWindow(state->open, !running);
  EnableWindow(state->format, !running);
  EnableWindow(state->gamut, !running);
  EnableWindow(state->transfer, !running);
  EnableWindow(state->gainmap_resolution, !running);
  EnableWindow(state->gainmap_channels, !running);
  EnableWindow(state->peak, !running);
  EnableWindow(state->source_folder, !running);
  EnableWindow(state->choose_folder, !running);
  EnableWindow(state->base_name, !running);
  EnableWindow(state->name_suffix, !running);
  EnableWindow(state->collision, !running);
  // Keep the queue scrollable/selectable while work runs; mutation controls
  // remain disabled, so Delete cannot alter worker indices.
  EnableWindow(state->queue, TRUE);
  EnableWindow(state->remove, !running && !state->inputs.empty());
  EnableWindow(state->clear, !running && !state->inputs.empty());
  EnableWindow(state->convert, !running && !state->inputs.empty());
  EnableWindow(state->cancel_button, running);
  if (!running) update_mode_ui(state);
}

void start_conversion(AppState* state) {
  if (state->running || state->inputs.empty()) return;
  const int index = static_cast<int>(SendMessageW(state->format, CB_GETCURSEL, 0, 0));
  const std::wstring mode_w = mode_for_index(index);
  update_output_preview(state);
  if (state->output.empty()) return;
  EnableWindow(state->reveal, FALSE);
  EnableWindow(state->open_output, FALSE);
  state->cancel.store(false);
  for (size_t item = 0; item < state->inputs.size(); ++item) {
    if (item < state->input_statuses.size()) state->input_statuses[item] = {};
    replace_queue_summary(state->queue, static_cast<int>(item),
                          state->input_infos[item]
                              ? queue_summary(state->inputs[item], *state->input_infos[item])
                              : pending_queue_summary(state->inputs[item]));
  }
  InvalidateRect(state->queue, nullptr, TRUE);
  KillTimer(state->window, kProgressResetTimer);
  ShowWindow(state->progress, SW_SHOW);
  SendMessageW(state->progress, PBM_SETPOS, 0, 0);
  SetWindowTextW(state->status, L"Converting…");
  append_log(state, L"Starting " + mode_w + L" → " + state->output.wstring());
  set_running(state, true);

  const auto inputs = state->inputs;
  const HWND window = state->window;
  hdrbridge::ConversionOptions options;
  options.mode = narrow(mode_w);
  options.lossless = SendMessageW(state->lossless, BM_GETCHECK, 0, 0) == BST_CHECKED;
  options.copy_exif = SendMessageW(state->copy_exif, BM_GETCHECK, 0, 0) == BST_CHECKED;
  options.copy_xmp = SendMessageW(state->copy_xmp, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const int quality = static_cast<int>(SendMessageW(state->quality, TBM_GETPOS, 0, 0));
  options.image_quality = quality / 100.0f;
  options.base_quality = quality;
  options.gainmap_quality = std::max(50, quality - 5);
  const int gainmap_resolution_index = static_cast<int>(
      SendMessageW(state->gainmap_resolution, CB_GETCURSEL, 0, 0));
  constexpr int gainmap_scales[] = {4, 2, 1};
  options.gainmap_scale = gainmap_scales[
      std::clamp(gainmap_resolution_index, 0, 2)];
  options.multi_channel_gainmap =
      SendMessageW(state->gainmap_channels, CB_GETCURSEL, 0, 0) == 1;
  const int gamut_index = static_cast<int>(SendMessageW(state->gamut, CB_GETCURSEL, 0, 0));
  options.output_gamut = gamut_index == 1 ? "p3" : "rec2020";
  const int transfer_index = static_cast<int>(SendMessageW(state->transfer, CB_GETCURSEL, 0, 0));
  const bool transfer_output = index == 1 || index == 2 || index == 4;
  options.output_transfer = transfer_output && transfer_index == 1 ? "hlg" : "pq";
  if (options.output_transfer == "hlg") options.output_gamut = "rec2020";
  const int peak_index = static_cast<int>(SendMessageW(state->peak, CB_GETCURSEL, 0, 0));
  constexpr float peaks[] = {0.0f, 1000.0f, 2000.0f, 4000.0f, 10000.0f};
  options.target_peak_nits = peaks[std::clamp(peak_index, 0, 4)];
  options.overwrite = SendMessageW(state->collision, CB_GETCURSEL, 0, 0) == 1;
  const bool use_source_folder = state->use_source_folder;
  const auto selected_folder = state->selected_folder;
  const auto edited_base = control_text(state->base_name);
  const auto suffix = control_text(state->name_suffix);
  const auto collision = options.overwrite
      ? hdrbridge::desktop::CollisionPolicy::overwrite
      : hdrbridge::desktop::CollisionPolicy::auto_number;
  std::thread([state, window, inputs, options, mode_w, use_source_folder,
               selected_folder, edited_base, suffix, collision] {
    try {
      std::vector<hdrbridge::ConversionResult> results;
      results.reserve(inputs.size());
      size_t succeeded = 0;
      size_t skipped = 0;
      size_t failed = 0;
      for (size_t index = 0; index < inputs.size(); ++index) {
        if (state->cancel.load()) throw std::runtime_error("conversion cancelled");
        try {
          const hdrbridge::SourceInfo info = hdrbridge::inspect(inputs[index]);
          if (info.asset_kind != "direct-hdr" && info.asset_kind != "gain-map-hdr") {
            ++skipped;
            PostMessageW(window, WM_APP_ITEM_STATUS, 0,
                reinterpret_cast<LPARAM>(new ItemStatusPayload{
                    index, QueueStatus::skipped,
                    queue_result_summary(inputs[index], QueueStatus::skipped),
                    L"No HDR gain map or direct HDR signal"}));
            const int overall = static_cast<int>((index + 1u) * 100u / inputs.size());
            PostMessageW(window, WM_APP_PROGRESS, static_cast<WPARAM>(overall),
                reinterpret_cast<LPARAM>(new std::wstring(
                    L"[" + std::to_wstring(index + 1u) + L"/" +
                    std::to_wstring(inputs.size()) + L"] Skipped \u2014 No HDR data")));
            continue;
          }
          hdrbridge::desktop::NamingRequest request;
          request.source = inputs[index];
          request.selected_folder = selected_folder;
          request.use_source_folder = use_source_folder;
          request.base_name = inputs.size() == 1u ? edited_base : inputs[index].stem().wstring();
          request.suffix = suffix;
          request.mode = mode_w;
          request.collision = collision;
          const auto output = hdrbridge::desktop::resolve_output_path(request);
          auto result = hdrbridge::convert(inputs[index], output, options,
            [window, index, count = inputs.size()](int value, const std::string& stage) {
              const int overall = static_cast<int>((index * 100u + static_cast<size_t>(value)) / count);
              auto text = L"[" + std::to_wstring(index + 1u) + L"/" + std::to_wstring(count) +
                          L"] " + widen(stage);
              PostMessageW(window, WM_APP_PROGRESS, static_cast<WPARAM>(overall),
                           reinterpret_cast<LPARAM>(new std::wstring(std::move(text))));
            }, &state->cancel);
          results.push_back(std::move(result));
          ++succeeded;
          PostMessageW(window, WM_APP_ITEM_STATUS, 0,
              reinterpret_cast<LPARAM>(new ItemStatusPayload{
                  index, QueueStatus::success,
                  queue_result_summary(inputs[index], QueueStatus::success), L""}));
        } catch (const std::exception& item_error) {
          if (state->cancel.load()) throw;
          ++failed;
          std::wstring reason = widen(item_error.what());
          if (reason.size() > 180u) reason.resize(180u);
          PostMessageW(window, WM_APP_ITEM_STATUS, 0,
              reinterpret_cast<LPARAM>(new ItemStatusPayload{
                  index, QueueStatus::failed,
                  queue_result_summary(inputs[index], QueueStatus::failed),
                  std::move(reason)}));
          const int overall = static_cast<int>((index + 1u) * 100u / inputs.size());
          PostMessageW(window, WM_APP_PROGRESS, static_cast<WPARAM>(overall),
              reinterpret_cast<LPARAM>(new std::wstring(
                  L"[" + std::to_wstring(index + 1u) + L"/" +
                  std::to_wstring(inputs.size()) + L"] Failed \u2014 Decode error")));
        }
      }
      PostMessageW(window, WM_APP_SUCCESS, 0,
                   reinterpret_cast<LPARAM>(new SuccessPayload{
                       std::move(results), succeeded, skipped, failed}));
    } catch (const std::exception& e) {
      PostMessageW(window, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(new std::wstring(widen(e.what()))));
    }
  }).detach();
}

HWND add_control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
  HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
  return control;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_CREATE) {
    auto owned = std::make_unique<AppState>();
    owned->window = window;
    owned->title = add_control(window, L"STATIC", L"HDR Bridge", SS_LEFT, IDC_TITLE);
    owned->reset_layout = add_control(window, L"BUTTON", L"Reset layout", BS_PUSHBUTTON, IDC_RESET_LAYOUT);
    owned->vertical_splitter = add_control(window, L"STATIC", L"", SS_NOTIFY, IDC_VERTICAL_SPLITTER);
    owned->horizontal_splitter = add_control(window, L"STATIC", L"", SS_NOTIFY, IDC_HORIZONTAL_SPLITTER);
    owned->queue_label = add_control(window, L"STATIC", L"FILES", SS_LEFT, IDC_QUEUE_LABEL);
    owned->output_label = add_control(window, L"STATIC", L"OUTPUT", SS_LEFT, IDC_OUTPUT_LABEL);
    owned->inspector_label = add_control(window, L"STATIC", L"SOURCE INSPECTOR", SS_LEFT, IDC_INSPECTOR_LABEL);
    owned->activity_label = add_control(window, L"STATIC", L"ACTIVITY", SS_LEFT, IDC_ACTIVITY_LABEL);
    owned->open = add_control(window, L"BUTTON", L"Add files", BS_PUSHBUTTON, IDC_OPEN);
    owned->source_path = add_control(window, L"STATIC", L"Drop or add supported HDR still images", SS_LEFT | SS_PATHELLIPSIS, IDC_SOURCE_PATH);
    owned->queue = add_control(window, L"LISTBOX", L"", LBS_NOTIFY | LBS_EXTENDEDSEL |
                               LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_BORDER |
                               WS_VSCROLL | LBS_NOINTEGRALHEIGHT, IDC_QUEUE);
    SendMessageW(owned->queue, LB_SETITEMHEIGHT, 0,
                 MulDiv(26, GetDpiForWindow(window), 96));
    owned->remove = add_control(window, L"BUTTON", L"Remove", BS_PUSHBUTTON, IDC_REMOVE);
    owned->clear = add_control(window, L"BUTTON", L"Clear", BS_PUSHBUTTON, IDC_CLEAR);
    owned->inspector = add_control(window, L"EDIT", L"No file selected", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_BORDER, IDC_INSPECTOR);
    owned->format = add_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_FORMAT);
    for (const wchar_t* item : {L"Share — Ultra HDR JPEG",
                                L"Video — HDR PNG RGB16",
                                L"Edit / Master — JPEG XL RGB16",
                                L"Edit / Master — JPEG XR FP16 scRGB",
                                L"Compact — Direct HDR AVIF 10-bit",
                                L"Advanced — Direct HDR TIFF RGB16 PQ",
                                L"Advanced — JPEG XR RGB10 (Experimental)"}) {
      SendMessageW(owned->format, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(owned->format, CB_SETCURSEL, 0, 0);
    owned->format_note = add_control(window, L"STATIC", L"", SS_LEFT, IDC_FORMAT_NOTE);
    owned->gamut = add_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_GAMUT);
    SendMessageW(owned->gamut, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Rec.2020 / PQ — Video default"));
    SendMessageW(owned->gamut, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Display P3 / PQ"));
    SendMessageW(owned->gamut, CB_SETCURSEL, 0, 0);
    owned->transfer = add_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_TRANSFER);
    SendMessageW(owned->transfer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Transfer: PQ / ST2084 — default"));
    SendMessageW(owned->transfer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Transfer: HLG / BT.2100"));
    SendMessageW(owned->transfer, CB_SETCURSEL, 0, 0);
    owned->gainmap_resolution = add_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_GAINMAP_RESOLUTION);
    SendMessageW(owned->gainmap_resolution, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Gain map: 1/4"));
    SendMessageW(owned->gainmap_resolution, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Gain map: 1/2 — default"));
    SendMessageW(owned->gainmap_resolution, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Gain map: Full resolution"));
    SendMessageW(owned->gainmap_resolution, CB_SETCURSEL, 1, 0);
    owned->gainmap_channels = add_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_GAINMAP_CHANNELS);
    SendMessageW(owned->gainmap_channels, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Channels: Mono — default"));
    SendMessageW(owned->gainmap_channels, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Channels: RGB"));
    SendMessageW(owned->gainmap_channels, CB_SETCURSEL, 0, 0);
    owned->peak = add_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_PEAK);
    for (const wchar_t* item : {L"Peak: Faithful / Auto", L"Peak: 1000 nits",
                                L"Peak: 2000 nits", L"Peak: 4000 nits", L"Peak: 10000 nits"}) {
      SendMessageW(owned->peak, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(owned->peak, CB_SETCURSEL, 0, 0);
    owned->lossless = add_control(window, L"BUTTON", L"Lossless", BS_AUTOCHECKBOX, IDC_LOSSLESS);
    owned->copy_exif = add_control(window, L"BUTTON", L"Exif", BS_AUTOCHECKBOX, IDC_COPY_EXIF);
    owned->copy_xmp = add_control(window, L"BUTTON", L"XMP", BS_AUTOCHECKBOX, IDC_COPY_XMP);
    SendMessageW(owned->lossless, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(owned->copy_exif, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(owned->copy_xmp, BM_SETCHECK, BST_CHECKED, 0);
    owned->quality = add_control(window, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS, IDC_QUALITY);
    SendMessageW(owned->quality, TBM_SETRANGE, TRUE, MAKELPARAM(50, 100));
    SendMessageW(owned->quality, TBM_SETPOS, TRUE, 95);
    owned->quality_value = add_control(window, L"STATIC", L"95%", SS_CENTER, IDC_QUALITY_VALUE);
    owned->source_folder = add_control(window, L"BUTTON", L"Source folder", BS_AUTOCHECKBOX, IDC_SOURCE_FOLDER);
    SendMessageW(owned->source_folder, BM_SETCHECK, BST_CHECKED, 0);
    owned->choose_folder = add_control(window, L"BUTTON", L"Choose...", BS_PUSHBUTTON, IDC_CHOOSE_FOLDER);
    owned->folder_path = add_control(window, L"STATIC", L"Source folder", SS_LEFT | SS_PATHELLIPSIS, IDC_FOLDER_PATH);
    owned->base_name = add_control(window, L"EDIT", L"Base name", ES_AUTOHSCROLL | WS_BORDER, IDC_BASE_NAME);
    owned->name_suffix = add_control(window, L"EDIT", L"_ultrahdr", ES_AUTOHSCROLL | WS_BORDER, IDC_NAME_SUFFIX);
    owned->collision = add_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_COLLISION);
    SendMessageW(owned->collision, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"If file exists: auto-number"));
    SendMessageW(owned->collision, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"If file exists: overwrite"));
    SendMessageW(owned->collision, CB_SETCURSEL, 0, 0);
    owned->convert = add_control(window, L"BUTTON", L"Convert", BS_DEFPUSHBUTTON, IDC_CONVERT);
    owned->cancel_button = add_control(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON, IDC_CANCEL);
    owned->reveal = add_control(window, L"BUTTON", L"Reveal", BS_PUSHBUTTON, IDC_REVEAL);
    owned->open_output = add_control(window, L"BUTTON", L"Open", BS_PUSHBUTTON, IDC_OPEN_OUTPUT);
    owned->progress = add_control(window, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROGRESS);
    SendMessageW(owned->progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    owned->status = add_control(window, L"STATIC", L"Ready", SS_LEFT, IDC_STATUS);
    owned->output_path = add_control(window, L"STATIC", L"No output yet", SS_LEFT | SS_PATHELLIPSIS, IDC_OUTPUT_PATH);
    owned->log = add_control(window, L"EDIT", L"Ready. Detailed conversion activity appears here.",
                             ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_BORDER, IDC_LOG);
    EnableWindow(owned->convert, FALSE);
    EnableWindow(owned->remove, FALSE);
    EnableWindow(owned->clear, FALSE);
    EnableWindow(owned->cancel_button, FALSE);
    EnableWindow(owned->reveal, FALSE);
    EnableWindow(owned->open_output, FALSE);
    ShowWindow(owned->progress, SW_HIDE);
    SetWindowSubclass(owned->queue, queue_subclass, 1, 0);
    SetWindowSubclass(owned->vertical_splitter, splitter_subclass, 1, 1);
    SetWindowSubclass(owned->horizontal_splitter, splitter_subclass, 1, 2);
    DragAcceptFiles(window, TRUE);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned.release()));
    state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    load_split_layout(state);
    apply_fonts(state);
    apply_theme(state);
    update_mode_ui(state);
    return 0;
  }
  if (!state) return DefWindowProcW(window, message, wparam, lparam);
  switch (message) {
    case WM_MEASUREITEM: {
      auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
      if (measure && measure->CtlID == IDC_QUEUE) {
        measure->itemHeight = static_cast<UINT>(MulDiv(26, GetDpiForWindow(window), 96));
        return TRUE;
      }
      break;
    }
    case WM_DRAWITEM: {
      auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (!draw || draw->CtlID != IDC_QUEUE || draw->itemID == static_cast<UINT>(-1)) break;
      const size_t index = static_cast<size_t>(draw->itemID);
      QueueStatus status = QueueStatus::pending;
      if (index < state->input_statuses.size()) status = state->input_statuses[index].status;
      const bool selected = (draw->itemState & ODS_SELECTED) != 0;
      const bool error = status == QueueStatus::skipped || status == QueueStatus::failed;
      COLORREF background = state->surface_color;
      COLORREF foreground = state->text_color;
      if (selected) {
        background = error
            ? (state->dark ? RGB(91, 43, 47) : RGB(255, 225, 226))
            : (state->dark ? RGB(48, 63, 78) : RGB(219, 232, 245));
      }
      if (error) foreground = state->dark ? RGB(255, 145, 150) : RGB(178, 34, 43);
      HBRUSH brush = CreateSolidBrush(background);
      FillRect(draw->hDC, &draw->rcItem, brush);
      DeleteObject(brush);
      const int length = static_cast<int>(SendMessageW(draw->hwndItem, LB_GETTEXTLEN,
                                                        draw->itemID, 0));
      std::wstring text(static_cast<size_t>(std::max(length, 0)) + 1u, L'\0');
      if (length >= 0) SendMessageW(draw->hwndItem, LB_GETTEXT, draw->itemID,
                                    reinterpret_cast<LPARAM>(text.data()));
      text.resize(static_cast<size_t>(std::max(length, 0)));
      SetBkMode(draw->hDC, TRANSPARENT);
      SetTextColor(draw->hDC, foreground);
      if (state->font) SelectObject(draw->hDC, state->font);
      RECT text_rect = draw->rcItem;
      text_rect.left += MulDiv(9, GetDpiForWindow(window), 96);
      text_rect.right -= MulDiv(7, GetDpiForWindow(window), 96);
      DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &text_rect,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
      if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC, &draw->rcItem);
      return TRUE;
    }
    case WM_SIZE:
      layout(state, LOWORD(lparam), HIWORD(lparam));
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize = {980, 760};
      return 0;
    }
    case WM_DROPFILES: {
      const auto drop = reinterpret_cast<HDROP>(wparam);
      const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
      std::vector<std::filesystem::path> paths;
      paths.reserve(count);
      for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(static_cast<size_t>(length) + 1u, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1u);
        path.resize(length);
        paths.emplace_back(std::move(path));
      }
      DragFinish(drop);
      add_sources(state, paths);
      return 0;
    }
    case WM_HSCROLL:
      if (reinterpret_cast<HWND>(lparam) == state->quality) {
        const int value = static_cast<int>(SendMessageW(state->quality, TBM_GETPOS, 0, 0));
        SetWindowTextW(state->quality_value, (std::to_wstring(value) + L"%").c_str());
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case IDC_OPEN: choose_source(state); break;
        case IDC_QUEUE:
          if (HIWORD(wparam) == LBN_SELCHANGE) {
            const int selected = static_cast<int>(SendMessageW(state->queue, LB_GETCARETINDEX, 0, 0));
            if (selected != LB_ERR && static_cast<size_t>(selected) < state->inputs.size()) {
              load_source(state, state->inputs[static_cast<size_t>(selected)]);
            }
          } else if (HIWORD(wparam) == LBN_DBLCLK) {
            const int selected = static_cast<int>(SendMessageW(state->queue, LB_GETCARETINDEX, 0, 0));
            if (selected != LB_ERR && static_cast<size_t>(selected) < state->inputs.size()) {
              ShellExecuteW(window, L"open", state->inputs[static_cast<size_t>(selected)].c_str(),
                            nullptr, nullptr, SW_SHOWNORMAL);
            }
          }
          break;
        case IDC_REMOVE: remove_selected_source(state); break;
        case IDC_CLEAR: clear_sources(state); break;
        case IDC_RESET_LAYOUT: {
          state->right_panel_width = 560;
          state->activity_panel_height = 230;
          RECT rect{};
          GetClientRect(window, &rect);
          layout(state, rect.right, rect.bottom);
          save_layout(state);
          break;
        }
        case IDC_FORMAT: if (HIWORD(wparam) == CBN_SELCHANGE) update_mode_ui(state); break;
        case IDC_TRANSFER:
        case IDC_GAMUT:
          if (HIWORD(wparam) == CBN_SELCHANGE) update_mode_ui(state);
          break;
        case IDC_LOSSLESS: update_mode_ui(state); break;
        case IDC_SOURCE_FOLDER:
          state->use_source_folder =
              SendMessageW(state->source_folder, BM_GETCHECK, 0, 0) == BST_CHECKED;
          if (state->use_source_folder && !state->input.empty()) {
            SetWindowTextW(state->folder_path, state->input.parent_path().c_str());
          } else if (!state->use_source_folder && !state->selected_folder.empty()) {
            SetWindowTextW(state->folder_path, state->selected_folder.c_str());
          }
          update_output_preview(state);
          break;
        case IDC_CHOOSE_FOLDER: choose_output_folder(state); break;
        case IDC_BASE_NAME:
        case IDC_NAME_SUFFIX:
          if (HIWORD(wparam) == EN_CHANGE) update_output_preview(state);
          break;
        case IDC_COLLISION:
          if (HIWORD(wparam) == CBN_SELCHANGE) update_output_preview(state);
          break;
        case IDC_CONVERT: start_conversion(state); break;
        case IDC_CANCEL:
          state->cancel.store(true);
          SetWindowTextW(state->status, L"Cancelling…");
          append_log(state, L"Cancellation requested");
          break;
        case IDC_REVEAL:
          if (!state->output.empty()) {
            const std::wstring args = L"/select,\"" + state->output.wstring() + L"\"";
            ShellExecuteW(window, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
          }
          break;
        case IDC_OPEN_OUTPUT:
          if (!state->output.empty()) ShellExecuteW(window, L"open", state->output.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          break;
      }
      return 0;
    case WM_APP_PROGRESS: {
      std::unique_ptr<std::wstring> stage(reinterpret_cast<std::wstring*>(lparam));
      ShowWindow(state->progress, SW_SHOW);
      SendMessageW(state->progress, PBM_SETPOS, wparam, 0);
      SetWindowTextW(state->status, stage->c_str());
      append_log(state, std::to_wstring(wparam) + L"%  " + *stage);
      return 0;
    }
    case WM_APP_ITEM_STATUS: {
      std::unique_ptr<ItemStatusPayload> item(
          reinterpret_cast<ItemStatusPayload*>(lparam));
      if (item->index < state->input_statuses.size()) {
        state->input_statuses[item->index] = {item->status, item->reason};
        replace_queue_summary(state->queue, static_cast<int>(item->index), item->label);
        InvalidateRect(state->queue, nullptr, TRUE);
      }
      if (item->status == QueueStatus::skipped) {
        append_log(state, L"SKIPPED  " + state->inputs[item->index].filename().wstring() +
                          L" \u2014 " + item->reason);
      } else if (item->status == QueueStatus::failed) {
        append_log(state, L"FAILED  " + state->inputs[item->index].filename().wstring() +
                          L" \u2014 " + item->reason);
      }
      return 0;
    }
    case WM_APP_SUCCESS: {
      std::unique_ptr<SuccessPayload> payload(reinterpret_cast<SuccessPayload*>(lparam));
      set_running(state, false);
      SendMessageW(state->progress, PBM_SETPOS, 100, 0);
      SetTimer(window, kProgressResetTimer, 850, nullptr);
      const std::wstring summary =
          L"Complete \u2014 " + std::to_wstring(payload->succeeded) + L" succeeded / " +
          std::to_wstring(payload->skipped) + L" skipped / " +
          std::to_wstring(payload->failed) + L" failed";
      SetWindowTextW(state->status, summary.c_str());
      append_log(state, L"BATCH SUMMARY  " + std::to_wstring(payload->succeeded) +
                        L" succeeded / " + std::to_wstring(payload->skipped) +
                        L" skipped / " + std::to_wstring(payload->failed) + L" failed");
      if (payload->results.empty()) {
        state->output.clear();
        SetWindowTextW(state->output_path, L"No output generated");
        return 0;
      }
      state->output = payload->results.back().output_path;
      EnableWindow(state->reveal, TRUE);
      EnableWindow(state->open_output, TRUE);
      for (size_t index = 0; index < payload->results.size(); ++index) {
        const auto& item = payload->results[index];
        append_log(state, L"BATCH PASS [" + std::to_wstring(index + 1u) + L"/" +
                          std::to_wstring(payload->results.size()) + L"] " +
                          item.output_path.wstring() + L" • SHA-256 " + widen(item.sha256));
      }
      const auto& result = payload->results.back();
      const auto& v = result.verification;
      std::wostringstream line;
      line << L"PASS  " << v.width << L"×" << v.height << L"  " << widen(v.pixel_format)
           << L"  •  " << result.output_bytes << L" bytes  •  SHA-256 " << widen(result.sha256);
      append_log(state, line.str());
      if (v.max_channel_nits > 0.0) {
        std::wostringstream diagnostics;
        diagnostics.setf(std::ios::fixed); diagnostics.precision(2);
        diagnostics << L"HDR NUMBERS  max channel " << v.max_channel_nits << L" nit  •  max luminance "
                    << v.max_luminance_nits << L" nit  •  P99.9 " << v.percentile_99_9_nits
                    << L" nit  •  P99.99 " << v.percentile_99_99_nits << L" nit  •  chosen peak "
                    << v.chosen_target_peak_nits << L" nit";
        append_log(state, diagnostics.str());
        append_log(state, L"PEAK REASON  " + widen(v.peak_choice_reason));
      }
      if (v.hdr_capacity_max > 0.0 || v.reconstruction_rmse > 0.0) {
        std::wostringstream reconstruction;
        reconstruction.setf(std::ios::fixed); reconstruction.precision(6);
        reconstruction << L"GAIN MAP  capacity " << v.hdr_capacity_max << L"×  •  linear RMSE "
                       << v.reconstruction_rmse << L"  •  max abs error " << v.reconstruction_max_abs_error;
        append_log(state, reconstruction.str());
      }
      const auto& timing = result.timings;
      std::wostringstream performance;
      performance.setf(std::ios::fixed); performance.precision(1);
      performance << L"TIMING ms  decode " << timing.decode_ms
                  << L" • orientation " << timing.orientation_ms
                  << L" • color " << timing.color_conversion_ms
                  << L" • gain map " << timing.gain_map_ms
                  << L" • encode " << timing.encode_ms
                  << L" • verify " << timing.verification_ms
                  << L" • total " << timing.total_ms
                  << (timing.canonical_cache_hit ? L" • canonical cache HIT" : L" • canonical cache MISS");
      append_log(state, performance.str());
      return 0;
    }
    case WM_APP_ERROR: {
      std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lparam));
      set_running(state, false);
      const bool cancelled = state->cancel.load();
      KillTimer(window, kProgressResetTimer);
      SendMessageW(state->progress, PBM_SETPOS, 0, 0);
      ShowWindow(state->progress, SW_HIDE);
      SetWindowTextW(state->status, cancelled ? L"Cancelled" : L"Conversion failed");
      append_log(state, (cancelled ? L"CANCELLED  " : L"ERROR  ") + *error);
      if (!cancelled) MessageBoxW(window, error->c_str(), L"HDR Bridge conversion error", MB_OK | MB_ICONERROR);
      return 0;
    }
    case WM_TIMER:
      if (wparam == kProgressResetTimer) {
        KillTimer(window, kProgressResetTimer);
        SendMessageW(state->progress, PBM_SETPOS, 0, 0);
        ShowWindow(state->progress, SW_HIDE);
        return 0;
      }
      break;
    case WM_SETTINGCHANGE:
      apply_theme(state);
      break;
    case WM_APP_SPLITTER_MOVE: {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      RECT rect{};
      GetClientRect(window, &rect);
      if (wparam == 1) {
        state->right_panel_width = rect.right - 24 - point.x - 12;
      } else {
        state->activity_panel_height = rect.bottom - point.y;
      }
      layout(state, rect.right, rect.bottom);
      return 0;
    }
    case WM_APP_SPLITTER_END:
      save_layout(state);
      return 0;
    case WM_APP_OPEN_SOURCE: {
      std::unique_ptr<std::filesystem::path> path(reinterpret_cast<std::filesystem::path*>(lparam));
      add_sources(state, {*path});
      return 0;
    }
    case WM_CLOSE:
      if (state->running) {
        state->cancel.store(true);
        MessageBoxW(window, L"Cancellation requested. Close HDR Bridge after the conversion worker stops.", L"HDR Bridge", MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      save_layout(state);
      RemoveWindowSubclass(state->queue, queue_subclass, 1);
      RemoveWindowSubclass(state->vertical_splitter, splitter_subclass, 1);
      RemoveWindowSubclass(state->horizontal_splitter, splitter_subclass, 1);
      if (state->font) DeleteObject(state->font);
      if (state->title_font) DeleteObject(state->title_font);
      if (state->background_brush) DeleteObject(state->background_brush);
      if (state->surface_brush) DeleteObject(state->surface_brush);
      delete state;
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      PostQuitMessage(0);
      return 0;
    case WM_CTLCOLORSTATIC: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      const auto control = reinterpret_cast<HWND>(lparam);
      SetBkMode(dc, TRANSPARENT);
      if (control == state->vertical_splitter || control == state->horizontal_splitter) {
        return reinterpret_cast<LRESULT>(state->surface_brush);
      }
      const bool primary = control == state->title || control == state->queue_label ||
                           control == state->output_label ||
                           control == state->inspector_label ||
                           control == state->activity_label || control == state->status;
      SetTextColor(dc, primary ? state->text_color : state->muted_text_color);
      return reinterpret_cast<LRESULT>(state->background_brush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      SetBkColor(dc, state->surface_color);
      SetTextColor(dc, state->text_color);
      return reinterpret_cast<LRESULT>(state->surface_brush);
    }
    case WM_CTLCOLORBTN: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, state->text_color);
      return reinterpret_cast<LRESULT>(state->background_brush);
    }
    case WM_ERASEBKGND: {
      RECT rect{}; GetClientRect(window, &rect);
      FillRect(reinterpret_cast<HDC>(wparam), &rect, state->background_brush);
      return 1;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                      COINIT_DISABLE_OLE1DDE);
  if (FAILED(com_result)) return 1;
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
  InitCommonControlsEx(&controls);
  WNDCLASSEXW cls{sizeof(cls)};
  cls.style = CS_HREDRAW | CS_VREDRAW;
  cls.lpfnWndProc = window_proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  cls.lpszClassName = kWindowClass;
  cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  cls.hIconSm = cls.hIcon;
  if (!RegisterClassExW(&cls)) {
    CoUninitialize();
    return 1;
  }
  HWND window = CreateWindowExW(0, kWindowClass, L"HDR Bridge",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1320, 900,
      nullptr, nullptr, instance, nullptr);
  if (!window) {
    CoUninitialize();
    return 1;
  }
  restore_window_bounds(window);
  ShowWindow(window, show);
  UpdateWindow(window);
  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (arguments && argument_count > 1) {
    PostMessageW(window, WM_APP_OPEN_SOURCE, 0,
                 reinterpret_cast<LPARAM>(new std::filesystem::path(arguments[1])));
  }
  if (arguments) LocalFree(arguments);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  const int exit_code = static_cast<int>(message.wParam);
  CoUninitialize();
  return exit_code;
}
