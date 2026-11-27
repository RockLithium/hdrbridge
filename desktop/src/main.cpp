#include "hdrbridge_core.h"
#include "hdr_preview_tiles.h"
#include "output_naming.h"
#include "hdrbridge_version.h"
#include "app_update.h"
#include "ui_language.h"

#include <Windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>
#include <Richedit.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace ui = hdrbridge::desktop::i18n;

constexpr wchar_t kWindowClass[] = L"HDRBridgeMainWindow";
constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_SUCCESS = WM_APP + 2;
constexpr UINT WM_APP_ERROR = WM_APP + 3;
constexpr UINT WM_APP_OPEN_SOURCE = WM_APP + 4;
constexpr UINT WM_APP_SPLITTER_MOVE = WM_APP + 5;
constexpr UINT WM_APP_SPLITTER_END = WM_APP + 6;
constexpr UINT WM_APP_ITEM_STATUS = WM_APP + 7;
constexpr UINT WM_APP_RESULT = WM_APP + 10;
constexpr UINT WM_APP_UPDATE_CHECK = WM_APP + 11;
constexpr UINT WM_APP_UPDATE_DOWNLOAD = WM_APP + 12;
constexpr UINT WM_APP_TASK_PROGRESS = WM_APP + 8;
constexpr UINT WM_APP_TASK_OUTPUT = WM_APP + 9;
constexpr UINT WM_APP_TASK_REORDER = WM_APP + 20;
constexpr UINT WM_APP_PREVIEW_READY = WM_APP + 21;
constexpr UINT WM_APP_PREVIEW_ERROR = WM_APP + 22;
constexpr UINT WM_APP_QUEUE_SELECT_ALL = WM_APP + 23;
constexpr UINT_PTR kProgressResetTimer = 1;
constexpr UINT_PTR kUpdateCheckTimer = 2;
constexpr wchar_t kLayoutRegistryPath[] = L"Software\\HDR Bridge\\Desktop";
constexpr UINT ID_CONTEXT_OPEN = 4001;
constexpr UINT ID_CONTEXT_FOLDER = 4002;
constexpr UINT ID_CONTEXT_REMOVE = 4003;
constexpr UINT ID_TASK_CLEAR_STATUS = 4010;
constexpr UINT ID_TASK_DELETE = 4011;
constexpr UINT ID_TASK_OPEN_SOURCE = 4012;
constexpr UINT ID_TASK_SOURCE_FOLDER = 4013;
constexpr UINT ID_TASK_OPEN_OUTPUT = 4014;
constexpr UINT ID_TASK_OUTPUT_FOLDER = 4015;
constexpr UINT ID_TASK_EDIT = 4016;
constexpr UINT ID_TASK_REMOVE_COMPLETED = 4017;
constexpr UINT ID_TASK_REMOVE_ALL = 4018;
constexpr UINT ID_TASK_START = 4019;
constexpr UINT IDC_PREVIEW_ZOOM = 5001;
constexpr UINT IDC_PREVIEW_FULLSCREEN = 5002;

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
  IDC_PROGRESS = 113,
  IDC_STATUS,
  IDC_OUTPUT_PATH,
  IDC_LOG
  ,IDC_GAMUT
  ,IDC_TRANSFER
  ,IDC_REPRESENTATION
  ,IDC_GAINMAP_RESOLUTION
  ,IDC_GAINMAP_CHANNELS
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
  ,IDC_VERSION
  ,IDC_RESET_LAYOUT
  ,IDC_RESET_SETTINGS
  ,IDC_VERTICAL_SPLITTER
  ,IDC_HORIZONTAL_SPLITTER
  ,IDC_TASKS
  ,IDC_TASKS_LABEL
  ,IDC_USE_SUFFIX
  ,IDC_FILES_SPLITTER
  ,IDC_REMOVE_ALL_TASKS
  ,IDC_TIFF_COMPRESSION
  ,IDC_GAINMAP_EXPORT_FORMAT
  ,IDC_FRAME_EXPORT_FORMAT
  ,IDC_PREVIEW_TOGGLE
};

enum class QueueStatus { pending, running, success, skipped, failed };

class PreviewRenderer;

struct TaskOptions {
  hdrbridge::ConversionOptions conversion;
  std::wstring mode;
  bool use_source_folder = true;
  std::filesystem::path selected_folder;
  std::wstring base_name;
  std::wstring suffix;
  bool use_suffix = true;
  hdrbridge::desktop::CollisionPolicy collision =
      hdrbridge::desktop::CollisionPolicy::auto_number;
};

struct ConversionTask {
  std::filesystem::path source;
  std::filesystem::path output;
  TaskOptions options;
  QueueStatus status = QueueStatus::pending;
  int progress = 0;
  std::wstring reason;
};

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

struct TaskProgressPayload {
  size_t index = 0;
  int item_progress = 0;
  int overall_progress = 0;
  std::wstring stage;
};

struct TaskOutputPayload {
  size_t index = 0;
  std::filesystem::path output;
};

struct PreviewPayload {
  uint64_t generation = 0;
  hdrbridge::PreviewImage image;
  std::vector<hdrbridge::preview::MipLevel> mip_levels;
};

struct PreviewErrorPayload {
  uint64_t generation = 0;
  std::wstring error;
};

std::wstring widen(const std::string& input);

struct PreviewRequest {
  uint64_t generation = 0;
  std::filesystem::path path;
  std::shared_ptr<std::atomic_bool> cancel;
};

struct PreviewWorker {
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
  bool has_pending = false;
  PreviewRequest pending;
  std::shared_ptr<std::atomic_bool> active_cancel;
  std::thread thread;

  ~PreviewWorker() { shutdown(); }

  void submit(HWND window, uint64_t generation, std::filesystem::path path,
              std::shared_ptr<std::atomic_bool> cancel) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stop) return;
      if (!thread.joinable()) {
        thread = std::thread([this, window] { run(window); });
      }
      if (active_cancel) active_cancel->store(true);
      if (has_pending && pending.cancel) pending.cancel->store(true);
      pending = {generation, std::move(path), std::move(cancel)};
      has_pending = true;
    }
    cv.notify_one();
  }

  void cancel_all() {
    std::lock_guard<std::mutex> lock(mutex);
    if (active_cancel) active_cancel->store(true);
    if (has_pending && pending.cancel) pending.cancel->store(true);
    has_pending = false;
    pending = {};
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
      if (active_cancel) active_cancel->store(true);
      if (has_pending && pending.cancel) pending.cancel->store(true);
      has_pending = false;
      pending = {};
    }
    cv.notify_all();
    if (thread.joinable()) thread.join();
  }

  void run(HWND window) {
    for (;;) {
      PreviewRequest job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return stop || has_pending; });
        if (stop && !has_pending) break;
        job = std::move(pending);
        pending = {};
        has_pending = false;
        active_cancel = job.cancel;
      }
      auto clear_active = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        if (active_cancel == job.cancel) active_cancel.reset();
      };
      if (job.cancel && !job.cancel->load()) {
        try {
          auto image = hdrbridge::decode_preview(
              job.path, std::numeric_limits<uint32_t>::max(), {}, job.cancel.get());
          if (!job.cancel->load()) {
            std::vector<hdrbridge::preview::MipLevel> mip_levels;
            try {
              mip_levels = hdrbridge::preview::build_2x_mip_levels(
                  image.width, image.height, image.rgba_fp16, 64u, job.cancel.get());
            } catch (const std::exception&) {
              mip_levels.clear();
            }
            if (!job.cancel->load()) {
              auto payload = std::make_unique<PreviewPayload>(
                  PreviewPayload{job.generation, std::move(image), std::move(mip_levels)});
              if (PostMessageW(window, WM_APP_PREVIEW_READY, 0,
                               reinterpret_cast<LPARAM>(payload.get()))) {
                payload.release();
              }
            }
          }
        } catch (const std::exception& error) {
          if (!job.cancel->load()) {
            PostMessageW(window, WM_APP_PREVIEW_ERROR, 0,
                         reinterpret_cast<LPARAM>(new PreviewErrorPayload{
                             job.generation, widen(error.what())}));
          }
        }
      }
      clear_active();
    }
  }
};

struct SuccessPayload {
  std::vector<hdrbridge::ConversionResult> results;
  size_t succeeded = 0;
  size_t skipped = 0;
  size_t failed = 0;
};

struct UpdateCheckPayload {
  std::optional<hdrbridge::app_update::Release> release;
  std::string error;
  bool manual = false;
  bool from_network = false;
};
struct UpdateDownloadPayload {
  std::optional<hdrbridge::app_update::VerifiedInstaller> path;
  std::string error;
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
  HWND remove_all_tasks = nullptr;
  HWND progress = nullptr;
  HWND status = nullptr;
  HWND output_path = nullptr;
  HWND log = nullptr;
  std::wstring activity_text = HB_UI("Ready. Detailed conversion activity appears here.");
  std::wstring inspector_text = HB_UI("No file selected");
  std::wstring base_name_cue = HB_UI("Base name");
  HWND gamut = nullptr;
  HWND transfer = nullptr;
  HWND representation = nullptr;
  HWND gainmap_resolution = nullptr;
  HWND gainmap_channels = nullptr;
  HWND tiff_compression = nullptr;
  HWND gainmap_export_format = nullptr;
  HWND frame_export_format = nullptr;
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
  HWND version = nullptr;
  bool update_busy = false;
  bool update_manual_requested = false;
  bool update_prompt_open = false;
  std::optional<hdrbridge::app_update::VerifiedInstaller> ready_installer;
  std::optional<hdrbridge::app_update::Release> pending_release;
  std::string offered_update_version;
  HWND reset_layout = nullptr;
  HWND reset_settings = nullptr;
  HWND preview_toggle = nullptr;
  HWND vertical_splitter = nullptr;
  HWND horizontal_splitter = nullptr;
  HWND tasks = nullptr;
  HWND tasks_label = nullptr;
  HWND use_suffix = nullptr;
  HWND files_splitter = nullptr;
  HFONT font = nullptr;
  HFONT cjk_font = nullptr;
  HFONT small_font = nullptr;
  HFONT small_cjk_font = nullptr;
  HFONT title_font = nullptr;
  HFONT version_font = nullptr;
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
  std::vector<ConversionTask> conversion_tasks;
  int editing_task = -1;
  int drag_task = -1;
  std::atomic_bool cancel{false};
  bool running = false;
  bool dark = false;
  int right_panel_width = 620;
  int activity_panel_height = 230;
  int files_panel_height = 270;
  int last_format_index = -1;
  int last_transfer_index = -1;
  int last_representation_index = -1;
  int last_frame_export_index = -1;
  int last_encoding_slot = -1;
  int uhdr_gainmap_channels = 1;
  int normal_gainmap_resolution = 2, frame_gainmap_resolution = 2, frame_gainmap_channels = 1;
  bool normal_copy_exif = true, normal_copy_xmp = true;
  bool lossless_setting = true;
  bool tiff_compressed = true;
  std::array<int, 9> encoding_values{95, 4, 6, 90, 95, 95, 95, 95, 4};
  std::unique_ptr<PreviewRenderer> preview_renderer;
  PreviewWorker preview_worker;
  std::shared_ptr<std::atomic_bool> preview_cancel;
  uint64_t preview_generation = 0;
  bool preview_enabled = false;
  std::filesystem::path preview_source;
};

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

const wchar_t* ui_english_font_family() {
  static const bool ubuntu = font_available(L"Ubuntu");
  return ubuntu ? L"Ubuntu" : L"Segoe UI";
}

const wchar_t* ui_cjk_font_family() {
  static const wchar_t* family = font_available(L"Microsoft YaHei UI")
      ? L"Microsoft YaHei UI"
      : (font_available(L"Microsoft YaHei") ? L"Microsoft YaHei" : L"Segoe UI");
  return family;
}

const wchar_t* ui_font_family() {
  return ui_english_font_family();
}

struct FontRun {
  std::wstring_view text;
  bool use_cjk = false;
};

std::vector<FontRun> split_font_runs(HDC dc, HFONT primary_font, std::wstring_view text) {
  std::vector<FontRun> runs;
  if (text.empty()) return runs;
  const HGDIOBJ old_font = primary_font ? SelectObject(dc, primary_font) : nullptr;
  std::vector<WORD> glyphs(text.size(), 0);
  const DWORD res = GetGlyphIndicesW(dc, text.data(), static_cast<int>(text.size()),
                                     glyphs.data(), GGI_MARK_NONEXISTING_GLYPHS);
  if (old_font) SelectObject(dc, old_font);
  if (res == GDI_ERROR) {
    runs.push_back({text, false});
    return runs;
  }
  size_t index = 0;
  while (index < text.size()) {
    const wchar_t ch = text[index];
    const bool use_cjk = ch >= 0x80 && glyphs[index] == 0xFFFF;
    size_t next = index + 1;
    while (next < text.size()) {
      const wchar_t nch = text[next];
      const bool n_cjk = nch >= 0x80 && glyphs[next] == 0xFFFF;
      if (n_cjk != use_cjk) break;
      ++next;
    }
    runs.push_back({text.substr(index, next - index), use_cjk});
    index = next;
  }
  return runs;
}

int measure_mixed_width(HDC dc, HFONT primary_font, HFONT cjk_font, std::wstring_view text) {
  if (text.empty()) return 0;
  const auto runs = split_font_runs(dc, primary_font, text);
  const HGDIOBJ saved = SelectObject(dc, primary_font);
  int total = 0;
  for (const auto& run : runs) {
    SelectObject(dc, (run.use_cjk && cjk_font) ? cjk_font : primary_font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, run.text.data(), static_cast<int>(run.text.size()), &sz);
    total += sz.cx;
  }
  SelectObject(dc, saved);
  return total;
}

std::wstring fit_mixed_text(HDC dc, HFONT primary_font, HFONT cjk_font,
                            std::wstring_view text, int max_width, UINT flags) {
  if (max_width <= 0 || text.empty()) return std::wstring(text);
  if (measure_mixed_width(dc, primary_font, cjk_font, text) <= max_width) {
    return std::wstring(text);
  }
  if ((flags & DT_PATH_ELLIPSIS) != 0) {
    const size_t slash = text.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) {
      const std::wstring_view file = text.substr(slash);
      const std::wstring_view dir = text.substr(0, slash);
      std::wstring root;
      const size_t first_sep = dir.find_first_of(L"\\/");
      if (first_sep != std::wstring_view::npos) {
        root = std::wstring(dir.substr(0, first_sep + 1));
      }
      for (size_t keep = dir.size();;) {
        std::wstring candidate = (keep > root.size() + 3)
            ? (std::wstring(dir.substr(0, keep)) + L"\\..." + std::wstring(file))
            : (root + L"..." + std::wstring(file));
        if (measure_mixed_width(dc, primary_font, cjk_font, candidate) <= max_width) {
          return candidate;
        }
        if (keep <= root.size() + 3) break;
        const size_t prev = dir.rfind(L'\\', keep - 1);
        keep = (prev == std::wstring_view::npos || prev <= root.size()) ? root.size() : prev;
      }
    }
  }
  if ((flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS)) == 0) {
    return std::wstring(text);
  }
  const std::wstring ellipsis = L"…";
  size_t lo = 0, hi = text.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo + 1) / 2;
    size_t len = mid;
    if (len > 0 && len < text.size() &&
        text[len - 1] >= 0xD800 && text[len - 1] <= 0xDBFF) {
      --len;
    }
    const std::wstring cand = std::wstring(text.substr(0, len)) + ellipsis;
    if (measure_mixed_width(dc, primary_font, cjk_font, cand) <= max_width) lo = mid;
    else hi = mid - 1;
  }
  if (lo > 0 && lo < text.size() &&
      text[lo - 1] >= 0xD800 && text[lo - 1] <= 0xDBFF) {
    --lo;
  }
  return std::wstring(text.substr(0, lo)) + ellipsis;
}

void draw_mixed_text(HDC dc, HFONT primary_font, HFONT cjk_font,
                     std::wstring_view text, RECT rect, UINT flags) {
  if (text.empty() || rect.right <= rect.left || rect.bottom <= rect.top) return;
  const std::wstring fitted = fit_mixed_text(dc, primary_font, cjk_font, text,
                                             rect.right - rect.left, flags);
  const auto runs = split_font_runs(dc, primary_font, fitted);
  if (runs.empty()) return;
  const HGDIOBJ saved = SelectObject(dc, primary_font);
  TEXTMETRICW primary_tm{}, cjk_tm{};
  GetTextMetricsW(dc, &primary_tm);
  if (cjk_font) {
    SelectObject(dc, cjk_font);
    GetTextMetricsW(dc, &cjk_tm);
  } else {
    cjk_tm = primary_tm;
  }
  const int total_width = measure_mixed_width(dc, primary_font, cjk_font, fitted);
  int x = rect.left;
  if ((flags & DT_CENTER) != 0) {
    x = rect.left + std::max(0L, ((rect.right - rect.left) - total_width) / 2);
  } else if ((flags & DT_RIGHT) != 0) {
    x = rect.right - total_width;
  }
  const int base_y = (flags & DT_VCENTER) != 0
      ? rect.top + ((rect.bottom - rect.top) - static_cast<int>(primary_tm.tmHeight)) / 2
      : rect.top;
  const int prev_bk = SetBkMode(dc, TRANSPARENT);
  for (const auto& run : runs) {
    SelectObject(dc, (run.use_cjk && cjk_font) ? cjk_font : primary_font);
    const int run_y = (run.use_cjk && cjk_font)
        ? base_y + static_cast<int>(primary_tm.tmAscent) - static_cast<int>(cjk_tm.tmAscent)
        : base_y;
    SIZE sz{};
    GetTextExtentPoint32W(dc, run.text.data(), static_cast<int>(run.text.size()), &sz);
    ExtTextOutW(dc, x, run_y, ETO_CLIPPED, &rect, run.text.data(),
                static_cast<UINT>(run.text.size()), nullptr);
    x += sz.cx;
  }
  SetBkMode(dc, prev_bk);
  SelectObject(dc, saved);
}

void draw_mixed_multiline(HDC dc, HFONT primary_font, HFONT cjk_font,
                          std::wstring_view text, RECT rect, UINT flags) {
  std::vector<std::wstring_view> lines;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t eol = text.find(L'\n', pos);
    std::wstring_view line = eol == std::wstring_view::npos
        ? text.substr(pos) : text.substr(pos, eol - pos);
    if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
    lines.push_back(line);
    if (eol == std::wstring_view::npos) break;
    pos = eol + 1;
  }
  const HGDIOBJ saved = SelectObject(dc, primary_font);
  TEXTMETRICW tm{};
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, saved);
  const int line_h = std::max(1, static_cast<int>(tm.tmHeight + tm.tmExternalLeading));
  const int total_h = static_cast<int>(lines.size()) * line_h;
  int y = (flags & DT_VCENTER) != 0
      ? rect.top + std::max(0, static_cast<int>((rect.bottom - rect.top) - total_h) / 2)
      : rect.top;
  for (const auto& line : lines) {
    RECT row{rect.left, y, rect.right, std::min<LONG>(rect.bottom, y + line_h)};
    draw_mixed_text(dc, primary_font, cjk_font, line, row,
                    (flags & (DT_CENTER | DT_RIGHT | DT_END_ELLIPSIS)) | DT_VCENTER);
    y += line_h;
  }
}

bool draw_combobox_item(const DRAWITEMSTRUCT* draw, HWND window, HFONT primary_font,
                        HFONT cjk_font, bool dark, COLORREF text_color,
                        COLORREF muted_color, COLORREF surface_color) {
  if (!draw || draw->CtlType != ODT_COMBOBOX ||
      draw->itemID == static_cast<UINT>(-1)) {
    return false;
  }
  const bool in_edit = (draw->itemState & ODS_COMBOBOXEDIT) != 0;
  const bool selected = (draw->itemState & ODS_SELECTED) != 0;
  const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
  COLORREF fg = disabled ? muted_color : text_color;
  if (!in_edit) {
    const COLORREF bg = selected
        ? (dark ? RGB(58, 96, 148) : GetSysColor(COLOR_HIGHLIGHT))
        : surface_color;
    if (selected) fg = dark ? RGB(245, 248, 252) : GetSysColor(COLOR_HIGHLIGHTTEXT);
    HBRUSH bg_brush = CreateSolidBrush(bg);
    FillRect(draw->hDC, &draw->rcItem, bg_brush);
    DeleteObject(bg_brush);
  }
  const int len = static_cast<int>(SendMessageW(
      draw->hwndItem, CB_GETLBTEXTLEN, draw->itemID, 0));
  if (len > 0) {
    std::wstring text(static_cast<size_t>(len) + 1u, L'\0');
    const int copied = static_cast<int>(SendMessageW(
        draw->hwndItem, CB_GETLBTEXT, draw->itemID,
        reinterpret_cast<LPARAM>(text.data())));
    if (copied > 0) {
      text.resize(static_cast<size_t>(copied));
      RECT tr = draw->rcItem;
      const UINT dpi = GetDpiForWindow(window);
      const int pad_x = MulDiv(in_edit ? 3 : 8, dpi, 96);
      tr.left += pad_x;
      tr.right -= pad_x;
      SetTextColor(draw->hDC, fg);
      draw_mixed_text(draw->hDC, primary_font, cjk_font, text, tr,
                      DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
  }
  if (!in_edit && (draw->itemState & ODS_FOCUS) != 0) {
    DrawFocusRect(draw->hDC, &draw->rcItem);
  }
  return true;
}

void draw_combo_chrome(HDC dc, HWND control, RECT rc, bool dark,
                       COLORREF surface_color, COLORREF bg_color,
                       COLORREF text_color, COLORREF muted_color, int btn_w) {
  HBRUSH bg_brush = CreateSolidBrush(bg_color);
  FillRect(dc, &rc, bg_brush);
  DeleteObject(bg_brush);
  const bool disabled = !IsWindowEnabled(control);
  const bool dropped = SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0) != 0;
  const bool hot = GetPropW(control, L"HBHot") != nullptr || GetFocus() == control;
  const UINT dpi = GetDpiForWindow(control);
  const COLORREF fill_color = disabled
      ? (dark ? RGB(30, 33, 38) : RGB(240, 242, 245))
      : dropped ? (dark ? RGB(45, 51, 59) : RGB(230, 236, 244))
      : hot ? (dark ? RGB(42, 47, 55) : RGB(248, 250, 252))
      : surface_color;
  const COLORREF border_color = disabled
      ? (dark ? RGB(58, 63, 70) : RGB(208, 214, 220))
      : (dropped || hot) ? (dark ? RGB(105, 115, 128) : RGB(120, 140, 165))
      : (dark ? RGB(75, 82, 91) : RGB(190, 199, 208));
  HBRUSH fill_brush = CreateSolidBrush(fill_color);
  HPEN border_pen = CreatePen(PS_SOLID, 1, border_color);
  const HGDIOBJ old_brush = SelectObject(dc, fill_brush);
  const HGDIOBJ old_pen = SelectObject(dc, border_pen);
  const int radius = MulDiv(3, dpi, 96);
  RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(border_pen);
  DeleteObject(fill_brush);
  const int cx = rc.right - btn_w / 2 - MulDiv(1, dpi, 96);
  const int cy = (rc.top + rc.bottom) / 2 + MulDiv(1, dpi, 96);
  const int arm = MulDiv(4, dpi, 96);
  const int drop = MulDiv(2, dpi, 96);
  HPEN arrow_pen = CreatePen(PS_SOLID, std::max(1, MulDiv(15, dpi, 960)),
                             disabled ? muted_color : text_color);
  const HGDIOBJ prev_pen = SelectObject(dc, arrow_pen);
  const POINT pts[3] = {{cx - arm, cy - drop}, {cx, cy + drop}, {cx + arm, cy - drop}};
  Polyline(dc, pts, 3);
  SelectObject(dc, prev_pen);
  DeleteObject(arrow_pen);
}

std::pair<LRESULT, LRESULT> measure_native_combo_heights(HFONT font, UINT dpi) {
  HWND probe = CreateWindowExW(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST,
                               0, 0, 120, 200, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
  LRESULT sel_h = MulDiv(16, dpi, 96);
  LRESULT list_h = MulDiv(20, dpi, 96);
  if (probe) {
    if (font) SendMessageW(probe, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    sel_h = SendMessageW(probe, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0);
    list_h = SendMessageW(probe, CB_GETITEMHEIGHT, 0, 0);
    DestroyWindow(probe);
  }
  return {sel_h, list_h};
}

class PreviewRenderer {
 public:
  explicit PreviewRenderer(HWND owner) : owner_(owner) {}
  ~PreviewRenderer() {
    if (zoom_edit_) RemoveWindowSubclass(zoom_edit_, zoom_edit_subclass, 1);
    if (zoom_combo_) RemoveWindowSubclass(zoom_combo_, zoom_combo_subclass, 1);
    if (window_) DestroyWindow(window_);
    if (surface_brush_) DeleteObject(surface_brush_);
    if (toolbar_font_) DeleteObject(toolbar_font_);
    if (cjk_toolbar_font_) DeleteObject(cjk_toolbar_font_);
    if (icon_font_) DeleteObject(icon_font_);
  }

  HWND create() {
    static std::once_flag registered;
    std::call_once(registered, [] {
      WNDCLASSEXW cls{sizeof(cls)};
      cls.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
      cls.hInstance = GetModuleHandleW(nullptr);
      cls.lpfnWndProc = &PreviewRenderer::window_proc;
      cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      cls.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
      cls.hIconSm = cls.hIcon;
      cls.lpszClassName = L"HDRBridgePreviewWindow";
      RegisterClassExW(&cls);
    });
    window_ = CreateWindowExW(
        0, L"HDRBridgePreviewWindow", L"HDR Bridge",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        720, 960, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!window_) {
      error_ = HB_UI("Could not create the HDR preview window.");
      return nullptr;
    }
    ui::set_text(window_, HB_UI("HDR Bridge — Preview"));
    create_toolbar();
    try {
      create_device();
    } catch (const std::exception& error) {
      error_ = widen(error.what());
    }
    apply_window_theme();
    reset_layout();
    return window_;
  }

  HWND window() const { return window_; }
  bool ready() const { return device_ && swapchain_ && rtv_; }
  const std::wstring& error() const { return error_; }

  void show(bool visible) {
    if (!window_) return;
    if (visible) {
      if (!IsWindowVisible(window_)) {
        reset_layout();
      }
      ShowWindow(window_, SW_SHOW);
      render();
    } else {
      ShowWindow(window_, SW_HIDE);
    }
  }

  void reset_layout() {
    if (!window_) return;
    if (fullscreen_) toggle_fullscreen();
    if (IsZoomed(window_)) ShowWindow(window_, SW_RESTORE);
    RECT owner_rect{};
    GetWindowRect(owner_, &owner_rect);
    HMONITOR monitor = MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const RECT work = info.rcWork;
    const int work_height = work.bottom - work.top;
    const int desired_height = std::clamp(static_cast<int>(owner_rect.bottom - owner_rect.top),
                                          480, std::max(480, work_height));
    const int height = std::min(desired_height, work_height);
    const int width = std::max(360, height * 3 / 4);
    const int x = owner_rect.right + 8;
    const int y = std::clamp(static_cast<int>(owner_rect.top),
                             static_cast<int>(work.top),
                             static_cast<int>(work.bottom - height));
    SetWindowPos(window_, nullptr, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER);
  }


  static LRESULT CALLBACK zoom_edit_subclass(HWND edit, UINT message, WPARAM wparam,
                                             LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<PreviewRenderer*>(data);
    if (message == WM_CHAR && (wparam == L'\r' || wparam == L'\n' || wparam == VK_ESCAPE)) {
      return 0;
    }
    if (message == WM_KEYDOWN && self) {
      if (wparam == VK_RETURN) {
        self->commit_zoom_edit();
        if (self->window_) SetFocus(self->window_);
        return 0;
      }
      if (wparam == VK_ESCAPE) {
        self->zoom_edit_dirty_ = false;
        self->sync_zoom_combo();
        if (self->window_) SetFocus(self->window_);
        return 0;
      }
    }
    if (message == WM_KILLFOCUS && self && self->window_) {
      HWND next = reinterpret_cast<HWND>(wparam);
      if (next != nullptr && (next == self->window_ || IsChild(self->window_, next))) {
        COMBOBOXINFO info{sizeof(info)};
        const bool has_info = self->zoom_combo_ && GetComboBoxInfo(self->zoom_combo_, &info);
        if (next != self->zoom_combo_ && next != edit &&
            (!has_info || next != info.hwndList)) {
          self->commit_zoom_edit();
        }
      }
    }
    if (message == WM_PAINT && self && self->toolbar_font_ && GetFocus() != edit) {
      const int length = GetWindowTextLengthW(edit);
      std::wstring text(static_cast<size_t>(std::max(length, 0)) + 1u, L'\0');
      if (length > 0) GetWindowTextW(edit, text.data(), length + 1);
      text.resize(static_cast<size_t>(std::max(length, 0)));
      const bool has_non_ascii = std::any_of(text.begin(), text.end(),
                                             [](wchar_t ch) { return ch >= 0x80; });
      if (has_non_ascii) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(edit, &ps);
        RECT rc{};
        GetClientRect(edit, &rc);
        const COLORREF bg = self->dark_ ? RGB(35, 39, 45) : RGB(255, 255, 255);
        const COLORREF fg = self->dark_ ? RGB(235, 239, 243) : RGB(28, 35, 43);
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        rc.left += MulDiv(2, GetDpiForWindow(edit), 96);
        SetTextColor(dc, fg);
        draw_mixed_text(dc, self->toolbar_font_, self->cjk_toolbar_font_, text, rc,
                        DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        EndPaint(edit, &ps);
        return 0;
      }
    }
    return DefSubclassProc(edit, message, wparam, lparam);
  }

  static LRESULT CALLBACK zoom_combo_subclass(HWND control, UINT message, WPARAM wparam,
                                              LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<PreviewRenderer*>(data);
    if (message == WM_MOUSEMOVE) {
      if (!GetPropW(control, L"HBHot")) {
        SetPropW(control, L"HBHot", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, control, 0};
        TrackMouseEvent(&tme);
        InvalidateRect(control, nullptr, FALSE);
      }
    } else if (message == WM_MOUSELEAVE) {
      RemovePropW(control, L"HBHot");
      InvalidateRect(control, nullptr, FALSE);
    } else if (message == WM_ERASEBKGND) {
      return 1;
    } else if (message == WM_PAINT && self) {
      PAINTSTRUCT ps{};
      HDC dc = BeginPaint(control, &ps);
      RECT rc{};
      GetClientRect(control, &rc);
      const UINT dpi = GetDpiForWindow(control);
      const int btn_w = MulDiv(22, dpi, 96);
      const COLORREF surf = self->dark_ ? RGB(35, 39, 45) : RGB(255, 255, 255);
      const COLORREF bg = self->dark_ ? RGB(24, 27, 31) : RGB(244, 246, 248);
      const COLORREF fg = self->dark_ ? RGB(235, 239, 243) : RGB(28, 35, 43);
      draw_combo_chrome(dc, control, rc, self->dark_, surf, bg, fg,
                        RGB(140, 148, 158), btn_w);
      EndPaint(control, &ps);
      if (self->zoom_edit_) InvalidateRect(self->zoom_edit_, nullptr, FALSE);
      return 0;
    } else if (message == WM_NCDESTROY) {
      RemovePropW(control, L"HBHot");
    }
    return DefSubclassProc(control, message, wparam, lparam);
  }

  void create_toolbar() {
    zoom_combo_ = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | CBS_DROPDOWN | CBS_AUTOHSCROLL |
            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 110, 240, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PREVIEW_ZOOM)),
        GetModuleHandleW(nullptr), nullptr);
    for (const wchar_t* item : {HB_UI("Fit"), L"25%", L"50%", L"100%", L"200%", L"400%"}) {
      ui::send(zoom_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(zoom_combo_, CB_SETCURSEL, 0, 0);
    SetWindowSubclass(zoom_combo_, zoom_combo_subclass, 1,
                      reinterpret_cast<DWORD_PTR>(this));
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(zoom_combo_, &info) && info.hwndItem) {
      zoom_edit_ = info.hwndItem;
      SetWindowSubclass(zoom_edit_, zoom_edit_subclass, 1,
                        reinterpret_cast<DWORD_PTR>(this));
    }
    fullscreen_button_ = CreateWindowExW(
        0, L"BUTTON", L"\xE740", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 38, 30, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PREVIEW_FULLSCREEN)),
        GetModuleHandleW(nullptr), nullptr);
    const UINT dpi = GetDpiForWindow(window_);
    toolbar_font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        ui_english_font_family());
    cjk_toolbar_font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        ui_cjk_font_family());
    icon_font_ = CreateFontW(-MulDiv(14, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe MDL2 Assets");
    SendMessageW(zoom_combo_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(toolbar_font_), TRUE);
    const auto [sel_h, list_h] = measure_native_combo_heights(
        toolbar_font_, dpi);
    SendMessageW(zoom_combo_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), sel_h);
    SendMessageW(zoom_combo_, CB_SETITEMHEIGHT, 0, list_h);
    SendMessageW(fullscreen_button_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(icon_font_), TRUE);
    layout_toolbar();
  }

  void layout_toolbar() {
    if (!window_) return;
    RECT rect{};
    GetClientRect(window_, &rect);
    const int y = std::max(0, static_cast<int>(rect.bottom) - toolbar_height_ + 6);
    MoveWindow(fullscreen_button_, std::max(8, static_cast<int>(rect.right) - 46),
               y, 38, 30, TRUE);
    MoveWindow(zoom_combo_, std::max(8, static_cast<int>(rect.right) - 164),
               y, 110, 240, TRUE);
  }

 private:
  struct PreviewTexture {
    hdrbridge::preview::Tile tile;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
  };
  struct PreviewLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<hdrbridge::preview::TileSpan> cols;
    std::vector<hdrbridge::preview::TileSpan> rows;
    std::vector<PreviewTexture> textures;
  };

  PreviewLevel upload_level(uint32_t width, uint32_t height, const uint16_t* rgba_fp16) {
    PreviewLevel level{
        width,
        height,
        hdrbridge::preview::split_texture_axis(width, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION),
        hdrbridge::preview::split_texture_axis(height, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION),
        {}};
    level.textures.reserve(level.cols.size() * level.rows.size());
    for (const auto& y : level.rows) {
      for (const auto& x : level.cols) {
        const hdrbridge::preview::Tile tile{x, y};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = tile.x.sample_length;
        desc.Height = tile.y.sample_length;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = rgba_fp16 +
            (static_cast<size_t>(tile.y.sample_offset) * width + tile.x.sample_offset) * 4u;
        data.SysMemPitch = width * 8u;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        require_hr(device_->CreateTexture2D(&desc, &data, &texture), "create preview texture");
        PreviewTexture item{tile, {}};
        require_hr(device_->CreateShaderResourceView(texture.Get(), nullptr, &item.view),
                   "create preview shader view");
        level.textures.push_back(std::move(item));
      }
    }
    return level;
  }

 public:
  void set_image(hdrbridge::PreviewImage image,
                 std::vector<hdrbridge::preview::MipLevel> mip_levels = {}) {
    try {
      if (!ready()) create_device();
      std::vector<PreviewLevel> levels;
      std::vector<hdrbridge::preview::LevelExtent> extents;
      levels.reserve(1u + mip_levels.size());
      extents.reserve(1u + mip_levels.size());
      levels.push_back(upload_level(image.width, image.height, image.rgba_fp16.data()));
      extents.push_back({image.width, image.height});
      for (const auto& mip : mip_levels) {
        try {
          levels.push_back(upload_level(mip.width, mip.height, mip.rgba_fp16.data()));
          extents.push_back({mip.width, mip.height});
        } catch (const std::exception&) {
          break;
        }
      }
      ID3D11ShaderResourceView* empty[2] = {nullptr, nullptr};
      context_->PSSetShaderResources(0, 2, empty);
      levels_ = std::move(levels);
      level_extents_ = std::move(extents);
      image_width_ = image.width;
      image_height_ = image.height;
      error_.clear();
      fit_ = fitted_scale() < 1.0f;
      zoom_ = 1.0f;
      pan_x_ = pan_y_ = 0.0f;
      if (fit_) sync_fit_combo();
      else update_zoom_combo();
      const std::wstring title = HB_UI("HDR Bridge — Preview — ") +
          std::to_wstring(image_width_) + L" × " + std::to_wstring(image_height_);
      ui::set_text(window_, title.c_str());
      render();
    } catch (const std::exception& error) {
      error_ = widen(error.what());
      clear();
      ui::set_text(window_, HB_UI("HDR Bridge — Preview (unavailable)"));
      if (window_) InvalidateRect(window_, nullptr, TRUE);
    }
  }

  void clear() {
    image_width_ = image_height_ = 0;
    Microsoft::WRL::ComPtr<IDXGIDevice3> dxgi_device;
    if (!levels_.empty()) {
      levels_.clear();
      level_extents_.clear();
      context_->ClearState();
      context_->Flush();
      device_.As(&dxgi_device);
    }
    render();
    if (dxgi_device) dxgi_device->Trim();
  }

  void refresh_fonts() {
    if (!window_ || !zoom_combo_) return;
    const UINT dpi = GetDpiForWindow(window_);
    const auto next = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        ui_english_font_family());
    const auto next_cjk = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        ui_cjk_font_family());
    if (!next || !next_cjk) {
      if (next) DeleteObject(next);
      if (next_cjk) DeleteObject(next_cjk);
      return;
    }
    SendMessageW(zoom_combo_, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
    const auto [sel_h, list_h] = measure_native_combo_heights(next, dpi);
    SendMessageW(zoom_combo_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), sel_h);
    SendMessageW(zoom_combo_, CB_SETITEMHEIGHT, 0, list_h);
    if (toolbar_font_) DeleteObject(toolbar_font_);
    if (cjk_toolbar_font_) DeleteObject(cjk_toolbar_font_);
    toolbar_font_ = next;
    cjk_toolbar_font_ = next_cjk;
    InvalidateRect(window_, nullptr, TRUE);
    if (zoom_edit_) InvalidateRect(zoom_edit_, nullptr, TRUE);
  }

 private:
  using ComPtr = Microsoft::WRL::ComPtr<IUnknown>;

  static void require_hr(HRESULT result, const char* operation) {
    if (FAILED(result)) {
      std::ostringstream out;
      out << operation << " failed (HRESULT 0x" << std::hex
          << static_cast<uint32_t>(result) << ')';
      throw std::runtime_error(out.str());
    }
  }

  void apply_window_theme() {
    DWORD light = 1;
    DWORD size = sizeof(light);
    const bool dark = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS &&
        light == 0;
    dark_ = dark;
    if (surface_brush_) DeleteObject(surface_brush_);
    surface_brush_ = CreateSolidBrush(dark ? RGB(35, 39, 45) : RGB(255, 255, 255));
    const BOOL dark_title = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window_, 20, &dark_title, sizeof(dark_title));
    if (zoom_combo_) SetWindowTheme(zoom_combo_, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (fullscreen_button_) SetWindowTheme(
        fullscreen_button_, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
  }

  static Microsoft::WRL::ComPtr<ID3DBlob> compile_shader(const char* source,
                                                          const char* entry,
                                                          const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(result)) {
      std::string message = errors
          ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
          : "shader compilation failed";
      throw std::runtime_error(message);
    }
    return blob;
  }

  void create_device() {
    if (ready()) return;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
        D3D11_SDK_VERSION, &device_, &selected, &context_);
    if (FAILED(result)) {
      result = D3D11CreateDevice(
          nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
          D3D11_SDK_VERSION, &device_, &selected, &context_);
    }
    require_hr(result, "create D3D11 device");

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    require_hr(device_.As(&dxgi_device), "query DXGI device");
    require_hr(dxgi_device->GetAdapter(&adapter), "query DXGI adapter");
    require_hr(adapter->GetParent(IID_PPV_ARGS(&factory)), "query DXGI factory");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SampleDesc.Count = 1;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    require_hr(factory->CreateSwapChainForHwnd(
                    device_.Get(), window_, &desc, nullptr, nullptr, &swapchain_),
                "create HDR preview swap chain");
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3;
    if (SUCCEEDED(swapchain_.As(&swapchain3))) {
      UINT support = 0;
      if (SUCCEEDED(swapchain3->CheckColorSpaceSupport(
              DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
          (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        swapchain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
      }
    }

    static constexpr char vertex_source[] = R"(
      struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
      VSOut main(uint id : SV_VertexID) {
        float2 p[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
        VSOut output;
        output.position = float4(p[id], 0, 1);
        output.uv = float2((p[id].x + 1) * 0.5, 1 - (p[id].y + 1) * 0.5);
        return output;
      }
    )";
    static constexpr char pixel_source[] = R"(
      Texture2D image_texture0 : register(t0);
      Texture2D image_texture1 : register(t1);
      SamplerState image_sampler : register(s0);
      cbuffer View : register(b0) {
        float4 uv_transform0;
        float4 uv_transform1;
        float4 blend_params;
      };
      struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
      float4 main(VSOut input) : SV_TARGET {
        float4 c0 = image_texture0.Sample(
            image_sampler, input.uv * uv_transform0.xy + uv_transform0.zw);
        if (blend_params.x <= 0.0) return c0;
        float4 c1 = image_texture1.Sample(
            image_sampler, input.uv * uv_transform1.xy + uv_transform1.zw);
        return lerp(c0, c1, blend_params.x);
      }
    )";
    auto vertex = compile_shader(vertex_source, "main", "vs_5_0");
    auto pixel = compile_shader(pixel_source, "main", "ps_5_0");
    require_hr(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(),
                                           nullptr, &vertex_shader_), "create preview vertex shader");
    require_hr(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(),
                                          nullptr, &pixel_shader_), "create preview pixel shader");
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    require_hr(device_->CreateSamplerState(&sampler_desc, &sampler_), "create preview sampler");
    D3D11_BUFFER_DESC view_desc{};
    view_desc.ByteWidth = sizeof(float) * 12;
    view_desc.Usage = D3D11_USAGE_DEFAULT;
    view_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    require_hr(device_->CreateBuffer(&view_desc, nullptr, &view_buffer_),
               "create preview view buffer");
    create_render_target();
  }

  void create_render_target() {
    if (!swapchain_) return;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    require_hr(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)), "get preview back buffer");
    require_hr(device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv_),
                "create preview render target");
  }

  void resize() {
    if (!swapchain_) return;
    RECT rect{};
    GetClientRect(window_, &rect);
    const int content_height = std::max(0, static_cast<int>(rect.bottom) - toolbar_height_);
    if (rect.right <= 0 || content_height <= 0) return;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (rtv_ && SUCCEEDED(swapchain_->GetDesc1(&desc)) &&
        desc.Width == static_cast<UINT>(rect.right) && desc.Height == static_cast<UINT>(rect.bottom)) {
      return;
    }
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    if (SUCCEEDED(swapchain_->ResizeBuffers(0, static_cast<UINT>(rect.right),
                                             static_cast<UINT>(rect.bottom),
                                             DXGI_FORMAT_UNKNOWN, 0))) {
      create_render_target();
    }
  }

  void render() {
    if (!ready()) {
      if (window_) InvalidateRect(window_, nullptr, TRUE);
      return;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const int content_height = std::max(0, static_cast<int>(rect.bottom) - toolbar_height_);
    if (rect.right <= 0 || content_height <= 0) return;
    const FLOAT clear[] = {0.015f, 0.017f, 0.02f, 1.0f};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clear);
    if (!levels_.empty()) {
      const float fit_scale = std::min(
          static_cast<float>(rect.right) / image_width_,
          static_cast<float>(content_height) / image_height_);
      const float scale = fit_ ? fit_scale : zoom_;
      const float draw_width = image_width_ * scale;
      const float draw_height = image_height_ * scale;
      const float draw_x = (rect.right - draw_width) * 0.5f + pan_x_;
      const float draw_y = (content_height - draw_height) * 0.5f + pan_y_;
      const auto selection = hdrbridge::preview::select_trilinear_levels(
          image_width_, image_height_, scale, level_extents_);
      const auto& level0 = levels_[selection.level0];
      const auto& level1 = levels_[selection.level1];
      const auto x_spans = hdrbridge::preview::trilinear_axis_spans(
          image_width_, level0.width, level0.cols, level1.width, level1.cols, selection.blend);
      const auto y_spans = hdrbridge::preview::trilinear_axis_spans(
          image_height_, level0.height, level0.rows, level1.height, level1.rows, selection.blend);
      const float sx0 = scale * (static_cast<float>(image_width_) / level0.width);
      const float sy0 = scale * (static_cast<float>(image_height_) / level0.height);
      const float sx1 = scale * (static_cast<float>(image_width_) / level1.width);
      const float sy1 = scale * (static_cast<float>(image_height_) / level1.height);
      context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
      context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
      context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
      context_->PSSetConstantBuffers(0, 1, view_buffer_.GetAddressOf());
      // Assign pixel centers to exactly one sub-rect. Keep the sampling transform
      // in the original continuous coordinates even when the viewport snaps.
      const auto pixel_edge = [](float edge) { return std::ceil(edge - 0.5f); };
      for (const auto& ry : y_spans) {
        const float top = std::max(0.0f, pixel_edge(draw_y + static_cast<float>(ry.base_start * scale)));
        const float bottom = std::min(
            static_cast<float>(content_height),
            pixel_edge(draw_y + static_cast<float>(ry.base_end * scale)));
        if (bottom <= top) continue;
        for (const auto& rx : x_spans) {
          const float left = std::max(0.0f, pixel_edge(draw_x + static_cast<float>(rx.base_start * scale)));
          const float right = std::min(
              static_cast<float>(rect.right),
              pixel_edge(draw_x + static_cast<float>(rx.base_end * scale)));
          if (right <= left) continue;
          const auto& tex0 = level0.textures[ry.tile0 * level0.cols.size() + rx.tile0];
          const auto& tex1 = level1.textures[ry.tile1 * level1.cols.size() + rx.tile1];
          const D3D11_VIEWPORT viewport{left, top, right - left, bottom - top, 0.0f, 1.0f};
          const float sw0 = tex0.tile.x.sample_length * sx0;
          const float sh0 = tex0.tile.y.sample_length * sy0;
          const float sw1 = tex1.tile.x.sample_length * sx1;
          const float sh1 = tex1.tile.y.sample_length * sy1;
          const float view[12] = {
              viewport.Width / sw0, viewport.Height / sh0,
              (left - (draw_x + tex0.tile.x.sample_offset * sx0)) / sw0,
              (top - (draw_y + tex0.tile.y.sample_offset * sy0)) / sh0,
              viewport.Width / sw1, viewport.Height / sh1,
              (left - (draw_x + tex1.tile.x.sample_offset * sx1)) / sw1,
              (top - (draw_y + tex1.tile.y.sample_offset * sy1)) / sh1,
              selection.blend, 0.0f, 0.0f, 0.0f};
          ID3D11ShaderResourceView* srvs[2] = {tex0.view.Get(), tex1.view.Get()};
          context_->UpdateSubresource(view_buffer_.Get(), 0, nullptr, view, 0, 0);
          context_->RSSetViewports(1, &viewport);
          context_->PSSetShaderResources(0, 2, srvs);
          context_->Draw(3, 0);
        }
      }
    }
    // Frame overlays can use the last viewport as their coordinate space.
    const D3D11_VIEWPORT window_viewport{0.0f, 0.0f, static_cast<float>(rect.right),
                                        static_cast<float>(rect.bottom), 0.0f, 1.0f};
    context_->RSSetViewports(1, &window_viewport);
    swapchain_->Present(1, 0);
  }

  float fitted_scale() const {
    if (!window_ || image_width_ == 0 || image_height_ == 0) return 1.0f;
    RECT rect{};
    GetClientRect(window_, &rect);
    return std::min(static_cast<float>(std::max<LONG>(1, rect.right)) / image_width_,
                    static_cast<float>(std::max<LONG>(1, rect.bottom - toolbar_height_)) /
                        image_height_);
  }

  void clamp_pan() {
    if (!window_ || image_width_ == 0 || image_height_ == 0) return;
    if (fit_) {
      pan_x_ = pan_y_ = 0.0f;
      return;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const float viewport_width = static_cast<float>(std::max<LONG>(1, rect.right));
    const float viewport_height = static_cast<float>(
        std::max<LONG>(1, rect.bottom - toolbar_height_));
    const float max_pan_x = std::max(0.0f, (image_width_ * zoom_ - viewport_width) * 0.5f);
    const float max_pan_y = std::max(0.0f, (image_height_ * zoom_ - viewport_height) * 0.5f);
    pan_x_ = std::clamp(pan_x_, -max_pan_x, max_pan_x);
    pan_y_ = std::clamp(pan_y_, -max_pan_y, max_pan_y);
  }

  void sync_fit_combo() {
    zoom_edit_dirty_ = false;
    if (!zoom_combo_) return;
    updating_zoom_text_ = true;
    ui::set_text(zoom_combo_, HB_UI("Fit"));
    SendMessageW(zoom_combo_, CB_SETCURSEL, 0, 0);
    updating_zoom_text_ = false;
  }

  void set_zoom(float next, POINT anchor) {
    if (image_width_ == 0 || image_height_ == 0) return;
    RECT rect{};
    GetClientRect(window_, &rect);
    const float fit = fitted_scale();
    const float old_scale = fit_ ? fit : zoom_;
    next = std::clamp(next, std::min(0.01f, fit), std::max(16.0f, fit));
    if (fit_ && next == old_scale && (fit <= 0.01f || fit >= 16.0f)) return;
    const float center_x = rect.right * 0.5f;
    const float center_y = (rect.bottom - toolbar_height_) * 0.5f;
    const float image_x = (anchor.x - center_x - pan_x_) / old_scale;
    const float image_y = (anchor.y - center_y - pan_y_) / old_scale;
    fit_ = false;
    zoom_ = next;
    pan_x_ = anchor.x - center_x - image_x * next;
    pan_y_ = anchor.y - center_y - image_y * next;
    clamp_pan();
    update_zoom_combo();
    render();
  }

  void set_fit() {
    fit_ = true;
    pan_x_ = pan_y_ = 0.0f;
    sync_fit_combo();
    render();
  }

  void update_zoom_combo() {
    zoom_edit_dirty_ = false;
    if (!zoom_combo_) return;
    const int percent = static_cast<int>(std::lround(zoom_ * 100.0f));
    int selection = CB_ERR;
    const int values[] = {0, 25, 50, 100, 200, 400};
    for (int index = 1; index < 6; ++index) {
      if (values[index] == percent) selection = index;
    }
    updating_zoom_text_ = true;
    const std::wstring label = std::to_wstring(percent) + L"%";
    ui::set_text(zoom_combo_, label.c_str());
    SendMessageW(zoom_combo_, CB_SETCURSEL, selection, 0);
    if (selection == CB_ERR) ui::set_text(zoom_combo_, label.c_str());
    updating_zoom_text_ = false;
  }

  void sync_zoom_combo() {
    if (fit_) sync_fit_combo();
    else update_zoom_combo();
  }

  bool apply_typed_zoom() {
    if (updating_zoom_text_ || !zoom_combo_) return false;
    wchar_t text[64]{};
    GetWindowTextW(zoom_combo_, text, 64);
    std::wstring value(text);
    value.erase(std::remove_if(value.begin(), value.end(),
        [](wchar_t ch) { return std::iswspace(ch) != 0; }), value.end());
    if (!value.empty() && value.back() == L'%') value.pop_back();
    if (_wcsicmp(value.c_str(), L"fit") == 0 || value == ui::render(HB_UI("Fit"))) {
      set_fit();
      return true;
    }
    wchar_t* end = nullptr;
    const double percent = wcstod(value.c_str(), &end);
    const float fit = fitted_scale();
    const double min_percent = std::min(1.0, static_cast<double>(fit) * 100.0);
    const double max_percent = std::max(1600.0, static_cast<double>(fit) * 100.0);
    if (end == value.c_str() || *end != L'\0' || !std::isfinite(percent) ||
        percent < min_percent || percent > max_percent) {
      return false;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    set_zoom(static_cast<float>(percent / 100.0),
             POINT{rect.right / 2, (rect.bottom - toolbar_height_) / 2});
    sync_zoom_combo();
    return true;
  }

  void commit_zoom_edit() {
    if (!zoom_edit_dirty_) return;
    zoom_edit_dirty_ = false;
    if (!apply_typed_zoom()) {
      sync_zoom_combo();
    }
  }

  void select_zoom(int selection) {
    if (selection <= 0) {
      set_fit();
      return;
    }
    static constexpr float scales[] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    RECT rect{};
    GetClientRect(window_, &rect);
    set_zoom(scales[std::clamp(selection, 1, 5)],
             POINT{rect.right / 2, (rect.bottom - toolbar_height_) / 2});
  }

  void toggle_fullscreen() {
    if (!window_) return;
    if (!fullscreen_) {
      saved_placement_.length = sizeof(saved_placement_);
      GetWindowPlacement(window_, &saved_placement_);
      saved_style_ = GetWindowLongPtrW(window_, GWL_STYLE);
      MONITORINFO info{sizeof(info)};
      GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &info);
      SetWindowLongPtrW(window_, GWL_STYLE, saved_style_ & ~WS_OVERLAPPEDWINDOW);
      SetWindowPos(window_, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                   info.rcMonitor.right - info.rcMonitor.left,
                   info.rcMonitor.bottom - info.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW);
      fullscreen_ = true;
    } else {
      SetWindowLongPtrW(window_, GWL_STYLE, saved_style_);
      SetWindowPlacement(window_, &saved_placement_);
      SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
      fullscreen_ = false;
    }
  }

  void draw_fallback(HDC dc) {
    RECT rect{};
    GetClientRect(window_, &rect);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 225, 232));
    const std::wstring message = ui::render(error_.empty()
        ? HB_UI("HDR preview is unavailable")
        : HB_UI("HDR preview unavailable\r\n") + error_);
    draw_mixed_multiline(dc, toolbar_font_, cjk_toolbar_font_, message, rect,
                         DT_CENTER | DT_VCENTER);
  }

  static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<PreviewRenderer*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<PreviewRenderer*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
      case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (self->ready()) self->render();
        else self->draw_fallback(dc);
        EndPaint(window, &paint);
        return 0;
      }
      case WM_SIZE:
        self->layout_toolbar();
        self->resize();
        if (!self->fit_ && self->image_width_ > 0 && self->image_height_ > 0) {
          const float fit = self->fitted_scale();
          const float clamped_zoom = std::clamp(
              self->zoom_, std::min(0.01f, fit), std::max(16.0f, fit));
          if (clamped_zoom != self->zoom_) {
            self->zoom_ = clamped_zoom;
            self->update_zoom_combo();
          }
        }
        self->clamp_pan();
        self->render();
        return 0;
      case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (measure && measure->CtlType == ODT_COMBOBOX) {
          measure->itemHeight = static_cast<UINT>(
              MulDiv(20, GetDpiForWindow(window), 96));
          return TRUE;
        }
        break;
      }
      case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (draw && draw->CtlID == IDC_PREVIEW_ZOOM &&
            draw_combobox_item(draw, window, self->toolbar_font_,
                               self->cjk_toolbar_font_, self->dark_,
                               self->dark_ ? RGB(235, 239, 243) : RGB(28, 35, 43),
                               RGB(140, 148, 158),
                               self->dark_ ? RGB(35, 39, 45) : RGB(255, 255, 255))) {
          return TRUE;
        }
        break;
      }
      case WM_COMMAND:
        if (LOWORD(wparam) == IDC_PREVIEW_ZOOM && HIWORD(wparam) == CBN_SELCHANGE) {
          self->select_zoom(static_cast<int>(SendMessageW(
              self->zoom_combo_, CB_GETCURSEL, 0, 0)));
          return 0;
        }
        if (LOWORD(wparam) == IDC_PREVIEW_ZOOM && HIWORD(wparam) == CBN_EDITCHANGE) {
          if (!self->updating_zoom_text_) self->zoom_edit_dirty_ = true;
          return 0;
        }
        if (LOWORD(wparam) == IDC_PREVIEW_FULLSCREEN && HIWORD(wparam) == BN_CLICKED) {
          self->toggle_fullscreen();
          return 0;
        }
        break;
      case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
          SetCursor(LoadCursorW(nullptr,
              self->dragging_ ? IDC_SIZEALL :
              (!self->levels_.empty() ? IDC_HAND : IDC_ARROW)));
          return TRUE;
        }
        break;
      case WM_MOUSEWHEEL:
        if ((GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) != 0) {
          POINT anchor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
          ScreenToClient(window, &anchor);
          const float current = self->fit_ ? self->fitted_scale() : self->zoom_;
          self->set_zoom(current * (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1.2f : 1.0f / 1.2f),
                         anchor);
          return 0;
        }
        break;
      case WM_LBUTTONDBLCLK: {
        if (self->fit_) {
          self->set_zoom(1.0f, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        } else {
          self->set_fit();
        }
        return 0;
      }
      case WM_NCLBUTTONDOWN:
        self->commit_zoom_edit();
        break;
      case WM_RBUTTONDOWN:
      case WM_MBUTTONDOWN:
        if (GetFocus() == self->zoom_edit_) SetFocus(window);
        self->commit_zoom_edit();
        break;
      case WM_LBUTTONDOWN:
        if (GetFocus() == self->zoom_edit_) SetFocus(window);
        self->commit_zoom_edit();
        self->dragging_ = true;
        self->drag_origin_ = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        self->drag_pan_x_ = self->pan_x_;
        self->drag_pan_y_ = self->pan_y_;
        SetCapture(window);
        return 0;
      case WM_MOUSEMOVE:
        if (self->dragging_) {
          if (self->fit_) return 0;
          const int cursor_x = GET_X_LPARAM(lparam);
          const int cursor_y = GET_Y_LPARAM(lparam);
          const float raw_x = self->drag_pan_x_ + cursor_x - self->drag_origin_.x;
          const float raw_y = self->drag_pan_y_ + cursor_y - self->drag_origin_.y;
          self->pan_x_ = raw_x;
          self->pan_y_ = raw_y;
          self->clamp_pan();
          if (self->pan_x_ != raw_x) {
            self->drag_pan_x_ = self->pan_x_;
            self->drag_origin_.x = cursor_x;
          }
          if (self->pan_y_ != raw_y) {
            self->drag_pan_y_ = self->pan_y_;
            self->drag_origin_.y = cursor_y;
          }
          self->render();
          return 0;
        }
        break;
      case WM_LBUTTONUP:
        if (self->dragging_) {
          self->dragging_ = false;
          if (GetCapture() == window) ReleaseCapture();
          return 0;
        }
        break;
      case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && self->fullscreen_) {
          self->toggle_fullscreen();
          return 0;
        }
        break;
      case WM_CTLCOLOREDIT: {
        const auto dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, self->dark_ ? RGB(35, 39, 45) : RGB(255, 255, 255));
        SetTextColor(dc, self->dark_ ? RGB(235, 239, 243) : RGB(28, 35, 43));
        return reinterpret_cast<LRESULT>(self->surface_brush_);
      }
      case WM_ERASEBKGND:
        return 1;
      case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        PostMessageW(self->owner_, WM_COMMAND,
                     MAKEWPARAM(IDC_PREVIEW_TOGGLE, BN_CLICKED),
                     reinterpret_cast<LPARAM>(window));
        return 0;
      case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }

  HWND owner_ = nullptr;
  HWND window_ = nullptr;
  std::wstring error_;
  uint32_t image_width_ = 0;
  uint32_t image_height_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
  std::vector<PreviewLevel> levels_;
  std::vector<hdrbridge::preview::LevelExtent> level_extents_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> view_buffer_;
  HWND zoom_combo_ = nullptr;
  HWND zoom_edit_ = nullptr;
  HWND fullscreen_button_ = nullptr;
  static constexpr int toolbar_height_ = 44;
  bool dark_ = false;
  bool fit_ = true;
  float zoom_ = 1.0f;
  float pan_x_ = 0.0f;
  float pan_y_ = 0.0f;
  bool dragging_ = false;
  POINT drag_origin_{};
  float drag_pan_x_ = 0.0f;
  float drag_pan_y_ = 0.0f;
  bool fullscreen_ = false;
  LONG_PTR saved_style_ = 0;
  WINDOWPLACEMENT saved_placement_{sizeof(WINDOWPLACEMENT)};
  HFONT toolbar_font_ = nullptr;
  HFONT cjk_toolbar_font_ = nullptr;
  HFONT icon_font_ = nullptr;
  HBRUSH surface_brush_ = nullptr;
  bool updating_zoom_text_ = false;
  bool zoom_edit_dirty_ = false;
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
void update_mode_ui(AppState* state, bool restore_saved_gamut = true);
void set_rich_editable_text(AppState* state, HWND rich, const std::wstring& text);

void open_folder_for(HWND owner, const std::filesystem::path& path, bool select_file) {
  if (path.empty()) return;
  if (select_file) {
    const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(owner, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  } else {
    ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
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

void set_rich_selection_format(HWND rich, const wchar_t* family, COLORREF color) {
  CHARFORMAT2W cf{};
  cf.cbSize = sizeof(cf);
  cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
  cf.yHeight = 200;
  cf.crTextColor = color;
  wcsncpy_s(cf.szFaceName, family, _TRUNCATE);
  SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
}

void append_rich_mixed(HWND rich, std::wstring_view visible_text,
                       HFONT primary_font, COLORREF color) {
  if (!rich || visible_text.empty()) return;
  HDC dc = GetDC(rich);
  const auto runs = split_font_runs(dc, primary_font, visible_text);
  if (dc) ReleaseDC(rich, dc);
  const wchar_t* eng_family = ui_english_font_family();
  const wchar_t* cjk_family = ui_cjk_font_family();
  for (const auto& run : runs) {
    CHARRANGE at_end{std::numeric_limits<LONG>::max(), std::numeric_limits<LONG>::max()};
    SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&at_end));
    set_rich_selection_format(rich, run.use_cjk ? cjk_family : eng_family, color);
    std::wstring chunk(run.text);
    SendMessageW(rich, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(chunk.c_str()));
  }
}

void set_rich_text_mixed(HWND rich, std::wstring_view visible_text,
                         HFONT primary_font, COLORREF color, bool preserve_scroll) {
  if (!rich) return;
  CHARRANGE saved_sel{0, 0};
  LRESULT first_line = 0;
  if (preserve_scroll) {
    SendMessageW(rich, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved_sel));
    first_line = SendMessageW(rich, EM_GETFIRSTVISIBLELINE, 0, 0);
  }
  SendMessageW(rich, WM_SETREDRAW, FALSE, 0);
  const LRESULT prev_mask = SendMessageW(rich, EM_GETEVENTMASK, 0, 0);
  if ((prev_mask & ENM_CHANGE) != 0) {
    SendMessageW(rich, EM_SETEVENTMASK, 0, prev_mask & ~ENM_CHANGE);
  }
  SetWindowTextW(rich, L"");
  append_rich_mixed(rich, visible_text, primary_font, color);
  if ((prev_mask & ENM_CHANGE) != 0) {
    SendMessageW(rich, EM_SETEVENTMASK, 0, prev_mask);
  }
  if (preserve_scroll) {
    SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved_sel));
    const LRESULT now_line = SendMessageW(rich, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (first_line != now_line) {
      SendMessageW(rich, EM_LINESCROLL, 0, first_line - now_line);
    }
  } else {
    CHARRANGE start_sel{0, 0};
    SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&start_sel));
    SendMessageW(rich, EM_LINESCROLL, 0, -SendMessageW(rich, EM_GETFIRSTVISIBLELINE, 0, 0));
  }
  SendMessageW(rich, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(rich, nullptr, TRUE);
}

void refresh_rich_mixed_style(HWND rich, HFONT primary_font, COLORREF bg, COLORREF fg) {
  if (!rich) return;
  SendMessageW(rich, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(bg));
  CHARFORMAT2W def_cf{};
  def_cf.cbSize = sizeof(def_cf);
  def_cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC;
  def_cf.yHeight = 200;
  def_cf.crTextColor = fg;
  wcsncpy_s(def_cf.szFaceName, ui_english_font_family(), _TRUNCATE);
  SendMessageW(rich, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&def_cf));
  const int len = GetWindowTextLengthW(rich);
  if (len <= 0) return;
  std::wstring current(static_cast<size_t>(len) + 1u, L'\0');
  GetWindowTextW(rich, current.data(), len + 1);
  current.resize(static_cast<size_t>(len));
  set_rich_text_mixed(rich, current, primary_font, fg, true);
}

void apply_fonts(AppState* state) {
  const wchar_t* english_family = ui_english_font_family();
  const wchar_t* cjk_family = ui_cjk_font_family();
  const UINT dpi = GetDpiForWindow(state->window);
  const int pixels = -MulDiv(10, dpi, 72);
  const int small_pixels = -MulDiv(9, dpi, 72);
  HFONT next = CreateFontW(pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           english_family);
  HFONT next_cjk = CreateFontW(pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               cjk_family);
  HFONT next_small = CreateFontW(small_pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 english_family);
  HFONT next_small_cjk = CreateFontW(small_pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                     cjk_family);
  HFONT next_title = CreateFontW(-MulDiv(12, dpi, 72), 0, 0, 0, FW_SEMIBOLD,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 english_family);
  HFONT next_version = CreateFontW(pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                   english_family);
  if (!next || !next_cjk || !next_small || !next_small_cjk || !next_title || !next_version) {
    if (next) DeleteObject(next);
    if (next_cjk) DeleteObject(next_cjk);
    if (next_small) DeleteObject(next_small);
    if (next_small_cjk) DeleteObject(next_small_cjk);
    if (next_title) DeleteObject(next_title);
    if (next_version) DeleteObject(next_version);
    return;
  }
  const auto [combo_sel_h, combo_list_h] = measure_native_combo_heights(next, dpi);
  struct FontUpdate {
    HWND title;
    HWND version;
    HFONT font;
    LRESULT combo_sel_h;
    LRESULT combo_list_h;
  };
  FontUpdate update{state->title, state->version, next, combo_sel_h, combo_list_h};
  EnumChildWindows(state->window, [](HWND child, LPARAM parameter) -> BOOL {
    const auto* update = reinterpret_cast<const FontUpdate*>(parameter);
    if (child != update->title && child != update->version) {
      SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(update->font), TRUE);
      wchar_t cls[64]{};
      GetClassNameW(child, cls, 64);
      if (_wcsicmp(cls, L"ComboBox") == 0) {
        SendMessageW(child, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), update->combo_sel_h);
        SendMessageW(child, CB_SETITEMHEIGHT, 0, update->combo_list_h);
      }
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&update));
  SendMessageW(state->title, WM_SETFONT, reinterpret_cast<WPARAM>(next_title), TRUE);
  SendMessageW(state->version, WM_SETFONT, reinterpret_cast<WPARAM>(next_version), TRUE);
  if (state->queue) {
    SendMessageW(state->queue, LB_SETITEMHEIGHT, 0, MulDiv(30, dpi, 96));
  }
  if (state->tasks) {
    SendMessageW(state->tasks, LB_SETITEMHEIGHT, 0, MulDiv(52, dpi, 96));
  }
  if (state->font) DeleteObject(state->font);
  if (state->cjk_font) DeleteObject(state->cjk_font);
  if (state->small_font) DeleteObject(state->small_font);
  if (state->small_cjk_font) DeleteObject(state->small_cjk_font);
  if (state->title_font) DeleteObject(state->title_font);
  if (state->version_font) DeleteObject(state->version_font);
  state->font = next;
  state->cjk_font = next_cjk;
  state->small_font = next_small;
  state->small_cjk_font = next_small_cjk;
  state->title_font = next_title;
  state->version_font = next_version;
  refresh_rich_mixed_style(state->inspector, state->font, state->surface_color, state->text_color);
  refresh_rich_mixed_style(state->log, state->font, state->surface_color, state->text_color);
  refresh_rich_mixed_style(state->base_name, state->font, state->surface_color, state->text_color);
  refresh_rich_mixed_style(state->name_suffix, state->font, state->surface_color, state->text_color);
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
  refresh_rich_mixed_style(state->inspector, state->font, state->surface_color, state->text_color);
  refresh_rich_mixed_style(state->log, state->font, state->surface_color, state->text_color);
  refresh_rich_mixed_style(state->base_name, state->font, state->surface_color, state->text_color);
  refresh_rich_mixed_style(state->name_suffix, state->font, state->surface_color, state->text_color);
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

std::int64_t now_epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t read_layout_qword(const wchar_t* name) {
  std::uint64_t value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kLayoutRegistryPath, name,
                   RRF_RT_REG_QWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
    return 0;
  }
  return static_cast<std::int64_t>(value);
}

void write_layout_qword(const wchar_t* name, std::int64_t value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kLayoutRegistryPath, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  const auto raw = static_cast<std::uint64_t>(value);
  RegSetValueExW(key, name, 0, REG_QWORD,
                 reinterpret_cast<const BYTE*>(&raw), sizeof(raw));
  RegCloseKey(key);
}

std::string read_layout_utf8(const wchar_t* name) {
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, kLayoutRegistryPath, name,
                   RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
      size < sizeof(wchar_t)) {
    return {};
  }
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (RegGetValueW(HKEY_CURRENT_USER, kLayoutRegistryPath, name,
                   RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) {
    return {};
  }
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  return narrow(value);
}

void write_layout_utf8(const wchar_t* name, const std::string& utf8) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kLayoutRegistryPath, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  if (utf8.empty()) {
    RegDeleteValueW(key, name);
  } else {
    const std::wstring wide = widen(utf8);
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(wide.c_str()),
                   static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
  }
  RegCloseKey(key);
}

void load_split_layout(AppState* state) {
  if (const auto value = read_layout_value(L"RightPanelWidth")) {
    state->right_panel_width = static_cast<int>(*value);
  }
  if (const auto value = read_layout_value(L"ActivityPanelHeight")) {
    state->activity_panel_height = static_cast<int>(*value);
  }
  if (const auto value = read_layout_value(L"FilesPanelHeight")) {
    state->files_panel_height = static_cast<int>(*value);
  }
}

int saved_index(const wchar_t* name, int fallback, int maximum) {
  if (const auto value = read_layout_value(name)) {
    return std::clamp(static_cast<int>(*value), 0, maximum);
  }
  return fallback;
}

bool is_frame_png(const AppState* state, int format_index) {
  return format_index == 7 &&
      SendMessageW(state->frame_export_format, CB_GETCURSEL, 0, 0) == 1;
}

int encoding_slot_for(const AppState* state, int format_index) {
  return is_frame_png(state, format_index) ? 8 : std::clamp(format_index, 0, 7);
}

bool is_compression_slot(int slot) {
  return slot == 1 || slot == 2 || slot == 8;
}

void load_conversion_settings(AppState* state) {
  const bool legacy_order = saved_index(L"SettingsSchemaVersion", 0, 2) < 2;
  int saved_format = saved_index(L"OutputFormat", 0, 7);
  if (legacy_order && saved_format < 7) {
    constexpr int migrated_format[] = {0, 1, 3, 4, 5, 2, 6};
    saved_format = migrated_format[saved_format];
  }
  SendMessageW(state->format, CB_SETCURSEL, saved_format, 0);
  SendMessageW(state->representation, CB_SETCURSEL,
               saved_index(L"OutputRepresentation", 0, 1), 0);
  SendMessageW(state->transfer, CB_SETCURSEL,
               saved_index(L"OutputTransfer", 0, 2), 0);
  SendMessageW(state->gainmap_resolution, CB_SETCURSEL,
               saved_index(L"GainMapResolution", 2, 2), 0);
  SendMessageW(state->gainmap_channels, CB_SETCURSEL,
               saved_index(L"GainMapChannels", 1, 1), 0);
  state->uhdr_gainmap_channels = static_cast<int>(
      SendMessageW(state->gainmap_channels, CB_GETCURSEL, 0, 0));
  state->normal_gainmap_resolution = saved_index(L"GainMapResolution", 2, 2);
  state->frame_gainmap_resolution = saved_index(L"FrameGainMapResolution", 2, 2);
  state->frame_gainmap_channels = saved_index(L"FrameGainMapChannels", 1, 1);
  state->normal_copy_exif = saved_index(L"CopyExif", 1, 1) != 0;
  state->normal_copy_xmp = saved_index(L"CopyXmp", 1, 1) != 0;
  state->lossless_setting = saved_index(L"Lossless", 1, 1) != 0;
  state->tiff_compressed = saved_index(L"TiffCompressed", 1, 1) != 0;
  SendMessageW(state->tiff_compression, CB_SETCURSEL,
               state->tiff_compressed ? 0 : 1, 0);
  SendMessageW(state->gainmap_export_format, CB_SETCURSEL,
               saved_index(L"GainMapExportFormat", 0, 3), 0);
  SendMessageW(state->frame_export_format, CB_SETCURSEL,
               saved_index(L"FrameExportFormat", 0, 1), 0);
  SendMessageW(state->lossless, BM_SETCHECK,
               state->lossless_setting ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(state->copy_exif, BM_SETCHECK,
               saved_index(L"CopyExif", 1, 1) ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(state->copy_xmp, BM_SETCHECK,
               saved_index(L"CopyXmp", 1, 1) ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(state->use_suffix, BM_SETCHECK,
               saved_index(L"UseSuffix", 1, 1) ? BST_CHECKED : BST_UNCHECKED, 0);
  constexpr size_t legacy_index_for_new[] = {0, 1, 5, 2, 3, 4, 6};
  for (size_t index = 0; index < state->encoding_values.size(); ++index) {
    const size_t stored_index = legacy_order && index < 7 ? legacy_index_for_new[index] : index;
    const std::wstring name = L"EncodingValue" + std::to_wstring(stored_index);
    if (const auto value = read_layout_value(name.c_str())) {
      const bool compression = is_compression_slot(static_cast<int>(index));
      state->encoding_values[index] = std::clamp(
          static_cast<int>(*value), 1, compression ? 9 : 100);
    }
  }
}

void reset_conversion_settings(AppState* state) {
  state->encoding_values = {95, 4, 6, 90, 95, 95, 95, 95, 4};
  state->lossless_setting = true;
  state->tiff_compressed = true;
  SendMessageW(state->tiff_compression, CB_SETCURSEL, 0, 0);
  SendMessageW(state->gainmap_export_format, CB_SETCURSEL, 0, 0);
  SendMessageW(state->frame_export_format, CB_SETCURSEL, 0, 0);
  SendMessageW(state->format, CB_SETCURSEL, 0, 0);
  SendMessageW(state->representation, CB_SETCURSEL, 0, 0);
  SendMessageW(state->transfer, CB_SETCURSEL, 0, 0);
  SendMessageW(state->gainmap_resolution, CB_SETCURSEL, 2, 0);
  SendMessageW(state->gainmap_channels, CB_SETCURSEL, 1, 0);
  state->uhdr_gainmap_channels = 1;
  state->normal_gainmap_resolution = 2; state->frame_gainmap_resolution = 2;
  state->frame_gainmap_channels = 1;
  state->normal_copy_exif = true; state->normal_copy_xmp = true;
  SendMessageW(state->lossless, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(state->copy_exif, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(state->copy_xmp, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(state->use_suffix, BM_SETCHECK, BST_CHECKED, 0);
  set_rich_editable_text(state, state->name_suffix, L"_ultrahdr");
  state->last_format_index = -1;
  state->last_transfer_index = -1;
  state->last_representation_index = -1;
  state->last_frame_export_index = -1;
  state->last_encoding_slot = -1;
  update_mode_ui(state, false);
}

void save_layout(AppState* state) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kLayoutRegistryPath, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  write_layout_value(key, L"RightPanelWidth", static_cast<DWORD>(state->right_panel_width));
  write_layout_value(key, L"ActivityPanelHeight", static_cast<DWORD>(state->activity_panel_height));
  write_layout_value(key, L"FilesPanelHeight", static_cast<DWORD>(state->files_panel_height));
  const int format_index = static_cast<int>(SendMessageW(state->format, CB_GETCURSEL, 0, 0));
  const bool frame_png = is_frame_png(state, format_index);
  const bool checked = SendMessageW(state->lossless, BM_GETCHECK, 0, 0) == BST_CHECKED;
  if (format_index != 1 && format_index != 2 && format_index != 7) state->lossless_setting = checked;
  state->tiff_compressed = SendMessageW(state->tiff_compression, CB_GETCURSEL, 0, 0) != 1;
  if (format_index >= 0) {
    const int slot = encoding_slot_for(state, format_index);
    state->encoding_values[static_cast<size_t>(slot)] =
        static_cast<int>(SendMessageW(state->quality, TBM_GETPOS, 0, 0));
  }
  write_layout_value(key, L"OutputFormat", static_cast<DWORD>(std::max(format_index, 0)));
  write_layout_value(key, L"SettingsSchemaVersion", 2);
  const int gamut_index = static_cast<int>(SendMessageW(state->gamut, CB_GETCURSEL, 0, 0));
  const DWORD gamut_cicp = gamut_index == CB_ERR ? 9u : static_cast<DWORD>(
      SendMessageW(state->gamut, CB_GETITEMDATA, gamut_index, 0));
  if (format_index != 7 || frame_png) write_layout_value(key, L"OutputGamutCicp", gamut_cicp);
  write_layout_value(key, L"OutputRepresentation", static_cast<DWORD>(
      std::max(0, static_cast<int>(SendMessageW(state->representation, CB_GETCURSEL, 0, 0)))));
  write_layout_value(key, L"OutputTransfer", static_cast<DWORD>(SendMessageW(state->transfer, CB_GETCURSEL, 0, 0)));
  if (format_index == 7) {
    state->frame_gainmap_resolution = static_cast<int>(SendMessageW(state->gainmap_resolution, CB_GETCURSEL, 0, 0));
    state->frame_gainmap_channels = static_cast<int>(SendMessageW(state->gainmap_channels, CB_GETCURSEL, 0, 0));
  } else {
    state->normal_gainmap_resolution = static_cast<int>(SendMessageW(state->gainmap_resolution, CB_GETCURSEL, 0, 0));
    state->normal_copy_exif = SendMessageW(state->copy_exif, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state->normal_copy_xmp = SendMessageW(state->copy_xmp, BM_GETCHECK, 0, 0) == BST_CHECKED;
  }
  write_layout_value(key, L"GainMapResolution", static_cast<DWORD>(state->normal_gainmap_resolution));
  write_layout_value(key, L"FrameGainMapResolution", static_cast<DWORD>(state->frame_gainmap_resolution));
  write_layout_value(key, L"FrameGainMapChannels", static_cast<DWORD>(state->frame_gainmap_channels));
  write_layout_value(key, L"GainMapChannels",
                     static_cast<DWORD>(state->uhdr_gainmap_channels));
  write_layout_value(key, L"Lossless", state->lossless_setting);
  write_layout_value(key, L"TiffCompressed", state->tiff_compressed);
  write_layout_value(key, L"GainMapExportFormat", static_cast<DWORD>(
      std::max(0, static_cast<int>(SendMessageW(
          state->gainmap_export_format, CB_GETCURSEL, 0, 0)))));
  write_layout_value(key, L"FrameExportFormat", static_cast<DWORD>(
      std::max(0, static_cast<int>(SendMessageW(
          state->frame_export_format, CB_GETCURSEL, 0, 0)))));
  write_layout_value(key, L"CopyExif", state->normal_copy_exif);
  write_layout_value(key, L"CopyXmp", state->normal_copy_xmp);
  write_layout_value(key, L"UseSuffix", SendMessageW(state->use_suffix, BM_GETCHECK, 0, 0) == BST_CHECKED);
  for (size_t index = 0; index < state->encoding_values.size(); ++index) {
    const std::wstring name = L"EncodingValue" + std::to_wstring(index);
    write_layout_value(key, name.c_str(),
                       static_cast<DWORD>(state->encoding_values[index]));
  }
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
  if (info.gain_map_present) out << L" · Gain Map";
  return out.str();
}

LRESULT CALLBACK queue_subclass(HWND control, UINT message, WPARAM wparam,
                                LPARAM lparam, UINT_PTR subclass_id, DWORD_PTR) {
  const bool select_all =
      (message == WM_KEYDOWN && wparam == L'A' &&
       (GetKeyState(VK_CONTROL) & 0x8000) != 0) ||
      (message == WM_CHAR && wparam == 1);
  if (select_all) {
    if (subclass_id != 1) return 0;
    if (message == WM_KEYDOWN) {
      const int count = static_cast<int>(SendMessageW(control, LB_GETCOUNT, 0, 0));
      for (int index = 0; index < count; ++index) {
        SendMessageW(control, LB_SETSEL, TRUE, index);
      }
      if (count > 0) SendMessageW(control, LB_SETCARETINDEX, 0, FALSE);
      InvalidateRect(control, nullptr, TRUE);
      PostMessageW(GetParent(control), WM_APP_QUEUE_SELECT_ALL, 0, 0);
    }
    return 0;
  }
  if (message == WM_KEYDOWN && wparam == VK_DELETE) {
    SendMessageW(GetParent(control), WM_COMMAND,
                 MAKEWPARAM(subclass_id == 2 ? ID_TASK_DELETE : IDC_REMOVE, BN_CLICKED),
                 reinterpret_cast<LPARAM>(control));
    return 0;
  }
  if (message == WM_CONTEXTMENU) {
    SendMessageW(GetParent(control), WM_CONTEXTMENU,
                 reinterpret_cast<WPARAM>(control), lparam);
    return 0;
  }
  if (subclass_id == 2 && message == WM_LBUTTONDOWN) {
    const int item = static_cast<int>(SendMessageW(control, LB_ITEMFROMPOINT, 0, lparam));
    if (HIWORD(item) == 0) {
      SendMessageW(control, LB_SETCURSEL, LOWORD(item), 0);
      SetPropW(control, L"HDRBridgeDragTask", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(LOWORD(item) + 1)));
      SetCapture(control);
    }
  }
  if (subclass_id == 2 && message == WM_LBUTTONUP) {
    const int selected = static_cast<int>(SendMessageW(control, LB_GETCURSEL, 0, 0));
    const int width = [] (HWND hwnd) { RECT r{}; GetClientRect(hwnd, &r); return r.right; }(control);
    const int action_width = MulDiv(96, GetDpiForWindow(control), 96);
    const int x = GET_X_LPARAM(lparam);
    const auto stored = reinterpret_cast<INT_PTR>(GetPropW(control, L"HDRBridgeDragTask"));
    RemovePropW(control, L"HDRBridgeDragTask");
    if (GetCapture() == control) ReleaseCapture();
    if (x >= width - action_width && selected != LB_ERR) {
      SendMessageW(GetParent(control), WM_COMMAND,
                   x < width - action_width / 2 ? ID_TASK_EDIT : ID_TASK_DELETE, 0);
      return 0;
    }
    const int target = static_cast<int>(SendMessageW(control, LB_ITEMFROMPOINT, 0, lparam));
    if (stored > 0 && HIWORD(target) == 0 && LOWORD(target) != stored - 1) {
      SendMessageW(GetParent(control), WM_APP_TASK_REORDER,
                   static_cast<WPARAM>(stored - 1), LOWORD(target));
      return 0;
    }
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

LRESULT CALLBACK mixed_static_subclass(HWND control, UINT message, WPARAM wparam,
                                       LPARAM lparam, UINT_PTR, DWORD_PTR data) {
  auto* state = reinterpret_cast<AppState*>(data);
  if (message == WM_SETTEXT) {
    const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
    InvalidateRect(control, nullptr, TRUE);
    return result;
  }
  if (message == WM_ERASEBKGND) {
    return 1;
  }
  if (message == WM_PAINT && state && state->font) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(control, &ps);
    RECT rc{};
    GetClientRect(control, &rc);
    HBRUSH bg = reinterpret_cast<HBRUSH>(SendMessageW(
        GetParent(control), WM_CTLCOLORSTATIC,
        reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(control)));
    FillRect(dc, &rc, bg ? bg : state->background_brush);
    const int len = GetWindowTextLengthW(control);
    if (len > 0) {
      std::wstring text(static_cast<size_t>(len) + 1u, L'\0');
      GetWindowTextW(control, text.data(), len + 1);
      text.resize(static_cast<size_t>(len));
      const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
      const LONG_PTR align = style & SS_TYPEMASK;
      UINT flags = DT_TOP | (align == SS_CENTER ? DT_CENTER :
                             align == SS_RIGHT ? DT_RIGHT : DT_LEFT);
      if ((style & SS_PATHELLIPSIS) == SS_PATHELLIPSIS) flags |= DT_PATH_ELLIPSIS;
      else if ((style & SS_ENDELLIPSIS) == SS_ENDELLIPSIS) flags |= DT_END_ELLIPSIS;
      HFONT primary = control == state->title ? state->title_font :
                      control == state->version ? state->version_font : state->font;
      draw_mixed_text(dc, primary, state->cjk_font, text, rc, flags);
    }
    EndPaint(control, &ps);
    return 0;
  }
  return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK combo_mixed_subclass(HWND control, UINT message, WPARAM wparam,
                                        LPARAM lparam, UINT_PTR, DWORD_PTR data) {
  auto* state = reinterpret_cast<AppState*>(data);
  if (message == WM_MOUSEMOVE) {
    if (!GetPropW(control, L"HBHot")) {
      SetPropW(control, L"HBHot", reinterpret_cast<HANDLE>(1));
      TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, control, 0};
      TrackMouseEvent(&tme);
      InvalidateRect(control, nullptr, FALSE);
    }
  } else if (message == WM_MOUSELEAVE) {
    RemovePropW(control, L"HBHot");
    InvalidateRect(control, nullptr, FALSE);
  } else if (message == WM_ERASEBKGND) {
    return 1;
  } else if (message == WM_PAINT && state && state->font) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(control, &ps);
    RECT rc{};
    GetClientRect(control, &rc);
    const bool disabled = !IsWindowEnabled(control);
    const UINT dpi = GetDpiForWindow(control);
    const int btn_w = MulDiv(22, dpi, 96);
    draw_combo_chrome(dc, control, rc, state->dark, state->surface_color,
                      state->background_color, state->text_color,
                      state->muted_text_color, btn_w);
    const int sel = static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0));
    if (sel >= 0) {
      const int len = static_cast<int>(SendMessageW(control, CB_GETLBTEXTLEN, sel, 0));
      if (len > 0) {
        std::wstring text(static_cast<size_t>(len) + 1u, L'\0');
        const int copied = static_cast<int>(SendMessageW(
            control, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(text.data())));
        if (copied > 0) {
          text.resize(static_cast<size_t>(copied));
          RECT tr = rc;
          tr.left += MulDiv(8, dpi, 96);
          tr.right = std::max<LONG>(tr.left, rc.right - btn_w - MulDiv(2, dpi, 96));
          SetTextColor(dc, disabled ? state->muted_text_color : state->text_color);
          draw_mixed_text(dc, state->font, state->cjk_font, text, tr,
                          DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
      }
    }
    if (GetFocus() == control &&
        (SendMessageW(GetParent(control), WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
      RECT focus = rc;
      InflateRect(&focus, -MulDiv(3, dpi, 96), -MulDiv(3, dpi, 96));
      DrawFocusRect(dc, &focus);
    }
    EndPaint(control, &ps);
    return 0;
  } else if (message == WM_NCDESTROY) {
    RemovePropW(control, L"HBHot");
  }
  return DefSubclassProc(control, message, wparam, lparam);
}

void update_rich_insets(HWND control) {
  if (!control) return;
  const UINT dpi = GetDpiForWindow(control);
  const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
  if ((style & ES_MULTILINE) != 0) {
    RECT rc{};
    GetClientRect(control, &rc);
    InflateRect(&rc, -MulDiv(8, dpi, 96), -MulDiv(6, dpi, 96));
    SendMessageW(control, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&rc));
  } else {
    SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(MulDiv(6, dpi, 96), MulDiv(6, dpi, 96)));
  }
}

LRESULT CALLBACK rich_edit_subclass(HWND control, UINT message, WPARAM wparam,
                                    LPARAM lparam, UINT_PTR, DWORD_PTR data) {
  auto* state = reinterpret_cast<AppState*>(data);
  if (message == WM_NCCALCSIZE) {
    const LRESULT res = DefSubclassProc(control, message, wparam, lparam);
    if (wparam == TRUE && lparam) {
      auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
      InflateRect(&params->rgrc[0], -1, -1);
    }
    return res;
  }
  if (message == WM_NCPAINT && state) {
    const LRESULT res = DefSubclassProc(control, message, wparam, lparam);
    HDC dc = GetWindowDC(control);
    if (dc) {
      RECT wr{};
      GetWindowRect(control, &wr);
      RECT frame{0, 0, wr.right - wr.left, wr.bottom - wr.top};
      HBRUSH br = CreateSolidBrush(state->dark ? RGB(75, 82, 91) : RGB(190, 199, 208));
      FrameRect(dc, &frame, br);
      DeleteObject(br);
      ReleaseDC(control, dc);
    }
    return res;
  }
  if (message == WM_SIZE) {
    const LRESULT res = DefSubclassProc(control, message, wparam, lparam);
    update_rich_insets(control);
    return res;
  }
  if (message == WM_PAINT && state && state->font &&
      control == state->base_name &&
      GetWindowTextLengthW(control) == 0 && !state->base_name_cue.empty()) {
    const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
    RECT rc{};
    GetClientRect(control, &rc);
    const UINT dpi = GetDpiForWindow(control);
    rc.left += MulDiv(6, dpi, 96);
    rc.right -= MulDiv(6, dpi, 96);
    HDC dc = GetDC(control);
    if (dc) {
      SetTextColor(dc, state->muted_text_color);
      draw_mixed_text(dc, state->font, state->cjk_font,
                      ui::render(state->base_name_cue), rc,
                      DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
      ReleaseDC(control, dc);
    }
    return result;
  }
  if (message == WM_PASTE && state &&
      (control == state->base_name || control == state->name_suffix)) {
    if (OpenClipboard(control)) {
      std::wstring plain;
      if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* ptr = static_cast<const wchar_t*>(GlobalLock(handle))) {
          plain = ptr;
          GlobalUnlock(handle);
        }
      }
      CloseClipboard();
      plain.erase(std::remove_if(plain.begin(), plain.end(), [](wchar_t ch) {
        return ch == L'\r' || ch == L'\n';
      }), plain.end());
      if (!plain.empty()) {
        HDC dc = GetDC(control);
        const auto runs = split_font_runs(dc, state->font, plain);
        if (dc) ReleaseDC(control, dc);
        const wchar_t* eng_family = ui_english_font_family();
        const wchar_t* cjk_family = ui_cjk_font_family();
        for (const auto& run : runs) {
          set_rich_selection_format(control, run.use_cjk ? cjk_family : eng_family,
                                    state->text_color);
          std::wstring chunk(run.text);
          SendMessageW(control, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(chunk.c_str()));
        }
      }
    }
    return 0;
  }
  if (message == WM_CHAR && wparam == L'\r' &&
      (GetWindowLongPtrW(control, GWL_STYLE) & ES_MULTILINE) == 0) {
    return 0;
  }
  if (control == state->base_name &&
      (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_CHAR ||
       message == WM_SETTEXT || message == EM_REPLACESEL)) {
    const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
    InvalidateRect(control, nullptr, TRUE);
    return result;
  }
  return DefSubclassProc(control, message, wparam, lparam);
}

void refresh_inspector(AppState* state) {
  set_rich_text_mixed(state->inspector, ui::render(state->inspector_text),
                      state->font, state->text_color, true);
}

void set_inspector_text(AppState* state, const std::wstring& canonical,
                        bool scroll_top = true) {
  state->inspector_text = canonical;
  set_rich_text_mixed(state->inspector, ui::render(canonical),
                      state->font, state->text_color, !scroll_top);
}

void set_base_name_cue(AppState* state, const std::wstring& canonical) {
  state->base_name_cue = canonical;
  if (state->base_name) InvalidateRect(state->base_name, nullptr, TRUE);
}

void set_rich_editable_text(AppState* state, HWND rich, const std::wstring& text) {
  set_rich_text_mixed(rich, text, state->font, state->text_color, false);
}

void refresh_activity(AppState* state) {
  set_rich_text_mixed(state->log, ui::render(state->activity_text),
                      state->font, state->text_color, true);
}

void append_log(AppState* state, const std::wstring& line) {
  const int length = GetWindowTextLengthW(state->log);
  const std::wstring canonical = (length > 0 ? L"\r\n" : L"") + line;
  state->activity_text += canonical;
  append_rich_mixed(state->log, ui::render(canonical), state->font, state->text_color);
  SendMessageW(state->log, EM_SCROLLCARET, 0, 0);
}

std::wstring metadata_status_label(const std::string& status) {
  if (status == "present") return HB_UI("Present");
  if (status == "absent") return HB_UI("Absent");
  if (status == "read-error") return HB_UI("Read error");
  return HB_UI("Unsupported");
}

std::wstring primaries_label(uint16_t value) {
  if (value == 9) return L"BT.2020";
  if (value == 12) return L"Display P3 / P3-D65";
  if (value == 1) return HB_UI("BT.709 / sRGB primaries");
  return HB_UI("Unspecified / other primaries");
}

std::wstring transfer_label(uint16_t value) {
  if (value == 1) return L"BT.709";
  if (value == 16) return L"PQ / ST 2084";
  if (value == 18) return L"HLG / BT.2100";
  if (value == 13) return HB_UI("sRGB transfer");
  if (value == 14) return L"BT.2020 SDR";
  if (value == 8) return HB_UI("Linear");
  return HB_UI("Unspecified / other transfer");
}

std::wstring matrix_label(uint16_t value) {
  if (value == 0) return HB_UI("RGB / identity");
  if (value == 9) return HB_UI("BT.2020 non-constant luminance");
  return HB_UI("Matrix ") + std::to_wstring(value);
}

std::wstring source_summary(const hdrbridge::SourceInfo& i) {
  std::wostringstream out;
  out << HB_UI("IMAGE\r\n") << i.width << L" × " << i.height
      << L"\r\n" << widen(i.format) << L"  •  " << widen(i.asset_kind)
      << HB_UI("\r\n\r\nCONTAINER\r\n") << widen(i.container_brand)
      << L"\r\n" << widen(i.codec);

  if (i.gain_map_present) {
    out << HB_UI("\r\n\r\nBASE RENDITION\r\n")
        << i.base_width << L" × " << i.base_height << L"  •  "
        << i.base_bit_depth << L"-bit  •  " << widen(i.base_codec)
        << L"  •  " << i.base_channels << HB_UI(" channels")
        << L"\r\n" << widen(i.base_color_space) << L"  •  "
        << widen(i.base_transfer) << L"  •  "
        << (!i.range_known ? HB_UI("Unknown range") : i.full_range ? HB_UI("Full range") : HB_UI("Limited range"))
        << HB_UI("\r\nSource color  ") << primaries_label(i.primaries)
        << L"  •  " << transfer_label(i.transfer)
        << L"  •  " << matrix_label(i.matrix)
        << HB_UI("\r\n\r\nGAIN MAP\r\nFamily  ") << widen(i.gain_map_family)
        << HB_UI("\r\nMap  ") << i.gain_map_width << L" × " << i.gain_map_height
        << L"  •  " << (i.gain_map_channels == 3 ? HB_UI("RGB (3 channels)") :
                          i.gain_map_channels == 1 ? HB_UI("Mono (1 channel)") : HB_UI("Unknown channels"))
        << HB_UI("\r\nScale  ") << i.gain_map_scale_x << L"× × "
        << i.gain_map_scale_y << L"×"
        << HB_UI("\r\nCapacity  ") << i.hdr_capacity_min << L" – "
        << i.hdr_capacity_max << L"×  (log2 " << i.base_hdr_headroom
        << L" – " << i.alternate_hdr_headroom << L")"
        << HB_UI("\r\nGain min/max  R ") << i.gain_map_min[0] << L" / " << i.gain_map_max[0]
        << L"  G " << i.gain_map_min[1] << L" / " << i.gain_map_max[1]
        << L"  B " << i.gain_map_min[2] << L" / " << i.gain_map_max[2]
        << HB_UI("\r\nGamma  ") << i.gain_map_gamma[0] << L" / "
        << i.gain_map_gamma[1] << L" / " << i.gain_map_gamma[2];
    if (i.base_item_id || i.gain_map_item_id || i.tone_map_item_id) {
      out << HB_UI("\r\nItems  base ") << i.base_item_id << HB_UI(" / map ")
          << i.gain_map_item_id << L" / tmap " << i.tone_map_item_id;
    }
    if (!i.auxiliary_type.empty()) {
      out << HB_UI("\r\nAuxiliary type  ") << widen(i.auxiliary_type);
    }
    out << HB_UI("\r\n\r\nRECONSTRUCTED HDR\r\n")
        << i.width << L" × " << i.height << L"  •  "
        << widen(i.reconstructed_color_space) << L"  •  "
        << widen(i.reconstructed_transfer) << L"  •  "
        << widen(i.reconstructed_precision);
  } else {
    out << HB_UI("\r\n\r\nPRIMARY ITEM\r\n");
    if (i.is_grid) {
      out << HB_UI("Grid ") << i.grid_columns << L" × " << i.grid_rows
          << HB_UI("\r\nTile ") << i.tile_width << L" × " << i.tile_height;
    } else {
      out << HB_UI("Single image");
    }
    out << HB_UI("\r\n\r\nSOURCE SIGNAL\r\n") << i.bit_depth << L"-bit  "
        << widen(i.pixel_format) << L"  •  "
        << (!i.range_known ? HB_UI("Unknown range") : i.full_range ? HB_UI("Full range") : HB_UI("Limited range"))
        << HB_UI("\r\n\r\nNATIVE SIGNAL\r\n");
    if (i.native_color_present) {
      out << widen(i.native_color_description) << L"  " << i.native_primaries
          << L" / " << i.native_transfer << L" / " << i.native_matrix
          << L"\r\n" << primaries_label(i.native_primaries) << L" / "
          << transfer_label(i.native_transfer);
    } else {
      out << HB_UI("CICP/NCLX: Absent / Unspecified");
    }
    out << HB_UI("\r\n\r\nICC SIGNAL\r\n");
    if (i.icc_present) {
      out << HB_UI("Present\r\nProfile  ")
          << (i.icc_description.empty() ? HB_UI("Unreadable / unnamed") : widen(i.icc_description));
      if (!i.icc_version.empty()) out << HB_UI("\r\nICC version  ") << widen(i.icc_version);
      if (i.icc_cicp_present) {
        out << L"\r\nICC CICP  " << i.icc_primaries << L" / " << i.icc_transfer
            << L" / " << i.icc_matrix << L" / " << (i.icc_full_range ? 1 : 0)
            << L"\r\n" << primaries_label(i.icc_primaries) << L" / "
            << transfer_label(i.icc_transfer);
      } else {
        out << HB_UI("\r\nICC CICP  Absent");
        if (i.icc_transfer != 0 || i.icc_primaries != 0) {
          out << HB_UI("\r\nICC transform  ") << primaries_label(i.icc_primaries)
              << L" / " << transfer_label(i.icc_transfer);
        }
      }
      if (!i.icc_transfer_interpretation.empty()) {
        out << HB_UI("\r\nInterpretation  ") << widen(i.icc_transfer_interpretation);
      }
    } else {
      out << HB_UI("Absent");
    }
    out << HB_UI("\r\n\r\nRESOLVED COLOR\r\n")
        << primaries_label(i.primaries) << L" / " << transfer_label(i.transfer)
        << HB_UI("\r\nHDR signaling  ") << widen(i.resolved_signaling_source)
        << (i.color_signaling_conflict ? HB_UI(" — Color signaling conflict") : L"")
        << HB_UI("\r\nRepresentation  ") << widen(i.asset_kind)
        << HB_UI("\r\nPrimaries  ") << primaries_label(i.primaries)
        << HB_UI("\r\nTransfer   ") << transfer_label(i.transfer)
        << HB_UI("\r\nMatrix     ") << matrix_label(i.matrix);
  }
  out << HB_UI("\r\n\r\nSOURCE METADATA\r\nEXIF  ") << metadata_status_label(i.exif_status)
      << L"\r\nXMP   " << metadata_status_label(i.xmp_status)
      << L"\r\nICC   " << metadata_status_label(i.icc_status)
      << HB_UI("\r\nOrientation  ") << metadata_status_label(i.orientation_status)
      << HB_UI("  •  source ") << static_cast<int>(i.original_orientation)
      << HB_UI(" -> canonical ") << static_cast<int>(
          i.orientation_normalized ? 1 : i.original_orientation);
  return out.str();
}

std::wstring mode_for_index(int index) {
  switch (index) {
    case 1: return L"png-pq16";
    case 2: return L"tiff-pq16";
    case 3: return L"jxl-pq16";
    case 4: return L"jxr-scrgb-fp16";
    case 5: return L"avif-pq10";
    case 6: return L"gainmap-extract";
    case 7: return L"photo-frame";
    default: return L"ultrahdr";
  }
}

int index_for_mode(const std::wstring& mode) {
  if (mode == L"png-pq16") return 1;
  if (mode == L"tiff-pq16") return 2;
  if (mode == L"jxl-pq16") return 3;
  if (mode == L"jxr-scrgb-fp16") return 4;
  if (mode == L"avif-pq10") return 5;
  if (mode == L"gainmap-extract") return 6;
  if (mode == L"photo-frame") return 7;
  return 0;
}

std::wstring format_label_for_mode(const std::wstring& mode) {
  if (mode == L"ultrahdr") return L"Ultra HDR JPEG";
  if (mode == L"png-pq16") return L"HDR PNG";
  if (mode == L"jxl-pq16") return L"JPEG XL";
  if (mode == L"jxr-scrgb-fp16") return L"JPEG XR FP16";
  if (mode == L"avif-pq10") return L"HDR AVIF";
  if (mode == L"tiff-pq16") return L"HDR TIFF";
  if (mode == L"photo-frame") return HB_UI("Photo Frame");
  return HB_UI("Gain Map");
}

std::wstring source_format_label(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  if (!extension.empty() && extension.front() == L'.') extension.erase(extension.begin());
  std::transform(extension.begin(), extension.end(), extension.begin(), towupper);
  return extension.empty() ? HB_UI("Image") : extension;
}

std::wstring task_summary(const ConversionTask& task) {
  const wchar_t* marker = HB_UI("Pending");
  if (task.status == QueueStatus::running) marker = HB_UI("PROCESSING");
  else if (task.status == QueueStatus::success) marker = HB_UI("Complete");
  else if (task.status == QueueStatus::skipped) marker = HB_UI("Skipped");
  else if (task.status == QueueStatus::failed) marker = HB_UI("Failed");
  std::wstring output_label = format_label_for_mode(task.options.mode);
  if (task.options.conversion.output_representation == "gainmap" &&
      (task.options.mode == L"jxl-pq16" || task.options.mode == L"avif-pq10")) {
    output_label += HB_UI(" (Gain Map)");
  }
  return source_format_label(task.source) + L" → " + output_label +
      L"    " + marker + L"\r\n" +
      task.output.wstring();
}

void rebuild_task_list(AppState* state) {
  const LRESULT previous_top = SendMessageW(state->tasks, LB_GETTOPINDEX, 0, 0);
  const LRESULT previous_selection = SendMessageW(state->tasks, LB_GETCURSEL, 0, 0);
  ui::send(state->tasks, LB_RESETCONTENT, 0, 0);
  for (const auto& task : state->conversion_tasks) {
    const auto text = task_summary(task);
    ui::send(state->tasks, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(text.c_str()));
  }
  SendMessageW(state->tasks, LB_SETITEMHEIGHT, 0,
               MulDiv(52, GetDpiForWindow(state->window), 96));
  const LRESULT count = SendMessageW(state->tasks, LB_GETCOUNT, 0, 0);
  if (count > 0) {
    if (previous_selection != LB_ERR) {
      SendMessageW(state->tasks, LB_SETCURSEL,
                   std::min(previous_selection, count - 1), 0);
    }
    SendMessageW(state->tasks, LB_SETTOPINDEX,
                 std::min(std::max<LRESULT>(previous_top, 0), count - 1), 0);
  }
  InvalidateRect(state->tasks, nullptr, TRUE);
}

std::wstring suffix_for_mode(const std::wstring& mode, bool hlg = false,
                             bool gainmap = false) {
  if (mode == L"jxr-scrgb-fp16") return L"_scrgb-fp16.jxr";
  if (mode == L"ultrahdr") return L"_ultrahdr.jpg";
  if (mode == L"png-pq16") return hlg ? L"_hdr-hlg.png" : L"_hdr-pq16.png";
  if (mode == L"tiff-pq16") return L"_hdr-pq16.tif";
  if (mode == L"avif-pq10") return gainmap ? L"_gainmap.avif" :
      hlg ? L"_direct-hlg10.avif" : L"_direct-pq10.avif";
  if (mode == L"gainmap-extract") return L"_gainmap";
  if (mode == L"photo-frame") return L"_frame.jpg";
  return gainmap ? L"_gainmap.jxl" : hlg ? L"_hlg16.jxl" : L"_pq16.jxl";
}

std::wstring default_suffix_for_ui(int format_index, int transfer_index) {
  const auto mode = mode_for_index(format_index);
  if (mode == L"gainmap-extract") return L"_gainmap";
  if (mode == L"photo-frame") return L"_frame";
  const bool repr_output = format_index == 3 || format_index == 5;
  const bool repr_gainmap = repr_output && transfer_index == 2;
  const bool transfer_output = (format_index == 1 || repr_output) && !repr_gainmap;
  const bool hlg = transfer_index == 1 && transfer_output;
  const auto complete = suffix_for_mode(mode, hlg, repr_gainmap);
  const auto extension = hdrbridge::desktop::extension_for_mode(mode);
  return complete.substr(0, complete.size() - extension.size());
}

bool is_known_default_suffix(const std::wstring& suffix) {
  static constexpr const wchar_t* kDefaults[] = {
      L"_ultrahdr", L"_hdr-pq16", L"_hdr-hlg", L"_pq16", L"_hlg16",
      L"_gainmap", L"_scrgb-fp16", L"_direct-pq10", L"_direct-hlg10", L"_frame"};
  for (const wchar_t* candidate : kDefaults) {
    if (suffix == candidate) return true;
  }
  return false;
}

std::wstring control_text(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<size_t>(length) + 1u, L'\0');
  if (length) GetWindowTextW(control, value.data(), length + 1);
  value.resize(static_cast<size_t>(length));
  return value;
}

void request_preview(AppState* state, const std::filesystem::path& path);

void cancel_preview(AppState* state) {
  if (state->preview_cancel) state->preview_cancel->store(true);
  state->preview_worker.cancel_all();
}

void clear_preview(AppState* state) {
  cancel_preview(state);
  ++state->preview_generation;
  state->preview_source.clear();
  if (state->preview_renderer) state->preview_renderer->clear();
}

void update_output_preview(AppState* state) {
  const bool editing_task = state->editing_task >= 0 &&
      static_cast<size_t>(state->editing_task) < state->conversion_tasks.size();
  const auto& preview_source = editing_task
      ? state->conversion_tasks[static_cast<size_t>(state->editing_task)].source
      : state->input;
  if (preview_source.empty()) return;
  try {
    hdrbridge::desktop::NamingRequest request;
    request.source = preview_source;
    request.selected_folder = state->selected_folder;
    request.use_source_folder = state->use_source_folder;
    const auto entered_base = control_text(state->base_name);
    request.base_name = (!editing_task && state->inputs.size() > 1u)
        ? preview_source.stem().wstring()
        : (entered_base.empty() ? preview_source.stem().wstring() : entered_base);
    request.suffix = SendMessageW(state->use_suffix, BM_GETCHECK, 0, 0) == BST_CHECKED
        ? control_text(state->name_suffix) : L"";
    const int format_index = static_cast<int>(SendMessageW(state->format, CB_GETCURSEL, 0, 0));
    request.mode = mode_for_index(format_index);
    const int gainmap_format = static_cast<int>(SendMessageW(
        state->gainmap_export_format, CB_GETCURSEL, 0, 0));
    request.gainmap_export_format = gainmap_format == 1 ? L"png" :
        gainmap_format == 2 ? L"tiff" : gainmap_format == 3 ? L"jpeg" : L"original";
    if (request.mode == L"photo-frame") {
      request.gainmap_export_format = is_frame_png(state, format_index) ? L"png" : L"jpeg";
    }
    request.collision = SendMessageW(state->collision, CB_GETCURSEL, 0, 0) == 1
        ? hdrbridge::desktop::CollisionPolicy::overwrite
        : hdrbridge::desktop::CollisionPolicy::auto_number;
    state->output = hdrbridge::desktop::resolve_output_path(request);
    ui::set_text(state->output_path, state->output.c_str());
  } catch (const std::exception& error) {
    ui::set_text(state->output_path, ui::mark_known(widen(error.what())).c_str());
  }
}

TaskOptions capture_task_options(AppState* state) {
  TaskOptions captured;
  const int index = static_cast<int>(SendMessageW(state->format, CB_GETCURSEL, 0, 0));
  captured.mode = mode_for_index(index);
  auto& options = captured.conversion;
  options.mode = narrow(captured.mode);
  options.lossless = index == 1 || index == 2 ||
      SendMessageW(state->lossless, BM_GETCHECK, 0, 0) == BST_CHECKED;
  options.copy_exif = SendMessageW(state->copy_exif, BM_GETCHECK, 0, 0) == BST_CHECKED;
  options.copy_xmp = SendMessageW(state->copy_xmp, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const bool frame_png = is_frame_png(state, index);
  const int quality = static_cast<int>(SendMessageW(state->quality, TBM_GETPOS, 0, 0));
  options.image_quality = quality / 100.0f;
  options.base_quality = quality;
  options.gainmap_quality = quality;
  if (index == 1 || frame_png) options.png_compression_level = quality;
  if (index == 2) {
    options.tiff_compression_level = quality;
    options.tiff_compressed = SendMessageW(state->tiff_compression, CB_GETCURSEL, 0, 0) != 1;
  }
  constexpr int scales[] = {4, 2, 1};
  options.gainmap_scale = scales[std::clamp(static_cast<int>(
      SendMessageW(state->gainmap_resolution, CB_GETCURSEL, 0, 0)), 0, 2)];
  const int transfer = static_cast<int>(SendMessageW(state->transfer, CB_GETCURSEL, 0, 0));
  const bool non_jpeg_gainmap = (index == 3 || index == 5) && transfer == 2;
  options.multi_channel_gainmap = non_jpeg_gainmap ||
      SendMessageW(state->gainmap_channels, CB_GETCURSEL, 0, 0) == 1;
  const int gamut_index = static_cast<int>(SendMessageW(state->gamut, CB_GETCURSEL, 0, 0));
  const int gamut = gamut_index == CB_ERR ? 9 : static_cast<int>(
      SendMessageW(state->gamut, CB_GETITEMDATA, gamut_index, 0));
  options.output_gamut = (index == 0 || (index == 7 && !frame_png) || gamut == 12) ? "p3" : "rec2020";
  options.output_representation = (index == 3 || index == 5) && transfer == 2
      ? "gainmap" : "direct";
  options.output_transfer =
      (index == 1 || index == 3 || index == 5 || frame_png) && transfer == 1 ? "hlg" : "pq";
  options.overwrite = SendMessageW(state->collision, CB_GETCURSEL, 0, 0) == 1;
  const int gainmap_format = static_cast<int>(SendMessageW(
      state->gainmap_export_format, CB_GETCURSEL, 0, 0));
  options.gainmap_export_format = gainmap_format == 1 ? "png" :
      gainmap_format == 2 ? "tiff" : gainmap_format == 3 ? "jpeg" : "original";
  if (index == 7) {
    options.frame_format = frame_png ? "png" : "jpeg";
    options.lossless = frame_png; options.copy_exif = false; options.copy_xmp = false;
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    options.frame_font_path = narrow((std::filesystem::path(executable).parent_path() / L"assets" / L"fonts" / L"Roboto-Bold.ttf").wstring());
    options.frame_logo_path = narrow((std::filesystem::path(executable).parent_path() / L"assets" / L"frame-logos").wstring());
  }
  const bool editing_task = state->editing_task >= 0 &&
      static_cast<size_t>(state->editing_task) < state->conversion_tasks.size();
  captured.use_source_folder = state->use_source_folder;
  captured.selected_folder = state->selected_folder;
  captured.base_name = (!editing_task && state->inputs.size() > 1u)
      ? L"" : control_text(state->base_name);
  captured.suffix = control_text(state->name_suffix);
  captured.use_suffix = SendMessageW(state->use_suffix, BM_GETCHECK, 0, 0) == BST_CHECKED;
  captured.collision = options.overwrite ? hdrbridge::desktop::CollisionPolicy::overwrite
                                         : hdrbridge::desktop::CollisionPolicy::auto_number;
  return captured;
}

std::filesystem::path task_output_path(const std::filesystem::path& source,
                                       const TaskOptions& options) {
  hdrbridge::desktop::NamingRequest request;
  request.source = source;
  request.selected_folder = options.selected_folder;
  request.use_source_folder = options.use_source_folder;
  request.base_name = options.base_name.empty() ? source.stem().wstring() : options.base_name;
  request.suffix = options.use_suffix ? options.suffix : L"";
  request.mode = options.mode;
  request.gainmap_export_format = widen(options.mode == L"photo-frame" ? options.conversion.frame_format : options.conversion.gainmap_export_format);
  request.collision = options.collision;
  return hdrbridge::desktop::resolve_output_path(request);
}

size_t selected_source_index(const AppState* state) {
  if (state->inputs.empty()) return 0;
  const int caret = static_cast<int>(SendMessageW(state->queue, LB_GETCARETINDEX, 0, 0));
  if (caret != LB_ERR && static_cast<size_t>(caret) < state->inputs.size() &&
      SendMessageW(state->queue, LB_GETSEL, caret, 0) > 0) {
    return static_cast<size_t>(caret);
  }
  const int selected_count = static_cast<int>(SendMessageW(state->queue, LB_GETSELCOUNT, 0, 0));
  if (selected_count > 0) {
    int first_selected = LB_ERR;
    if (SendMessageW(state->queue, LB_GETSELITEMS, 1, reinterpret_cast<LPARAM>(&first_selected)) == 1 &&
        first_selected >= 0 && static_cast<size_t>(first_selected) < state->inputs.size()) {
      return static_cast<size_t>(first_selected);
    }
  }
  if (caret != LB_ERR && static_cast<size_t>(caret) < state->inputs.size()) {
    return static_cast<size_t>(caret);
  }
  return 0;
}

bool is_editing_task(const AppState* state) {
  return state->editing_task >= 0 &&
      static_cast<size_t>(state->editing_task) < state->conversion_tasks.size();
}

bool can_queue_or_update(const AppState* state) {
  if (state->running) return false;
  if (is_editing_task(state)) return true;
  if (state->inputs.empty()) return false;
  const int selected_count = static_cast<int>(SendMessageW(state->queue, LB_GETSELCOUNT, 0, 0));
  if (selected_count > 0) {
    std::vector<int> selected(static_cast<size_t>(selected_count));
    if (SendMessageW(state->queue, LB_GETSELITEMS, selected_count,
                     reinterpret_cast<LPARAM>(selected.data())) != LB_ERR) {
      for (const int index : selected) {
        if (index < 0 || static_cast<size_t>(index) >= state->inputs.size()) continue;
        if (static_cast<size_t>(index) >= state->input_statuses.size() ||
            state->input_statuses[static_cast<size_t>(index)].status != QueueStatus::failed) {
          return true;
        }
      }
      return false;
    }
  }
  return !state->input.empty();
}

void refresh_convert_button(AppState* state) {
  EnableWindow(state->convert, can_queue_or_update(state) ? TRUE : FALSE);
}

void restore_queue_add_ui(AppState* state) {
  ui::set_text(state->convert, HB_UI("Add to queue"));
  if (state->inputs.empty()) {
    state->input.clear();
    state->output.clear();
    clear_preview(state);
    ui::set_text(state->source_path, HB_UI("Drop or add supported HDR still images"));
    set_inspector_text(state, HB_UI("SOURCE INSPECTOR\r\n\r\nNo image selected"), true);
    ui::set_text(state->output_path, HB_UI("No output yet"));
    set_rich_editable_text(state, state->base_name, L"");
    set_base_name_cue(state, HB_UI("Base name"));
    refresh_convert_button(state);
    return;
  }
  const auto& selected_source = state->inputs[selected_source_index(state)];
  state->input = selected_source;
  const std::wstring source_text = selected_source.wstring();
  ui::send(state->source_path, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(source_text.c_str()));
  set_rich_editable_text(state, state->base_name,
                         state->inputs.size() > 1u ? std::wstring() : selected_source.stem().wstring());
  set_base_name_cue(state, state->inputs.size() > 1u ? HB_UI("Per-source base names")
                                                     : HB_UI("Base name"));
  if (state->use_source_folder) {
    ui::set_text(state->folder_path, selected_source.parent_path().c_str());
  } else if (!state->selected_folder.empty()) {
    ui::set_text(state->folder_path, state->selected_folder.c_str());
  }
  refresh_convert_button(state);
  update_output_preview(state);
  if (state->preview_enabled && state->preview_source != selected_source) {
    request_preview(state, selected_source);
  }
}

void cancel_task_edit(AppState* state) {
  if (state->editing_task >= 0) {
    state->editing_task = -1;
    restore_queue_add_ui(state);
  }
}

void add_or_update_task(AppState* state) {
  const bool editing = is_editing_task(state);
  if (!can_queue_or_update(state)) return;
  try {
    auto options = capture_task_options(state);
    if (editing) {
      auto& existing = state->conversion_tasks[static_cast<size_t>(state->editing_task)];
      ConversionTask task;
      task.source = existing.source;
      task.options = options;
      task.output = task_output_path(task.source, task.options);
      task.status = QueueStatus::pending;
      existing = std::move(task);
      state->editing_task = -1;
      restore_queue_add_ui(state);
    } else {
      const int selected_count = static_cast<int>(SendMessageW(state->queue, LB_GETSELCOUNT, 0, 0));
      std::vector<int> selected(static_cast<size_t>(std::max(selected_count, 0)));
      if (selected_count > 0) {
        SendMessageW(state->queue, LB_GETSELITEMS, selected_count,
                     reinterpret_cast<LPARAM>(selected.data()));
      }
      if (selected.empty()) {
        const auto found = std::find(state->inputs.begin(), state->inputs.end(), state->input);
        if (found != state->inputs.end()) selected.push_back(static_cast<int>(found - state->inputs.begin()));
      }
      for (const int source_index : selected) {
        if (source_index < 0 || static_cast<size_t>(source_index) >= state->inputs.size()) continue;
        ConversionTask task;
        task.source = state->inputs[static_cast<size_t>(source_index)];
        task.options = options;
        if (selected.size() > 1u || state->inputs.size() > 1u) task.options.base_name.clear();
        task.output = task_output_path(task.source, task.options);
        state->conversion_tasks.push_back(std::move(task));
      }
    }
    rebuild_task_list(state);
    EnableWindow(state->cancel_button, !state->conversion_tasks.empty());
    EnableWindow(state->remove_all_tasks, !state->conversion_tasks.empty());
  } catch (const std::exception& error) {
    ui::message_box(state->window, widen(error.what()).c_str(), L"HDR Bridge", MB_OK | MB_ICONERROR);
  }
}

void update_mode_ui(AppState* state, bool restore_saved_gamut) {
  const int index = static_cast<int>(SendMessageW(state->format, CB_GETCURSEL, 0, 0));
  if (state->last_format_index >= 0 && state->last_format_index != 1 &&
      state->last_format_index != 2 && state->last_format_index != 7) {
    const bool checked = SendMessageW(state->lossless, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state->lossless_setting = checked;
  }
  if (state->last_format_index == 0) {
    const int channels = static_cast<int>(
        SendMessageW(state->gainmap_channels, CB_GETCURSEL, 0, 0));
    if (channels != CB_ERR) state->uhdr_gainmap_channels = std::clamp(channels, 0, 1);
  }
  if (state->last_format_index != index) {
    if (state->last_format_index == 7) {
      state->frame_gainmap_resolution = static_cast<int>(SendMessageW(state->gainmap_resolution, CB_GETCURSEL, 0, 0));
      state->frame_gainmap_channels = static_cast<int>(SendMessageW(state->gainmap_channels, CB_GETCURSEL, 0, 0));
    } else if (state->last_format_index >= 0) {
      state->normal_gainmap_resolution = static_cast<int>(SendMessageW(state->gainmap_resolution, CB_GETCURSEL, 0, 0));
      state->normal_copy_exif = SendMessageW(state->copy_exif, BM_GETCHECK, 0, 0) == BST_CHECKED;
      state->normal_copy_xmp = SendMessageW(state->copy_xmp, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    SendMessageW(state->gainmap_resolution, CB_SETCURSEL, index == 7 ? state->frame_gainmap_resolution : state->normal_gainmap_resolution, 0);
    SendMessageW(state->copy_exif, BM_SETCHECK, index != 7 && state->normal_copy_exif ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state->copy_xmp, BM_SETCHECK, index != 7 && state->normal_copy_xmp ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  EnableWindow(state->copy_exif, index != 7);
  EnableWindow(state->copy_xmp, index != 7);
  int transfer_index = static_cast<int>(SendMessageW(state->transfer, CB_GETCURSEL, 0, 0));
  if (transfer_index == CB_ERR) transfer_index = saved_index(L"OutputTransfer", 0, 2);
  const bool representation_output = index == 3 || index == 5;
  if (!representation_output && transfer_index == 2) transfer_index = 0;
  ui::send(state->transfer, CB_RESETCONTENT, 0, 0);
  ui::send(state->transfer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(HB_UI("PQ / ST2084 — default")));
  ui::send(state->transfer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"HLG"));
  if (representation_output) {
    ui::send(state->transfer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(HB_UI("Gain Map")));
  }
  SendMessageW(state->transfer, CB_SETCURSEL, transfer_index, 0);
  const int representation_index = (index == 3 || index == 5) && transfer_index == 2 ? 1 : 0;
  const bool frame_mode = index == 7;
  const int frame_export_index = std::clamp(static_cast<int>(
      SendMessageW(state->frame_export_format, CB_GETCURSEL, 0, 0)), 0, 1);
  const bool frame_png = frame_mode && frame_export_index == 1;
  const bool frame_jpeg = frame_mode && !frame_png;
  const bool ultrahdr_output = index == 0 || frame_jpeg;
  const bool gainmap_output = ultrahdr_output ||
      (representation_output && representation_index == 1);
  const bool transfer_output =
      (index == 1 || index == 3 || index == 5 || frame_png) && !gainmap_output;
  const int previous_index = static_cast<int>(SendMessageW(state->gamut, CB_GETCURSEL, 0, 0));
  const int previous_gamut = previous_index == CB_ERR ? -1 : static_cast<int>(
      SendMessageW(state->gamut, CB_GETITEMDATA, previous_index, 0));
  ShowWindow(state->transfer, (transfer_output || representation_output) ? SW_SHOW : SW_HIDE);
  ShowWindow(state->representation, SW_HIDE);
  ShowWindow(state->lossless, SW_SHOW);
  ShowWindow(state->quality, SW_SHOW);
  ShowWindow(state->quality_value, SW_SHOW);
  ShowWindow(state->gainmap_resolution, gainmap_output ? SW_SHOW : SW_HIDE);
  ShowWindow(state->gainmap_channels, gainmap_output ? SW_SHOW : SW_HIDE);
  ShowWindow(state->tiff_compression, index == 2 ? SW_SHOW : SW_HIDE);
  ShowWindow(state->gainmap_export_format, index == 6 ? SW_SHOW : SW_HIDE);
  ShowWindow(state->frame_export_format, frame_mode ? SW_SHOW : SW_HIDE);
  const bool selectable_gamut = index == 1 || index == 2 || representation_output || frame_png;
  ShowWindow(state->gamut, (ultrahdr_output || selectable_gamut) ? SW_SHOW : SW_HIDE);
  ui::send(state->gamut, CB_RESETCONTENT, 0, 0);
  const auto add_gamut = [&](const wchar_t* label, int cicp) {
    const int item = static_cast<int>(ui::send(
        state->gamut, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)));
    SendMessageW(state->gamut, CB_SETITEMDATA, item, cicp);
  };
  if (ultrahdr_output) {
    add_gamut(L"Display P3", 12);
  } else if (gainmap_output) {
    add_gamut(HB_UI("Display P3 — default"), 12);
    add_gamut(L"Rec.2020", 9);
  } else {
    add_gamut(HB_UI("Rec.2020 — default"), 9);
    add_gamut(L"Display P3", 12);
  }
  int saved_gamut = -1;
  if (restore_saved_gamut && state->last_format_index < 0) {
    if (const auto value = read_layout_value(L"OutputGamutCicp")) {
      saved_gamut = static_cast<int>(*value);
    }
  }
  const bool representation_changed = state->last_representation_index != representation_index;
  const bool frame_export_changed = frame_mode && state->last_frame_export_index != frame_export_index;
  const int default_gamut = gainmap_output ? 12 : 9;
  const int desired_gamut = saved_gamut > 0 ? saved_gamut :
      (state->last_format_index != index || representation_changed ||
       frame_export_changed || previous_gamut < 0)
      ? default_gamut : previous_gamut;
  int selected_gamut = 0;
  const int gamut_count = static_cast<int>(SendMessageW(state->gamut, CB_GETCOUNT, 0, 0));
  for (int item = 0; item < gamut_count; ++item) {
    if (static_cast<int>(SendMessageW(state->gamut, CB_GETITEMDATA, item, 0)) == desired_gamut) {
      selected_gamut = item; break;
    }
  }
  SendMessageW(state->gamut, CB_SETCURSEL, selected_gamut, 0);
  EnableWindow(state->frame_export_format, frame_mode);
  EnableWindow(state->gamut, ultrahdr_output || selectable_gamut);
  EnableWindow(state->transfer, transfer_output || representation_output);
  EnableWindow(state->gainmap_resolution, gainmap_output);
  if (gainmap_output && !ultrahdr_output) {
    SendMessageW(state->gainmap_channels, CB_SETCURSEL, 1, 0);
  } else if (ultrahdr_output) {
    SendMessageW(state->gainmap_channels, CB_SETCURSEL,
                 index == 7 ? state->frame_gainmap_channels : state->uhdr_gainmap_channels, 0);
  }
  EnableWindow(state->gainmap_channels, ultrahdr_output);
  ui::set_text(state->lossless, HB_UI("Lossless"));
  SendMessageW(state->lossless, BM_SETCHECK,
               ((index != 7 && (index == 1 || index == 2 || state->lossless_setting)) || frame_png)
                   ? BST_CHECKED : BST_UNCHECKED, 0);
  EnableWindow(state->lossless, index == 3 || index == 4);
  const int slot = encoding_slot_for(state, index);
  const bool compression = is_compression_slot(slot);
  if (state->last_encoding_slot != slot) {
    if (state->last_encoding_slot >= 0 && state->last_encoding_slot <
        static_cast<int>(state->encoding_values.size())) {
      state->encoding_values[static_cast<size_t>(state->last_encoding_slot)] =
          static_cast<int>(SendMessageW(state->quality, TBM_GETPOS, 0, 0));
    }
    SendMessageW(state->quality, TBM_SETRANGE, TRUE,
                 compression ? MAKELPARAM(1, 9) : MAKELPARAM(1, 100));
    SendMessageW(state->quality, TBM_SETPAGESIZE, 0, compression ? 1 : 5);
    const int value = state->encoding_values[static_cast<size_t>(slot)];
    SendMessageW(state->quality, TBM_SETPOS, TRUE, value);
    ui::set_text(state->quality_value,
                   (std::to_wstring(value) + (compression ? L"" : L"%")).c_str());
    state->last_encoding_slot = slot;
  }
  const bool lossy = index == 0 || index == 5 || frame_jpeg ||
                     SendMessageW(state->lossless, BM_GETCHECK, 0, 0) != BST_CHECKED;
  const bool compression_level_enabled = index != 2 || state->tiff_compressed;
  const bool gainmap_jpeg = index == 6 &&
      SendMessageW(state->gainmap_export_format, CB_GETCURSEL, 0, 0) == 3;
  EnableWindow(state->quality, ((lossy || compression) && index != 6 || gainmap_jpeg) &&
                               compression_level_enabled);
  EnableWindow(state->quality_value, ((lossy || compression) && index != 6 || gainmap_jpeg) &&
                                     compression_level_enabled);
  if (state->last_format_index != index || state->last_transfer_index != transfer_index ||
      frame_export_changed) {
    const auto current_suffix = control_text(state->name_suffix);
    const bool should_update_suffix = state->last_format_index < 0
        ? is_known_default_suffix(current_suffix)
        : current_suffix == default_suffix_for_ui(
              state->last_format_index, std::max(0, state->last_transfer_index));
    if (should_update_suffix) {
      set_rich_editable_text(state, state->name_suffix,
                             default_suffix_for_ui(index, transfer_index));
    }
    state->last_format_index = index;
    state->last_transfer_index = transfer_index;
  }
  state->last_representation_index = representation_index;
  state->last_frame_export_index = frame_export_index;
  update_output_preview(state);
  RECT client{};
  GetClientRect(state->window, &client);
  layout(state, client.right, client.bottom);
}

void layout(AppState* s, int width, int height) {
  const int margin = 24, gap = 24;
  const UINT dpi = GetDpiForWindow(s->window);
  const int pad = MulDiv(4, dpi, 96);
  HDC dc = GetDC(s->window);
  SIZE title_size{}, version_size{};
  TEXTMETRICW title_metrics{}, version_metrics{};
  const auto previous_font = SelectObject(dc, s->title_font);
  GetTextExtentPoint32W(dc, L"HDR Bridge", 10, &title_size);
  GetTextMetricsW(dc, &title_metrics);
  SelectObject(dc, s->version_font ? s->version_font : s->font);
  wchar_t version_text[256]{};
  GetWindowTextW(s->version, version_text, 256);
  GetTextExtentPoint32W(dc, version_text, static_cast<int>(wcslen(version_text)), &version_size);
  GetTextMetricsW(dc, &version_metrics);
  SelectObject(dc, previous_font);
  ReleaseDC(s->window, dc);
  const int title_y = MulDiv(7, dpi, 96);
  const int baseline = title_y + title_metrics.tmAscent;
  const int version_y = baseline - version_metrics.tmAscent;
  const int header = std::max(42, static_cast<int>(std::max(title_y + title_metrics.tmHeight, version_y + version_metrics.tmHeight) + pad));
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
  MoveWindow(s->title, margin, title_y, title_size.cx + pad, title_metrics.tmHeight + pad, TRUE);
  MoveWindow(s->version, margin + title_size.cx + pad * 2, version_y,
             version_size.cx + pad * 2, version_metrics.tmHeight + pad, TRUE);
  constexpr int reset_button_width = 144;
  constexpr int reset_button_gap = 8;
  constexpr int preview_button_width = 92;
  constexpr int preview_button_gap = 8;
  MoveWindow(s->preview_toggle, width - margin - preview_button_width, 7,
             preview_button_width, 30, TRUE);
  MoveWindow(s->reset_settings,
             width - margin - preview_button_width - preview_button_gap - reset_button_width, 7,
             reset_button_width, 30, TRUE);
  MoveWindow(s->reset_layout,
             width - margin - reset_button_width * 2 - reset_button_gap - preview_button_width - preview_button_gap, 7,
             reset_button_width, 30, TRUE);
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
  s->files_panel_height = std::clamp(s->files_panel_height, 150,
                                     std::max(150, available_left - 150));
  const int queue_height = s->files_panel_height;
  MoveWindow(s->queue, margin, content_top + 38, left_width, queue_height, TRUE);
  MoveWindow(s->source_path, margin, content_top + 44 + queue_height, left_width, 26, TRUE);
  MoveWindow(s->files_splitter, margin, content_top + 72 + queue_height,
             left_width, 6, TRUE);
  MoveWindow(s->inspector_label, margin, content_top + 84 + queue_height, left_width, 26, TRUE);
  const int inspector_y = content_top + 114 + queue_height;
  MoveWindow(s->inspector, margin, inspector_y, left_width,
             std::max(110, content_bottom - inspector_y), TRUE);

  MoveWindow(s->output_label, right_x, content_top + 3, right_width, 26, TRUE);
  MoveWindow(s->format, right_x, content_top + 36, right_width, 220, TRUE);
  MoveWindow(s->representation, right_x, content_top + 72, right_width, 150, TRUE);
  const int format_index = static_cast<int>(SendMessageW(s->format, CB_GETCURSEL, 0, 0));
  const bool non_jpeg_gainmap = (format_index == 3 || format_index == 5) &&
      SendMessageW(s->transfer, CB_GETCURSEL, 0, 0) == 2;
  const int representation_offset = non_jpeg_gainmap ? 34 : 0;
  const int color_row_y = content_top + 72;
  const int gainmap_option_gap = 8;
  const int gainmap_option_width = (right_width - gainmap_option_gap) / 2;
  if (format_index == 7) {
    MoveWindow(s->frame_export_format, right_x, color_row_y,
               gainmap_option_width, 150, TRUE);
    MoveWindow(s->gamut, right_x + gainmap_option_width + gainmap_option_gap,
               color_row_y, right_width - gainmap_option_width - gainmap_option_gap, 150, TRUE);
  } else {
    MoveWindow(s->gamut, right_x, color_row_y, right_width, 150, TRUE);
  }
  const int transfer_row_y = non_jpeg_gainmap ? content_top + 140 : content_top + 106;
  const int parameter_row_y = content_top + 106;
  MoveWindow(s->transfer, right_x, transfer_row_y, right_width, 170, TRUE);
  MoveWindow(s->gainmap_resolution, right_x, parameter_row_y,
             gainmap_option_width, 150, TRUE);
  MoveWindow(s->gainmap_channels,
             right_x + gainmap_option_width + gainmap_option_gap,
             parameter_row_y, right_width - gainmap_option_width - gainmap_option_gap, 150, TRUE);
  MoveWindow(s->tiff_compression, right_x, content_top + 106, right_width, 150, TRUE);
  MoveWindow(s->gainmap_export_format, right_x, content_top + 72, right_width, 150, TRUE);
  const int lower_offset = representation_offset;
  const int option_gap = 8;
  const int option_width = (right_width - option_gap * 2) / 3;
  MoveWindow(s->lossless, right_x, content_top + 142 + lower_offset, option_width, 28, TRUE);
  MoveWindow(s->copy_exif, right_x + option_width + option_gap,
             content_top + 142 + lower_offset, option_width, 28, TRUE);
  MoveWindow(s->copy_xmp, right_x + (option_width + option_gap) * 2,
             content_top + 142 + lower_offset, option_width, 28, TRUE);
  MoveWindow(s->quality, right_x, content_top + 174 + lower_offset, right_width - 62, 24, TRUE);
  MoveWindow(s->quality_value, right_x + right_width - 54, content_top + 174 + lower_offset, 54, 24, TRUE);
  MoveWindow(s->source_folder, right_x, content_top + 206 + lower_offset, 160, 28, TRUE);
  MoveWindow(s->choose_folder, right_x + 168, content_top + 204 + lower_offset, 104, 32, TRUE);
  MoveWindow(s->folder_path, right_x + 282, content_top + 208 + lower_offset, right_width - 282, 24, TRUE);
  MoveWindow(s->base_name, right_x, content_top + 242 + lower_offset, right_width * 57 / 100, 30, TRUE);
  MoveWindow(s->name_suffix, right_x + right_width * 59 / 100, content_top + 242 + lower_offset,
             right_width * 41 / 100, 30, TRUE);
  const int collision_width = right_width * 72 / 100;
  MoveWindow(s->collision, right_x, content_top + 278 + lower_offset, collision_width, 110, TRUE);
  MoveWindow(s->use_suffix, right_x + collision_width + 8, content_top + 278 + lower_offset,
             right_width - collision_width - 8, 30, TRUE);
  const int action_gap = 10;
  constexpr int action_height = 38;
  const int task_action_width = (right_width - action_gap * 2) / 3;
  MoveWindow(s->convert, right_x, content_top + 316 + lower_offset,
             task_action_width, action_height, TRUE);
  MoveWindow(s->remove_all_tasks,
             right_x + task_action_width + action_gap,
             content_top + 316 + lower_offset,
             task_action_width, action_height, TRUE);
  MoveWindow(s->cancel_button,
             right_x + (task_action_width + action_gap) * 2,
             content_top + 316 + lower_offset,
             right_width - task_action_width * 2 - action_gap * 2,
             action_height, TRUE);
  MoveWindow(s->tasks_label, right_x, content_top + 362 + lower_offset, right_width, 24, TRUE);
  MoveWindow(s->tasks, right_x, content_top + 388 + lower_offset, right_width,
             std::max(100, content_bottom - (content_top + 388 + lower_offset)), TRUE);

  const int status_y = split_y + 12;
  MoveWindow(s->progress, margin, status_y, width - margin * 2, 8, TRUE);
  MoveWindow(s->activity_label, margin, status_y + 16, width - margin * 2, 24, TRUE);
  MoveWindow(s->log, margin, status_y + 44, width - margin * 2,
             height - (status_y + 44) - margin, TRUE);
  RedrawWindow(s->title, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

bool supported_source(const std::filesystem::path& path) {
  const auto extension = path.extension().wstring();
  constexpr const wchar_t* extensions[] = {L".hif", L".heic", L".heif", L".hsp", L".avif", L".jxl", L".jxr", L".wdp", L".hdp", L".png", L".tif", L".tiff", L".jpg", L".jpeg", L".jpe"};
  return std::any_of(std::begin(extensions), std::end(extensions),
                     [&](const wchar_t* value) { return _wcsicmp(extension.c_str(), value) == 0; });
}

std::wstring pending_queue_summary(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  if (!extension.empty() && extension.front() == L'.') extension.erase(extension.begin());
  std::transform(extension.begin(), extension.end(), extension.begin(), towupper);
  return path.filename().wstring() + L"    " + extension + HB_UI(" • Metadata pending");
}

std::wstring queue_result_summary(const std::filesystem::path& path,
                                  QueueStatus status) {
  if (status == QueueStatus::success) {
    return path.filename().wstring() + HB_UI("    Converted");
  }
  if (status == QueueStatus::skipped) {
    return path.filename().wstring() + HB_UI("    Skipped \u2014 No HDR data");
  }
  if (status == QueueStatus::failed) {
    return path.filename().wstring() + HB_UI("    Failed \u2014 Decode error");
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
  ui::send(queue, LB_DELETESTRING, index, 0);
  ui::send(queue, LB_INSERTSTRING, index, reinterpret_cast<LPARAM>(text.c_str()));
  for (int item = 0; item < count; ++item) {
    if (selected[static_cast<size_t>(item)]) SendMessageW(queue, LB_SETSEL, TRUE, item);
  }
  if (caret != LB_ERR) SendMessageW(queue, LB_SETCARETINDEX, caret, FALSE);
  if (top != LB_ERR) SendMessageW(queue, LB_SETTOPINDEX, top, 0);
}

void request_preview(AppState* state, const std::filesystem::path& path) {
  if (!state->preview_enabled || !state->preview_renderer || path.empty()) return;
  cancel_preview(state);
  const uint64_t generation = ++state->preview_generation;
  state->preview_source = path;
  const auto cancel = std::make_shared<std::atomic_bool>(false);
  state->preview_cancel = cancel;
  state->preview_renderer->show(true);
  state->preview_worker.submit(state->window, generation, path, cancel);
}

void load_source(AppState* state, const std::filesystem::path& path) {
  state->editing_task = -1;
  ui::set_text(state->convert, HB_UI("Add to queue"));
  try {
    if (!supported_source(path)) {
      throw std::runtime_error("Select a supported HDR HIF/HEIF, UHDR, PNG, TIFF, AVIF, JXL, JXR, or HSP asset.");
    }
    ui::set_text(state->status, HB_UI("Inspecting source…"));
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
    if (cached_index < state->input_statuses.size() &&
        state->input_statuses[cached_index].status == QueueStatus::failed) {
      state->input_statuses[cached_index].status = QueueStatus::pending;
      InvalidateRect(state->queue, nullptr, FALSE);
    }
    const std::wstring path_text = path.wstring();
    const std::wstring inspector_text = source_summary(info);
    ui::send(state->source_path, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path_text.c_str()));
    set_inspector_text(state, inspector_text, true);
    ui::set_text(state->status, HB_UI("Source ready"));
    set_rich_editable_text(state, state->base_name,
                           state->inputs.size() > 1u ? std::wstring() : path.stem().wstring());
    set_base_name_cue(state, state->inputs.size() > 1u ? HB_UI("Per-source base names")
                                                       : HB_UI("Base name"));
    if (state->use_source_folder) {
      ui::set_text(state->folder_path, path.parent_path().c_str());
    } else if (!state->selected_folder.empty()) {
      ui::set_text(state->folder_path, state->selected_folder.c_str());
    }
    update_mode_ui(state);
    if (state->preview_source != path) request_preview(state, path);
    refresh_convert_button(state);
    append_log(state, HB_UI("Source inspected: ") + std::to_wstring(info.width) + L"×" + std::to_wstring(info.height) +
                      L", " + widen(info.chroma) + L", " + std::to_wstring(info.bit_depth) + L"-bit, CICP " +
                      std::to_wstring(info.primaries) + L"/" + std::to_wstring(info.transfer) + L"/" + std::to_wstring(info.matrix));
  } catch (const std::exception& e) {
    state->input.clear();
    state->output.clear();
    clear_preview(state);
    const auto found = std::find(state->inputs.begin(), state->inputs.end(), path);
    if (found != state->inputs.end()) {
      const size_t failed_index = static_cast<size_t>(found - state->inputs.begin());
      if (failed_index < state->input_statuses.size()) {
        state->input_statuses[failed_index].status = QueueStatus::failed;
        InvalidateRect(state->queue, nullptr, FALSE);
      }
    }
    const std::wstring path_text = path.wstring();
    ui::send(state->source_path, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path_text.c_str()));
    ui::set_text(state->output_path, HB_UI("No output yet"));
    set_base_name_cue(state, state->inputs.size() > 1u ? HB_UI("Per-source base names")
                                                       : HB_UI("Base name"));
    if (state->inputs.size() > 1u) {
      set_rich_editable_text(state, state->base_name, L"");
    }
    refresh_convert_button(state);
    ui::set_text(state->status, HB_UI("Inspection failed"));
    ui::message_box(state->window, widen(e.what()).c_str(), L"HDR Bridge", MB_OK | MB_ICONERROR);
  }
}

void add_sources(AppState* state, const std::vector<std::filesystem::path>& paths) {
  if (state->running) return;
  size_t first_added = state->inputs.size();
  for (const auto& path : paths) {
    if (!supported_source(path)) {
      append_log(state, HB_UI("Skipped unsupported file: ") + path.wstring());
      continue;
    }
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    if (std::find(state->inputs.begin(), state->inputs.end(), absolute) != state->inputs.end()) continue;
    state->inputs.push_back(absolute);
    state->input_infos.emplace_back(std::nullopt);
    state->input_statuses.emplace_back();
    const auto label = pending_queue_summary(absolute);
    ui::send(state->queue, LB_ADDSTRING, 0,
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
  ui::set_text(state->convert, HB_UI("Add to queue"));
  append_log(state, HB_UI("Batch queue: ") + std::to_wstring(state->inputs.size()) + HB_UI(" file(s), sequential processing"));
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
    ui::message_box(state->window, HB_UI("The Windows file picker could not be created."),
                L"HDR Bridge", MB_OK | MB_ICONERROR);
    return;
  }
  const COMDLG_FILTERSPEC canonical_filters[] = {
      {HB_UI("All supported HDR images"), L"*.avif;*.heif;*.heic;*.hif;*.hsp;*.jpg;*.jpeg;*.jpe;*.jxl;*.jxr;*.wdp;*.hdp;*.png;*.tif;*.tiff"},
      {L"AVIF (*.avif)", L"*.avif"},
      {L"HEIF (*.heif;*.heic;*.hif)", L"*.heif;*.heic;*.hif"},
      {L"Panasonic HLG Photo (*.hsp)", L"*.hsp"},
      {L"JPEG (*.jpg;*.jpeg;*.jpe)", L"*.jpg;*.jpeg;*.jpe"},
      {L"JPEG XL (*.jxl)", L"*.jxl"},
      {L"JPEG XR (*.jxr;*.wdp;*.hdp)", L"*.jxr;*.wdp;*.hdp"},
      {L"PNG (*.png)", L"*.png"},
      {L"TIFF (*.tif;*.tiff)", L"*.tif;*.tiff"},
      {HB_UI("All files (*.*)"), L"*.*"}};
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST |
                     FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
  std::vector<std::wstring> filter_names;
  std::vector<COMDLG_FILTERSPEC> filters;
  filter_names.reserve(std::size(canonical_filters));
  for (const auto& filter : canonical_filters) filter_names.push_back(ui::render(filter.pszName));
  for (size_t index = 0; index < filter_names.size(); ++index)
    filters.push_back({filter_names[index].c_str(), canonical_filters[index].pszSpec});
  dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
  dialog->SetFileTypeIndex(1);
  dialog->SetTitle(ui::render(HB_UI("Add HDR images")).c_str());
  dialog->SetOkButtonLabel(ui::render(HB_UI("Add")).c_str());
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
    ui::message_box(state->window, HB_UI("The Windows file picker failed."),
                L"HDR Bridge", MB_OK | MB_ICONERROR);
  }
  dialog->Release();
}

void choose_output_folder(AppState* state) {
  IFileOpenDialog* dialog = nullptr;
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog));
  if (FAILED(result)) {
    ui::message_box(state->window, HB_UI("The Windows folder picker could not be created."),
                L"HDR Bridge", MB_OK | MB_ICONERROR);
    return;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(ui::render(HB_UI("Choose output folder")).c_str());
  dialog->SetOkButtonLabel(ui::render(HB_UI("Select folder")).c_str());
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
        ui::set_text(state->folder_path, path.c_str());
        SendMessageW(state->source_folder, BM_SETCHECK, BST_UNCHECKED, 0);
        update_output_preview(state);
      }
    }
  } else if (result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    ui::message_box(state->window, HB_UI("The Windows folder picker failed."),
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
  bool removed_preview = false;
  for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
    const int index = *it;
    if (index < 0 || static_cast<size_t>(index) >= state->inputs.size()) continue;
    const auto& removed = state->inputs[static_cast<size_t>(index)];
    if (removed == state->preview_source) {
      removed_preview = true;
      cancel_preview(state);
    }
    hdrbridge::discard_cached_input(removed);
    state->inputs.erase(state->inputs.begin() + index);
    if (static_cast<size_t>(index) < state->input_infos.size()) {
      state->input_infos.erase(state->input_infos.begin() + index);
    }
    if (static_cast<size_t>(index) < state->input_statuses.size()) {
      state->input_statuses.erase(state->input_statuses.begin() + index);
    }
    ui::send(state->queue, LB_DELETESTRING, index, 0);
  }
  if (removed_preview || state->inputs.empty()) clear_preview(state);
  if (state->inputs.empty()) {
    hdrbridge::clear_input_cache();
    state->editing_task = -1;
    state->input.clear();
    state->output.clear();
    ui::set_text(state->source_path, HB_UI("Drop or add supported HDR still images"));
    set_inspector_text(state, HB_UI("SOURCE INSPECTOR\r\n\r\nNo image selected"), true);
    ui::set_text(state->output_path, HB_UI("No output yet"));
    ui::set_text(state->status, HB_UI("Ready"));
    ui::set_text(state->convert, HB_UI("Add to queue"));
    set_base_name_cue(state, HB_UI("Base name"));
    refresh_convert_button(state);
    EnableWindow(state->remove, FALSE);
    EnableWindow(state->clear, FALSE);
    return;
  }
  const auto current = std::find(state->inputs.begin(), state->inputs.end(), state->input);
  const int next = current == state->inputs.end()
      ? std::min(first_removed, static_cast<int>(state->inputs.size()) - 1)
      : static_cast<int>(current - state->inputs.begin());
  SendMessageW(state->queue, LB_SETSEL, TRUE, next);
  SendMessageW(state->queue, LB_SETCARETINDEX, next, FALSE);
  load_source(state, state->inputs[static_cast<size_t>(next)]);
}

void clear_sources(AppState* state) {
  if (state->running) return;
  state->editing_task = -1;
  state->inputs.clear();
  state->input_infos.clear();
  state->input_statuses.clear();
  state->input.clear();
  state->output.clear();
  clear_preview(state);
  hdrbridge::clear_input_cache();
  ui::send(state->queue, LB_RESETCONTENT, 0, 0);
  ui::set_text(state->source_path, HB_UI("Drop or add supported HDR still images"));
  set_inspector_text(state, HB_UI("SOURCE INSPECTOR\r\n\r\nNo image selected"), true);
  ui::set_text(state->output_path, HB_UI("No output yet"));
  ui::set_text(state->status, HB_UI("Ready"));
  ui::set_text(state->convert, HB_UI("Add to queue"));
  set_base_name_cue(state, HB_UI("Base name"));
  refresh_convert_button(state);
  EnableWindow(state->remove, FALSE);
  EnableWindow(state->clear, FALSE);
}

void set_running(AppState* state, bool running) {
  state->running = running;
  EnableWindow(state->open, !running);
  EnableWindow(state->format, !running);
  EnableWindow(state->gamut, !running);
  EnableWindow(state->transfer, !running);
  EnableWindow(state->representation, !running);
  EnableWindow(state->gainmap_resolution, !running);
  EnableWindow(state->gainmap_channels, !running);
  EnableWindow(state->gainmap_export_format, !running);
  EnableWindow(state->frame_export_format, !running);
  EnableWindow(state->source_folder, !running);
  EnableWindow(state->choose_folder, !running);
  EnableWindow(state->base_name, !running);
  EnableWindow(state->name_suffix, !running);
  EnableWindow(state->use_suffix, !running);
  EnableWindow(state->collision, !running);
  EnableWindow(state->reset_settings, !running);
  // Keep the queue scrollable/selectable while work runs; mutation controls
  // remain disabled, so Delete cannot alter worker indices.
  EnableWindow(state->queue, TRUE);
  EnableWindow(state->remove, !running && !state->inputs.empty());
  EnableWindow(state->clear, !running && !state->inputs.empty());
  refresh_convert_button(state);
  EnableWindow(state->cancel_button, running || !state->conversion_tasks.empty());
  EnableWindow(state->remove_all_tasks, !running && !state->conversion_tasks.empty());
  ui::set_text(state->cancel_button, running ? HB_UI("Stop") : HB_UI("Start all"));
  if (!running) update_mode_ui(state);
}

void start_task_queue(AppState* state,
                      std::optional<size_t> selected_task = std::nullopt) {
  if (state->running || state->conversion_tasks.empty()) return;
  state->cancel.store(false);
  KillTimer(state->window, kProgressResetTimer);
  ShowWindow(state->progress, SW_SHOW);
  SendMessageW(state->progress, PBM_SETPOS, 0, 0);
  set_running(state, true);
  const auto tasks = state->conversion_tasks;
  std::vector<size_t> runnable;
  if (selected_task && *selected_task < tasks.size()) {
    runnable.push_back(*selected_task);
  } else {
    for (size_t index = 0; index < tasks.size(); ++index) {
      if (tasks[index].status == QueueStatus::pending) runnable.push_back(index);
    }
  }
  if (runnable.empty()) {
    ShowWindow(state->progress, SW_HIDE);
    set_running(state, false);
    return;
  }
  const HWND window = state->window;
  std::thread([state, window, tasks, runnable] {
    try {
      std::vector<hdrbridge::ConversionResult> results;
      size_t succeeded = 0, skipped = 0, failed = 0;
      for (size_t ordinal = 0; ordinal < runnable.size(); ++ordinal) {
        const size_t index = runnable[ordinal];
        if (state->cancel.load()) throw std::runtime_error("conversion cancelled");
        PostMessageW(window, WM_APP_ITEM_STATUS, 0,
            reinterpret_cast<LPARAM>(new ItemStatusPayload{
                index, QueueStatus::running, L"", L""}));
        try {
          const auto info = hdrbridge::inspect(tasks[index].source);
          if (info.asset_kind != "direct-hdr" && info.asset_kind != "gain-map-hdr") {
            ++skipped;
            PostMessageW(window, WM_APP_ITEM_STATUS, 0,
                reinterpret_cast<LPARAM>(new ItemStatusPayload{
                    index, QueueStatus::skipped, L"", L"No HDR data"}));
            const int overall = static_cast<int>((ordinal + 1u) * 100u / runnable.size());
            PostMessageW(window, WM_APP_TASK_PROGRESS, 0,
                reinterpret_cast<LPARAM>(new TaskProgressPayload{
                    index, 100, overall,
                    L"[" + std::to_wstring(ordinal + 1u) + L"/" +
                    std::to_wstring(runnable.size()) + L"] Skipped"}));
            continue;
          }
          const auto resolved_output = task_output_path(
              tasks[index].source, tasks[index].options);
          PostMessageW(window, WM_APP_TASK_OUTPUT, 0,
              reinterpret_cast<LPARAM>(new TaskOutputPayload{
                  index, resolved_output}));
          auto result = hdrbridge::convert(
              tasks[index].source, resolved_output,
              tasks[index].options.conversion,
              [window, index, ordinal, count = runnable.size()](int value, const std::string& stage) {
                const int overall = static_cast<int>(
                    (ordinal * 100u + static_cast<size_t>(value)) / count);
                auto text = L"[" + std::to_wstring(ordinal + 1u) + L"/" +
                    std::to_wstring(count) + L"] " + widen(stage);
                PostMessageW(window, WM_APP_TASK_PROGRESS, 0,
                    reinterpret_cast<LPARAM>(new TaskProgressPayload{
                        index, value, overall, std::move(text)}));
              }, &state->cancel);
          PostMessageW(window, WM_APP_TASK_OUTPUT, 0,
              reinterpret_cast<LPARAM>(new TaskOutputPayload{
                  index, result.output_path}));
          PostMessageW(window, WM_APP_RESULT, 0,
              reinterpret_cast<LPARAM>(new hdrbridge::ConversionResult(result)));
          results.push_back(std::move(result));
          ++succeeded;
          PostMessageW(window, WM_APP_ITEM_STATUS, 0,
              reinterpret_cast<LPARAM>(new ItemStatusPayload{
                  index, QueueStatus::success, L"", L""}));
        } catch (const std::exception& error) {
          if (state->cancel.load()) throw;
          ++failed;
          PostMessageW(window, WM_APP_ITEM_STATUS, 0,
              reinterpret_cast<LPARAM>(new ItemStatusPayload{
                  index, QueueStatus::failed, L"", widen(error.what())}));
        }
        const int overall = static_cast<int>((ordinal + 1u) * 100u / runnable.size());
        PostMessageW(window, WM_APP_TASK_PROGRESS, 0,
            reinterpret_cast<LPARAM>(new TaskProgressPayload{
                index, 100, overall,
                L"[" + std::to_wstring(ordinal + 1u) + L"/" +
                std::to_wstring(runnable.size()) + L"] Complete"}));
      }
      PostMessageW(window, WM_APP_SUCCESS, 0,
          reinterpret_cast<LPARAM>(new SuccessPayload{
              std::move(results), succeeded, skipped, failed}));
    } catch (const std::exception& error) {
      PostMessageW(window, WM_APP_ERROR, 0,
          reinterpret_cast<LPARAM>(new std::wstring(widen(error.what()))));
    }
  }).detach();
}

HWND add_control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
  HWND control = CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE | style,
                                 0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
  ui::set_text(control, text);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
  return control;
}

void schedule_update_check_timer(AppState* state) {
  if (!state || !state->window) return;
  KillTimer(state->window, kUpdateCheckTimer);
  const auto now_ms = now_epoch_ms();
  const auto checked_ok = read_layout_qword(L"UpdateCheckedOkMs");
  const auto attempted = read_layout_qword(L"UpdateAttemptedMs");
  const auto delay_ms = std::max<std::int64_t>(
      hdrbridge::app_update::kManualCheckIntervalMs,
      hdrbridge::app_update::delay_until_automatic_check_ms(now_ms, checked_ok, attempted));
  const UINT clamped_ms = static_cast<UINT>(
      std::min<std::int64_t>(delay_ms, 24LL * 60LL * 60LL * 1000LL));
  SetTimer(state->window, kUpdateCheckTimer, clamped_ms, nullptr);
}

void set_update_available_indicator(AppState* state, bool available) {
  const auto label = available
      ? std::wstring(HDRBRIDGE_VERSION_WIDE) + HB_UI(" • update available")
      : std::wstring(HDRBRIDGE_VERSION_WIDE);
  ui::set_text(state->version, label.c_str());
  RECT client{};
  GetClientRect(state->window, &client);
  layout(state, client.right, client.bottom);
}

void start_update_check(AppState* state, bool manual) {
  if (state->update_busy || state->update_prompt_open) {
    if (manual) {
      state->update_manual_requested = true;
      append_log(state, HB_UI("An update check or download is already in progress."));
    } else {
      schedule_update_check_timer(state);
    }
    return;
  }
  if (manual && state->ready_installer) {
    std::error_code ec;
    const bool matches_pending = !state->pending_release ||
        hdrbridge::app_update::installer_matches_release(
            *state->ready_installer, *state->pending_release);
    if (!matches_pending || !std::filesystem::exists(state->ready_installer->path, ec)) {
      state->ready_installer.reset();
    } else {
      if (state->running) {
        ui::message_box(state->window, HB_UI("Finish the current conversion before installing."), HB_UI("Update ready"), MB_OK | MB_ICONINFORMATION);
      } else {
        const auto installer = *state->ready_installer;
        state->update_prompt_open = true;
        const int choice = ui::message_box(
            state->window, HB_UI("The installer was verified. Launch it now?"),
            HB_UI("Update ready"), MB_YESNO | MB_ICONQUESTION);
        state->update_prompt_open = false;
        if (choice == IDYES) {
          std::string error;
          if (!hdrbridge::app_update::launch_installer(installer, &error)) {
            state->ready_installer.reset();
            ui::message_box(state->window, widen(error).c_str(), HB_UI("Update failed"), MB_OK | MB_ICONERROR);
          } else {
            PostMessageW(state->window, WM_CLOSE, 0, 0);
          }
        }
      }
      return;
    }
  }
  const auto now_ms = now_epoch_ms();
  const auto checked_ok = read_layout_qword(L"UpdateCheckedOkMs");
  const auto attempted = read_layout_qword(L"UpdateAttemptedMs");
  if (manual && state->pending_release &&
      !hdrbridge::app_update::should_check_updates(now_ms, checked_ok, attempted, true)) {
    auto result = std::make_unique<UpdateCheckPayload>();
    result->manual = true;
    result->release = state->pending_release;
    if (PostMessageW(state->window, WM_APP_UPDATE_CHECK, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    return;
  }
  if (!hdrbridge::app_update::should_check_updates(now_ms, checked_ok, attempted, manual)) {
    if (manual) {
      const auto seconds = std::max<std::int64_t>(
          1, (attempted + hdrbridge::app_update::kManualCheckIntervalMs - now_ms + 999) / 1000);
      const auto wait_text = HB_UI("Please wait ") + std::to_wstring(seconds) +
          HB_UI(" seconds before checking again.");
      append_log(state, wait_text);
      ui::message_box(state->window, wait_text.c_str(), HB_UI("Update check"), MB_OK | MB_ICONINFORMATION);
    }
    schedule_update_check_timer(state);
    return;
  }
  write_layout_qword(L"UpdateAttemptedMs", now_ms);
  state->update_busy = true;
  const HWND window = state->window;
  std::thread([window, manual] {
    auto result = std::make_unique<UpdateCheckPayload>();
    result->manual = manual;
    result->from_network = true;
    result->release = hdrbridge::app_update::check_latest(&result->error);
    if (PostMessageW(window, WM_APP_UPDATE_CHECK, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
  }).detach();
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_CREATE) {
    LoadLibraryW(L"msftedit.dll");
    auto owned = std::make_unique<AppState>();
    ui::load_language();
    owned->window = window;
    owned->title = add_control(window, L"STATIC", L"HDR Bridge", SS_LEFT | SS_NOTIFY, IDC_TITLE);
    owned->version = add_control(window, L"STATIC", HDRBRIDGE_VERSION_WIDE, SS_NOTIFY, IDC_VERSION);
    HWND version_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        window, nullptr, GetModuleHandleW(nullptr), nullptr);
    TOOLINFOW tip{sizeof(tip)};
    tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tip.hwnd = window;
    tip.uId = reinterpret_cast<UINT_PTR>(owned->version);
    tip.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(version_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    tip.uId = reinterpret_cast<UINT_PTR>(owned->title);
    SendMessageW(version_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    owned->reset_layout = add_control(window, L"BUTTON", HB_UI("Reset layout"), BS_PUSHBUTTON, IDC_RESET_LAYOUT);
    owned->reset_settings = add_control(window, L"BUTTON", HB_UI("Reset settings"), BS_PUSHBUTTON, IDC_RESET_SETTINGS);
    owned->preview_toggle = add_control(window, L"BUTTON", HB_UI("Preview"), BS_PUSHBUTTON,
                                        IDC_PREVIEW_TOGGLE);
    owned->preview_renderer = std::make_unique<PreviewRenderer>(window);
    owned->preview_renderer->create();
    owned->vertical_splitter = add_control(window, L"STATIC", L"", SS_NOTIFY, IDC_VERTICAL_SPLITTER);
    owned->horizontal_splitter = add_control(window, L"STATIC", L"", SS_NOTIFY, IDC_HORIZONTAL_SPLITTER);
    owned->files_splitter = add_control(window, L"STATIC", L"", SS_NOTIFY, IDC_FILES_SPLITTER);
    owned->queue_label = add_control(window, L"STATIC", HB_UI("FILES"), SS_LEFT, IDC_QUEUE_LABEL);
    owned->output_label = add_control(window, L"STATIC", HB_UI("OUTPUT"), SS_LEFT, IDC_OUTPUT_LABEL);
    owned->inspector_label = add_control(window, L"STATIC", HB_UI("SOURCE INSPECTOR"), SS_LEFT, IDC_INSPECTOR_LABEL);
    owned->activity_label = add_control(window, L"STATIC", HB_UI("ACTIVITY"), SS_LEFT, IDC_ACTIVITY_LABEL);
    owned->tasks_label = add_control(window, L"STATIC", HB_UI("TASKS"), SS_LEFT, IDC_TASKS_LABEL);
    owned->open = add_control(window, L"BUTTON", HB_UI("Add files"), BS_PUSHBUTTON, IDC_OPEN);
    owned->source_path = add_control(window, L"STATIC", HB_UI("Drop or add supported HDR still images"), SS_LEFT | SS_PATHELLIPSIS, IDC_SOURCE_PATH);
    owned->queue = add_control(window, L"LISTBOX", L"", LBS_NOTIFY | LBS_EXTENDEDSEL |
                               LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_BORDER |
                               WS_VSCROLL | LBS_NOINTEGRALHEIGHT, IDC_QUEUE);
    SendMessageW(owned->queue, LB_SETITEMHEIGHT, 0,
                 MulDiv(26, GetDpiForWindow(window), 96));
    owned->remove = add_control(window, L"BUTTON", HB_UI("Remove"), BS_PUSHBUTTON, IDC_REMOVE);
    owned->clear = add_control(window, L"BUTTON", HB_UI("Clear"), BS_PUSHBUTTON, IDC_CLEAR);
    const auto configure_rich = [](HWND rich, bool editable) {
      if (!rich) return;
      const LRESULT lang_opts = SendMessageW(rich, EM_GETLANGOPTIONS, 0, 0);
      SendMessageW(rich, EM_SETLANGOPTIONS, 0,
                   lang_opts | IMF_UIFONTS | IMF_DUALFONT | IMF_AUTOFONT);
      SendMessageW(rich, EM_SETEDITSTYLE, SES_NOEALINEHEIGHTADJUST,
                   SES_NOEALINEHEIGHTADJUST);
      SendMessageW(rich, EM_EXLIMITTEXT, 0, 16 * 1024 * 1024);
      if (editable) SendMessageW(rich, EM_SETEVENTMASK, 0, ENM_CHANGE);
    };
    owned->inspector = add_control(window, MSFTEDIT_CLASS, L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_NOHIDESEL, IDC_INSPECTOR);
    ui::captions.erase(owned->inspector);
    configure_rich(owned->inspector, false);
    constexpr DWORD kComboStyle = CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL;
    owned->format = add_control(window, WC_COMBOBOXW, L"", kComboStyle, IDC_FORMAT);
    for (const wchar_t* item : {L"Ultra HDR JPEG",
                                L"HDR PNG",
                                L"HDR TIFF",
                                L"JPEG XL",
                                L"JPEG XR FP16",
                                L"HDR AVIF",
                                HB_UI("Extract Gain Map"),
                                HB_UI("Photo Frame")}) {
      ui::send(owned->format, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(owned->format, CB_SETCURSEL, 0, 0);
    owned->gamut = add_control(window, WC_COMBOBOXW, L"", kComboStyle, IDC_GAMUT);
    owned->transfer = add_control(window, WC_COMBOBOXW, L"", kComboStyle, IDC_TRANSFER);
    owned->representation = add_control(window, WC_COMBOBOXW, L"", kComboStyle, IDC_REPRESENTATION);
    ui::send(owned->representation, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"JPEG — HDR / SDR"));
    ui::send(owned->representation, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PNG — HDR / SDR"));
    SendMessageW(owned->representation, CB_SETCURSEL, 0, 0);
    owned->gainmap_resolution = add_control(window, WC_COMBOBOXW, L"", kComboStyle, IDC_GAINMAP_RESOLUTION);
    ui::send(owned->gainmap_resolution, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(HB_UI("Gain map: 1/4")));
    ui::send(owned->gainmap_resolution, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(HB_UI("Gain map: 1/2")));
    ui::send(owned->gainmap_resolution, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(HB_UI("Gain map: Full resolution — default")));
    SendMessageW(owned->gainmap_resolution, CB_SETCURSEL, 2, 0);
    owned->gainmap_channels = add_control(window, WC_COMBOBOXW, L"", kComboStyle, IDC_GAINMAP_CHANNELS);
    ui::send(owned->gainmap_channels, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(HB_UI("Channels: Mono")));
    ui::send(owned->gainmap_channels, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(HB_UI("Channels: RGB — default")));
    SendMessageW(owned->gainmap_channels, CB_SETCURSEL, 1, 0);
    owned->tiff_compression = add_control(window, WC_COMBOBOXW, L"",
                                          kComboStyle,
                                          IDC_TIFF_COMPRESSION);
    ui::send(owned->tiff_compression, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(HB_UI("Compression: Deflate — default")));
    ui::send(owned->tiff_compression, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(HB_UI("Compression: Uncompressed")));
    SendMessageW(owned->tiff_compression, CB_SETCURSEL, 0, 0);
    ShowWindow(owned->tiff_compression, SW_HIDE);
    owned->gainmap_export_format = add_control(
        window, WC_COMBOBOXW, L"", kComboStyle,
        IDC_GAINMAP_EXPORT_FORMAT);
    for (const wchar_t* item : {HB_UI("Format: Original — default"),
                                HB_UI("Format: PNG"),
                                HB_UI("Format: TIFF"),
                                HB_UI("Format: JPEG")}) {
      ui::send(owned->gainmap_export_format, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(owned->gainmap_export_format, CB_SETCURSEL, 0, 0);
    ShowWindow(owned->gainmap_export_format, SW_HIDE);
    owned->frame_export_format = add_control(
        window, WC_COMBOBOXW, L"", kComboStyle,
        IDC_FRAME_EXPORT_FORMAT);
    for (const wchar_t* item : {HB_UI("Format: Ultra HDR JPEG"),
                                HB_UI("Format: PNG")}) {
      ui::send(owned->frame_export_format, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(owned->frame_export_format, CB_SETCURSEL, 0, 0);
    ShowWindow(owned->frame_export_format, SW_HIDE);
    owned->lossless = add_control(window, L"BUTTON", HB_UI("Lossless"), BS_AUTOCHECKBOX, IDC_LOSSLESS);
    owned->copy_exif = add_control(window, L"BUTTON", L"EXIF", BS_AUTOCHECKBOX, IDC_COPY_EXIF);
    owned->copy_xmp = add_control(window, L"BUTTON", L"XMP", BS_AUTOCHECKBOX, IDC_COPY_XMP);
    SendMessageW(owned->lossless, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(owned->copy_exif, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(owned->copy_xmp, BM_SETCHECK, BST_CHECKED, 0);
    owned->quality = add_control(window, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS, IDC_QUALITY);
    SendMessageW(owned->quality, TBM_SETRANGE, TRUE, MAKELPARAM(1, 100));
    SendMessageW(owned->quality, TBM_SETPOS, TRUE, 95);
    owned->quality_value = add_control(window, L"STATIC", L"95%", SS_CENTER, IDC_QUALITY_VALUE);
    owned->source_folder = add_control(window, L"BUTTON", HB_UI("Source folder"), BS_AUTOCHECKBOX, IDC_SOURCE_FOLDER);
    SendMessageW(owned->source_folder, BM_SETCHECK, BST_CHECKED, 0);
    owned->choose_folder = add_control(window, L"BUTTON", HB_UI("Choose..."), BS_PUSHBUTTON, IDC_CHOOSE_FOLDER);
    owned->folder_path = add_control(window, L"STATIC", HB_UI("Source folder"), SS_LEFT | SS_PATHELLIPSIS, IDC_FOLDER_PATH);
    owned->base_name = add_control(window, MSFTEDIT_CLASS, L"", ES_AUTOHSCROLL, IDC_BASE_NAME);
    ui::captions.erase(owned->base_name);
    configure_rich(owned->base_name, true);
    owned->name_suffix = add_control(window, MSFTEDIT_CLASS, L"_ultrahdr", ES_AUTOHSCROLL, IDC_NAME_SUFFIX);
    ui::captions.erase(owned->name_suffix);
    configure_rich(owned->name_suffix, true);
    owned->collision = add_control(window, WC_COMBOBOXW, L"", kComboStyle, IDC_COLLISION);
    ui::send(owned->collision, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(HB_UI("If file exists: auto-number")));
    ui::send(owned->collision, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(HB_UI("If file exists: overwrite")));
    SendMessageW(owned->collision, CB_SETCURSEL, 0, 0);
    owned->use_suffix = add_control(window, L"BUTTON", HB_UI("Use suffix"), BS_AUTOCHECKBOX, IDC_USE_SUFFIX);
    SendMessageW(owned->use_suffix, BM_SETCHECK, BST_CHECKED, 0);
    owned->convert = add_control(window, L"BUTTON", HB_UI("Add to queue"), BS_DEFPUSHBUTTON, IDC_CONVERT);
    owned->remove_all_tasks = add_control(window, L"BUTTON", HB_UI("Remove all"), BS_PUSHBUTTON,
                                          IDC_REMOVE_ALL_TASKS);
    owned->cancel_button = add_control(window, L"BUTTON", HB_UI("Start all"), BS_PUSHBUTTON, IDC_CANCEL);
    owned->tasks = add_control(window, L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                               LBS_HASSTRINGS | WS_BORDER | WS_VSCROLL |
                               LBS_NOINTEGRALHEIGHT, IDC_TASKS);
    SendMessageW(owned->tasks, LB_SETITEMHEIGHT, 0,
                 MulDiv(52, GetDpiForWindow(window), 96));
    owned->progress = add_control(window, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROGRESS);
    SendMessageW(owned->progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    owned->status = add_control(window, L"STATIC", HB_UI("Ready"), SS_LEFT, IDC_STATUS);
    owned->output_path = add_control(window, L"STATIC", HB_UI("No output yet"), SS_LEFT | SS_PATHELLIPSIS, IDC_OUTPUT_PATH);
    owned->log = add_control(window, MSFTEDIT_CLASS, L"",
                             ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_NOHIDESEL, IDC_LOG);
    ui::captions.erase(owned->log);
    configure_rich(owned->log, false);
    EnableWindow(owned->convert, FALSE);
    EnableWindow(owned->remove, FALSE);
    EnableWindow(owned->clear, FALSE);
    EnableWindow(owned->cancel_button, FALSE);
    EnableWindow(owned->remove_all_tasks, FALSE);
    ShowWindow(owned->progress, SW_HIDE);
    SetWindowSubclass(owned->queue, queue_subclass, 1, 0);
    SetWindowSubclass(owned->tasks, queue_subclass, 2, 0);
    SetWindowSubclass(owned->vertical_splitter, splitter_subclass, 1, 1);
    SetWindowSubclass(owned->horizontal_splitter, splitter_subclass, 1, 2);
    SetWindowSubclass(owned->files_splitter, splitter_subclass, 1, 3);
    const auto static_data = reinterpret_cast<DWORD_PTR>(owned.get());
    for (HWND label : {owned->title, owned->version, owned->queue_label,
                       owned->output_label, owned->inspector_label,
                       owned->activity_label, owned->tasks_label,
                       owned->source_path, owned->quality_value,
                       owned->folder_path, owned->status, owned->output_path}) {
      SetWindowSubclass(label, mixed_static_subclass, 1, static_data);
    }
    for (HWND combo : {owned->format, owned->gamut, owned->transfer,
                       owned->representation, owned->gainmap_resolution,
                       owned->gainmap_channels,
                       owned->tiff_compression, owned->gainmap_export_format,
                       owned->frame_export_format,
                       owned->collision}) {
      SetWindowSubclass(combo, combo_mixed_subclass, 1, static_data);
    }
    for (HWND rich : {owned->inspector, owned->log, owned->base_name, owned->name_suffix}) {
      SetWindowSubclass(rich, rich_edit_subclass, 1, static_data);
      SetWindowPos(rich, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
      update_rich_insets(rich);
    }
    DragAcceptFiles(window, TRUE);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned.release()));
    state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    load_split_layout(state);
    load_conversion_settings(state);
    apply_fonts(state);
    apply_theme(state);
    refresh_inspector(state);
    refresh_activity(state);
    update_mode_ui(state);
    if (const auto cached = hdrbridge::app_update::parse_cached_release(
            read_layout_utf8(L"UpdateCachedRelease"))) {
      const auto comparison = hdrbridge::app_update::compare_versions(
          cached->version, HDRBRIDGE_VERSION_STRING);
      if (comparison && *comparison > 0) {
        state->pending_release = *cached;
        set_update_available_indicator(state, true);
        if (!hdrbridge::app_update::should_check_updates(
                now_epoch_ms(), read_layout_qword(L"UpdateCheckedOkMs"),
                read_layout_qword(L"UpdateAttemptedMs"), false)) {
          auto cached_offer = std::make_unique<UpdateCheckPayload>();
          cached_offer->release = state->pending_release;
          if (PostMessageW(window, WM_APP_UPDATE_CHECK, 0,
                           reinterpret_cast<LPARAM>(cached_offer.get()))) {
            cached_offer.release();
          }
        }
      } else {
        write_layout_utf8(L"UpdateCachedRelease", "");
      }
    }
    start_update_check(state, false);
    return 0;
  }
  if (!state) return DefWindowProcW(window, message, wparam, lparam);
  switch (message) {
    case WM_SETCURSOR:
      if (reinterpret_cast<HWND>(wparam) == state->version || reinterpret_cast<HWND>(wparam) == state->title) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_NOTIFY: {
      const auto* nmhdr = reinterpret_cast<NMHDR*>(lparam);
      if (nmhdr->code == TTN_GETDISPINFOW) {
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(lparam);
        static std::wstring tooltip;
        tooltip = ui::render(info->hdr.idFrom == reinterpret_cast<UINT_PTR>(state->title)
            ? HB_UI("Switch to Simplified Chinese") : HB_UI("Check for updates"));
        info->lpszText = tooltip.data();
        return 0;
      }
      if (nmhdr->code == NM_CUSTOMDRAW && state->font) {
        const auto* nm = reinterpret_cast<const NMCUSTOMDRAW*>(lparam);
        if (nm->dwDrawStage == CDDS_PREPAINT) {
          wchar_t cls[32]{};
          GetClassNameW(nm->hdr.hwndFrom, cls, 32);
          if (_wcsicmp(cls, L"Button") == 0) {
            const LONG_PTR style = GetWindowLongPtrW(nm->hdr.hwndFrom, GWL_STYLE);
            const LONG_PTR type = style & BS_TYPEMASK;
            const UINT dpi = GetDpiForWindow(window);
            if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
              HTHEME theme = OpenThemeData(nm->hdr.hwndFrom, L"BUTTON");
              if (!theme) return CDRF_DODEFAULT;
              const bool disabled = (nm->uItemState & CDIS_DISABLED) != 0;
              const bool pressed = (nm->uItemState & CDIS_SELECTED) != 0;
              const bool hot = (nm->uItemState & CDIS_HOT) != 0;
              const int part_state = disabled ? PBS_DISABLED :
                  pressed ? PBS_PRESSED :
                  hot ? PBS_HOT :
                  (type == BS_DEFPUSHBUTTON ? PBS_DEFAULTED : PBS_NORMAL);
              DrawThemeParentBackground(nm->hdr.hwndFrom, nm->hdc, &nm->rc);
              DrawThemeBackground(theme, nm->hdc, BP_PUSHBUTTON, part_state, &nm->rc, nullptr);
              RECT text_rc = nm->rc;
              SetTextColor(nm->hdc, disabled ? state->muted_text_color : state->text_color);
              draw_mixed_text(nm->hdc, state->font, state->cjk_font,
                              control_text(nm->hdr.hwndFrom), text_rc,
                              DT_CENTER | DT_VCENTER);
              if ((nm->uItemState & CDIS_FOCUS) != 0 &&
                  (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
                RECT focus_rc = nm->rc;
                InflateRect(&focus_rc, -MulDiv(3, dpi, 96), -MulDiv(3, dpi, 96));
                DrawFocusRect(nm->hdc, &focus_rc);
              }
              CloseThemeData(theme);
              return CDRF_SKIPDEFAULT;
            }
            if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX) {
              HTHEME theme = OpenThemeData(nm->hdr.hwndFrom, L"BUTTON");
              if (!theme) return CDRF_DODEFAULT;
              const bool checked = SendMessageW(nm->hdr.hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED;
              const bool disabled = (nm->uItemState & CDIS_DISABLED) != 0;
              const bool pressed = (nm->uItemState & CDIS_SELECTED) != 0;
              const bool hot = (nm->uItemState & CDIS_HOT) != 0;
              const int part_state = checked
                  ? (disabled ? CBS_CHECKEDDISABLED : pressed ? CBS_CHECKEDPRESSED :
                     hot ? CBS_CHECKEDHOT : CBS_CHECKEDNORMAL)
                  : (disabled ? CBS_UNCHECKEDDISABLED : pressed ? CBS_UNCHECKEDPRESSED :
                     hot ? CBS_UNCHECKEDHOT : CBS_UNCHECKEDNORMAL);
              FillRect(nm->hdc, &nm->rc, state->background_brush);
              SIZE box_sz{MulDiv(13, dpi, 96), MulDiv(13, dpi, 96)};
              GetThemePartSize(theme, nm->hdc, BP_CHECKBOX, part_state, nullptr, TS_TRUE, &box_sz);
              RECT box_rc{nm->rc.left,
                          nm->rc.top + ((nm->rc.bottom - nm->rc.top) - box_sz.cy) / 2,
                          nm->rc.left + box_sz.cx, 0};
              box_rc.bottom = box_rc.top + box_sz.cy;
              DrawThemeBackground(theme, nm->hdc, BP_CHECKBOX, part_state, &box_rc, nullptr);
              RECT text_rc = nm->rc;
              text_rc.left += box_sz.cx + MulDiv(6, dpi, 96);
              SetTextColor(nm->hdc, disabled ? state->muted_text_color : state->text_color);
              draw_mixed_text(nm->hdc, state->font, state->cjk_font,
                              control_text(nm->hdr.hwndFrom), text_rc,
                              DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
              if ((nm->uItemState & CDIS_FOCUS) != 0 &&
                  (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
                DrawFocusRect(nm->hdc, &text_rc);
              }
              CloseThemeData(theme);
              return CDRF_SKIPDEFAULT;
            }
          }
        }
      }
      break;
    }
    case WM_APP_UPDATE_CHECK: {
      std::unique_ptr<UpdateCheckPayload> result(reinterpret_cast<UpdateCheckPayload*>(lparam));
      state->update_busy = false;
      result->manual = result->manual || state->update_manual_requested;
      state->update_manual_requested = false;
      if (!result->release) {
        schedule_update_check_timer(state);
        if (result->manual) ui::message_box(window, widen(result->error).c_str(), HB_UI("Update check"), MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      const auto comparison = hdrbridge::app_update::compare_versions(result->release->version, HDRBRIDGE_VERSION_STRING);
      if (!comparison) {
        schedule_update_check_timer(state);
        if (result->manual) ui::message_box(window, HB_UI("The release version could not be compared."), HB_UI("Update check"), MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      if (result->from_network) {
        write_layout_qword(L"UpdateCheckedOkMs", now_epoch_ms());
      }
      if (*comparison <= 0) {
        state->pending_release.reset();
        state->ready_installer.reset();
        state->offered_update_version.clear();
        write_layout_utf8(L"UpdateCachedRelease", "");
        set_update_available_indicator(state, false);
        schedule_update_check_timer(state);
        if (result->manual) ui::message_box(window, HB_UI("You have the latest version."), HB_UI("Update check"), MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      if (state->ready_installer &&
          !hdrbridge::app_update::installer_matches_release(
              *state->ready_installer, *result->release)) {
        state->ready_installer.reset();
        state->offered_update_version.clear();
      }
      state->pending_release = *result->release;
      if (result->from_network) {
        write_layout_utf8(L"UpdateCachedRelease",
                          hdrbridge::app_update::serialize_release(*result->release));
      }
      set_update_available_indicator(state, true);
      schedule_update_check_timer(state);
      if (!result->manual) {
        if (state->offered_update_version == result->release->version) return 0;
        state->offered_update_version = result->release->version;
        if (state->running) {
          append_log(state, HB_UI("Update available: ") + widen(result->release->version) + HB_UI(". Click the version to download."));
          return 0;
        }
      } else {
        state->offered_update_version = result->release->version;
      }
      if (state->update_prompt_open) return 0;
      const auto prompt = HB_UI("Version ") + widen(result->release->version) + HB_UI(" is available. Download the installer?");
      state->update_prompt_open = true;
      const int download_choice = ui::message_box(window, prompt.c_str(), HB_UI("Update available"), MB_YESNO | MB_ICONQUESTION);
      state->update_prompt_open = false;
      if (download_choice != IDYES) return 0;
      state->ready_installer.reset();
      state->update_busy = true;
      auto release = std::move(*result->release);
      std::thread([window, release = std::move(release)] {
        auto payload = std::make_unique<UpdateDownloadPayload>();
        payload->path = hdrbridge::app_update::download_installer(release, {}, &payload->error);
        if (PostMessageW(window, WM_APP_UPDATE_DOWNLOAD, 0, reinterpret_cast<LPARAM>(payload.get()))) payload.release();
      }).detach();
      return 0;
    }
    case WM_APP_UPDATE_DOWNLOAD: {
      std::unique_ptr<UpdateDownloadPayload> result(reinterpret_cast<UpdateDownloadPayload*>(lparam));
      state->update_busy = false;
      state->update_manual_requested = false;
      if (!result->path) {
        write_layout_qword(L"UpdateCheckedOkMs", 0);
        write_layout_qword(L"UpdateAttemptedMs", now_epoch_ms());
        schedule_update_check_timer(state);
        ui::message_box(window, widen(result->error).c_str(), HB_UI("Update download failed"), MB_OK | MB_ICONERROR);
        return 0;
      }
      schedule_update_check_timer(state);
      const auto installer = *result->path;
      state->ready_installer = installer;
      if (state->running) {
        append_log(state, HB_UI("Update installer ready. Click the version after conversion to install."));
        return 0;
      }
      state->update_prompt_open = true;
      const int launch_choice = ui::message_box(window, HB_UI("The installer was verified. Launch it now?"), HB_UI("Update ready"), MB_YESNO | MB_ICONQUESTION);
      state->update_prompt_open = false;
      if (launch_choice == IDYES) {
        std::string error;
        if (!hdrbridge::app_update::launch_installer(installer, &error)) {
          state->ready_installer.reset();
          ui::message_box(window, widen(error).c_str(), HB_UI("Update failed"), MB_OK | MB_ICONERROR);
        } else {
          PostMessageW(window, WM_CLOSE, 0, 0);
        }
      }
      return 0;
    }
    case WM_APP_TASK_REORDER: {
      if (state->running) return 0;
      const size_t from = static_cast<size_t>(wparam);
      const size_t to = static_cast<size_t>(lparam);
      if (from < state->conversion_tasks.size() && to < state->conversion_tasks.size() && from != to) {
        auto task = std::move(state->conversion_tasks[from]);
        state->conversion_tasks.erase(state->conversion_tasks.begin() + from);
        state->conversion_tasks.insert(state->conversion_tasks.begin() + to, std::move(task));
        if (state->editing_task >= 0) {
          const auto editing = static_cast<size_t>(state->editing_task);
          if (editing == from) state->editing_task = static_cast<int>(to);
          else if (from < editing && editing <= to) --state->editing_task;
          else if (to <= editing && editing < from) ++state->editing_task;
        }
        rebuild_task_list(state);
        SendMessageW(state->tasks, LB_SETCURSEL, to, 0);
      }
      return 0;
    }
    case WM_CONTEXTMENU: {
      const HWND target = reinterpret_cast<HWND>(wparam);
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x == -1 && point.y == -1) GetCursorPos(&point);
      if (target == state->queue) {
        POINT client_point = point;
        ScreenToClient(state->queue, &client_point);
        const int hit = static_cast<int>(SendMessageW(
            state->queue, LB_ITEMFROMPOINT, 0,
            MAKELPARAM(client_point.x, client_point.y)));
        if (HIWORD(hit) == 0) {
          const int clicked = LOWORD(hit);
          if (SendMessageW(state->queue, LB_GETSEL, clicked, 0) <= 0) {
            SendMessageW(state->queue, LB_SETSEL, FALSE, -1);
            SendMessageW(state->queue, LB_SETSEL, TRUE, clicked);
          }
          SendMessageW(state->queue, LB_SETCARETINDEX, clicked, FALSE);
        }
        const int selected = static_cast<int>(SendMessageW(state->queue, LB_GETCARETINDEX, 0, 0));
        if (selected == LB_ERR || static_cast<size_t>(selected) >= state->inputs.size()) return 0;
        HMENU menu = CreatePopupMenu();
        ui::append_menu(menu, MF_STRING, ID_CONTEXT_OPEN, HB_UI("Open file"));
        ui::append_menu(menu, MF_STRING, ID_CONTEXT_FOLDER, HB_UI("Open containing folder"));
        ui::append_menu(menu, MF_SEPARATOR, 0, nullptr);
        ui::append_menu(menu, state->running ? MF_STRING | MF_GRAYED : MF_STRING,
                    ID_CONTEXT_REMOVE, HB_UI("Remove"));
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (command == ID_CONTEXT_REMOVE) {
          remove_selected_source(state);
          return 0;
        }
        const auto source = state->inputs[static_cast<size_t>(selected)];
        if (command == ID_CONTEXT_OPEN) ShellExecuteW(window, L"open", source.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (command == ID_CONTEXT_FOLDER) open_folder_for(window, source, true);
        return 0;
      }
      if (target == state->tasks) {
        POINT client_point = point;
        ScreenToClient(state->tasks, &client_point);
        const DWORD hit = static_cast<DWORD>(SendMessageW(
            state->tasks, LB_ITEMFROMPOINT, 0, MAKELPARAM(client_point.x, client_point.y)));
        const int hit_index = static_cast<int>(LOWORD(hit));
        const int task_count = static_cast<int>(SendMessageW(
            state->tasks, LB_GETCOUNT, 0, 0));
        RECT hit_rect{};
        const bool valid_item = HIWORD(hit) == 0 && hit_index >= 0 &&
            hit_index < task_count &&
            SendMessageW(state->tasks, LB_GETITEMRECT, hit_index,
                         reinterpret_cast<LPARAM>(&hit_rect)) != LB_ERR &&
            PtInRect(&hit_rect, client_point);
        const bool blank = !valid_item;
        if (!blank) SendMessageW(state->tasks, LB_SETCURSEL, hit_index, 0);
        if (blank) {
          const bool has_completed = std::any_of(
              state->conversion_tasks.begin(), state->conversion_tasks.end(),
              [](const ConversionTask& task) {
                return task.status == QueueStatus::success;
              });
          HMENU menu = CreatePopupMenu();
          ui::append_menu(menu, MF_STRING | (state->running || !has_completed ? MF_GRAYED : 0),
                      ID_TASK_REMOVE_COMPLETED, HB_UI("Remove completed"));
          ui::append_menu(menu, MF_STRING | (state->running || state->conversion_tasks.empty() ? MF_GRAYED : 0),
                      ID_TASK_REMOVE_ALL, HB_UI("Remove all"));
          const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                              point.x, point.y, 0, window, nullptr);
          DestroyMenu(menu);
          if (command == ID_TASK_REMOVE_COMPLETED) {
            if (state->editing_task >= 0 &&
                static_cast<size_t>(state->editing_task) < state->conversion_tasks.size()) {
              const auto editing = static_cast<size_t>(state->editing_task);
              if (state->conversion_tasks[editing].status == QueueStatus::success) {
                cancel_task_edit(state);
              } else {
                const auto removed_before = static_cast<int>(std::count_if(
                    state->conversion_tasks.begin(),
                    state->conversion_tasks.begin() + static_cast<std::ptrdiff_t>(editing),
                    [](const ConversionTask& task) { return task.status == QueueStatus::success; }));
                state->editing_task -= removed_before;
              }
            }
            std::erase_if(state->conversion_tasks, [](const ConversionTask& task) {
              return task.status == QueueStatus::success;
            });
            rebuild_task_list(state);
            EnableWindow(state->cancel_button, !state->conversion_tasks.empty());
            EnableWindow(state->remove_all_tasks, !state->conversion_tasks.empty());
          } else if (command == ID_TASK_REMOVE_ALL) {
            SendMessageW(window, WM_COMMAND,
                         MAKEWPARAM(IDC_REMOVE_ALL_TASKS, BN_CLICKED),
                         reinterpret_cast<LPARAM>(state->remove_all_tasks));
          }
          return 0;
        }
        const int selected = static_cast<int>(SendMessageW(state->tasks, LB_GETCURSEL, 0, 0));
        if (selected == LB_ERR || static_cast<size_t>(selected) >= state->conversion_tasks.size()) return 0;
        const auto& task = state->conversion_tasks[static_cast<size_t>(selected)];
        const bool output_ready = task.status == QueueStatus::success && std::filesystem::exists(task.output);
        HMENU menu = CreatePopupMenu();
        ui::append_menu(menu, MF_STRING | (state->running ? MF_GRAYED : 0),
                    ID_TASK_START, HB_UI("Start task"));
        ui::append_menu(menu, MF_STRING, ID_TASK_CLEAR_STATUS, HB_UI("Clear task status"));
        ui::append_menu(menu, MF_STRING | (state->running ? MF_GRAYED : 0), ID_TASK_DELETE, HB_UI("Delete task"));
        ui::append_menu(menu, MF_SEPARATOR, 0, nullptr);
        ui::append_menu(menu, MF_STRING, ID_TASK_OPEN_SOURCE, HB_UI("Open source file"));
        ui::append_menu(menu, MF_STRING, ID_TASK_SOURCE_FOLDER, HB_UI("Open source folder"));
        ui::append_menu(menu, MF_STRING | (output_ready ? 0 : MF_GRAYED), ID_TASK_OPEN_OUTPUT, HB_UI("Open converted file"));
        ui::append_menu(menu, MF_STRING | (output_ready ? 0 : MF_GRAYED), ID_TASK_OUTPUT_FOLDER, HB_UI("Open converted folder"));
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (command == ID_TASK_START) {
          start_task_queue(state, static_cast<size_t>(selected));
        } else if (command == ID_TASK_CLEAR_STATUS) {
          state->conversion_tasks[static_cast<size_t>(selected)].status = QueueStatus::pending;
          state->conversion_tasks[static_cast<size_t>(selected)].progress = 0;
          state->conversion_tasks[static_cast<size_t>(selected)].reason.clear();
          rebuild_task_list(state);
        } else if (command == ID_TASK_DELETE) {
          SendMessageW(window, WM_COMMAND, ID_TASK_DELETE, 0);
        } else if (command == ID_TASK_OPEN_SOURCE) {
          ShellExecuteW(window, L"open", task.source.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (command == ID_TASK_SOURCE_FOLDER) {
          open_folder_for(window, task.source, true);
        } else if (command == ID_TASK_OPEN_OUTPUT && output_ready) {
          ShellExecuteW(window, L"open", task.output.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (command == ID_TASK_OUTPUT_FOLDER && output_ready) {
          open_folder_for(window, task.output, true);
        }
        return 0;
      }
      break;
    }
    case WM_MEASUREITEM: {
      auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
      if (measure && (measure->CtlID == IDC_QUEUE || measure->CtlID == IDC_TASKS)) {
        measure->itemHeight = static_cast<UINT>(MulDiv(
            measure->CtlID == IDC_TASKS ? 52 : 30, GetDpiForWindow(window), 96));
        return TRUE;
      }
      if (measure && measure->CtlType == ODT_COMBOBOX) {
        measure->itemHeight = static_cast<UINT>(MulDiv(
            20, GetDpiForWindow(window), 96));
        return TRUE;
      }
      break;
    }
    case WM_DRAWITEM: {
      auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (draw && draw->CtlType == ODT_COMBOBOX &&
          draw_combobox_item(draw, window, state->font, state->cjk_font,
                             state->dark, state->text_color,
                             state->muted_text_color, state->surface_color)) {
        return TRUE;
      }
      if (!draw || (draw->CtlID != IDC_QUEUE && draw->CtlID != IDC_TASKS) ||
          draw->itemID == static_cast<UINT>(-1)) break;
      const size_t index = static_cast<size_t>(draw->itemID);
      QueueStatus status = QueueStatus::pending;
      if (draw->CtlID == IDC_TASKS) {
        if (index < state->conversion_tasks.size()) status = state->conversion_tasks[index].status;
      } else if (index < state->input_statuses.size()) status = state->input_statuses[index].status;
      const bool selected = (draw->itemState & ODS_SELECTED) != 0;
      const bool error = status == QueueStatus::skipped || status == QueueStatus::failed;
      const bool complete = status == QueueStatus::success;
      const bool processing = status == QueueStatus::running;
      COLORREF background = state->surface_color;
      COLORREF foreground = state->text_color;
      if (selected) {
        background = error
            ? (state->dark ? RGB(91, 43, 47) : RGB(255, 225, 226))
            : (state->dark ? RGB(48, 63, 78) : RGB(219, 232, 245));
      }
      if (error) foreground = state->dark ? RGB(255, 145, 150) : RGB(178, 34, 43);
      if (complete) foreground = state->dark ? RGB(115, 215, 150) : RGB(23, 126, 68);
      if (processing) {
        foreground = state->dark ? RGB(112, 181, 255) : RGB(20, 103, 184);
        if (!selected) background = state->dark ? RGB(31, 48, 66) : RGB(226, 240, 255);
      }
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
      HGDIOBJ original_font = state->font ? SelectObject(draw->hDC, state->font) : nullptr;
      RECT text_rect = draw->rcItem;
      text_rect.left += MulDiv(9, GetDpiForWindow(window), 96);
      text_rect.right -= MulDiv(draw->CtlID == IDC_TASKS ? 104 : 7,
                               GetDpiForWindow(window), 96);
      if (draw->CtlID == IDC_TASKS) {
        RECT card = draw->rcItem;
        const int card_inset = MulDiv(2, GetDpiForWindow(window), 96);
        InflateRect(&card, -card_inset, -card_inset);
        HPEN border_pen = CreatePen(PS_SOLID, 1,
            state->dark ? RGB(75, 82, 91) : RGB(190, 199, 208));
        HGDIOBJ old_pen = SelectObject(draw->hDC, border_pen);
        HGDIOBJ old_brush = SelectObject(draw->hDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(draw->hDC, card.left, card.top, card.right, card.bottom);
        SelectObject(draw->hDC, old_brush);
        SelectObject(draw->hDC, old_pen);
        DeleteObject(border_pen);
        const size_t split = text.find(L"\r\n");
        const std::wstring first = text.substr(0, split);
        const std::wstring second = split == std::wstring::npos ? L"" : text.substr(split + 2u);
        RECT first_rect = text_rect;
        first_rect.top += MulDiv(2, GetDpiForWindow(window), 96);
        first_rect.bottom = first_rect.top + MulDiv(19, GetDpiForWindow(window), 96);
        draw_mixed_text(draw->hDC, state->font, state->cjk_font, first,
                        first_rect, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        SetTextColor(draw->hDC, processing ? foreground : state->muted_text_color);
        RECT second_rect = text_rect;
        second_rect.top = first_rect.bottom;
        second_rect.bottom = second_rect.top + MulDiv(18, GetDpiForWindow(window), 96);
        draw_mixed_text(draw->hDC,
                        state->small_font ? state->small_font : state->font,
                        state->small_cjk_font ? state->small_cjk_font : state->cjk_font,
                        second, second_rect,
                        DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        RECT item_progress = card;
        item_progress.left += MulDiv(7, GetDpiForWindow(window), 96);
        item_progress.right -= MulDiv(103, GetDpiForWindow(window), 96);
        item_progress.top = card.bottom - MulDiv(6, GetDpiForWindow(window), 96);
        item_progress.bottom = card.bottom - MulDiv(3, GetDpiForWindow(window), 96);
        HBRUSH track_brush = CreateSolidBrush(
            state->dark ? RGB(58, 63, 70) : RGB(218, 223, 228));
        FillRect(draw->hDC, &item_progress, track_brush);
        DeleteObject(track_brush);
        if (index < state->conversion_tasks.size()) {
          RECT fill = item_progress;
          fill.right = fill.left + (fill.right - fill.left) *
              std::clamp(state->conversion_tasks[index].progress, 0, 100) / 100;
          HBRUSH fill_brush = CreateSolidBrush(error
              ? RGB(190, 61, 67) : complete ? RGB(39, 156, 86) : RGB(45, 112, 190));
          FillRect(draw->hDC, &fill, fill_brush);
          DeleteObject(fill_brush);
        }
        SetTextColor(draw->hDC, foreground);
        HFONT action_font = CreateFontW(-MulDiv(15, GetDpiForWindow(window), 72), 0, 0, 0,
                                        FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                        L"Segoe UI Symbol");
        HGDIOBJ prior_action_font = action_font ? SelectObject(draw->hDC, action_font) : nullptr;
        RECT icon_rect = draw->rcItem;
        icon_rect.left = icon_rect.right - MulDiv(96, GetDpiForWindow(window), 96);
        RECT edit_rect = icon_rect;
        edit_rect.right = edit_rect.left + (edit_rect.right - edit_rect.left) / 2;
        RECT delete_rect = icon_rect;
        delete_rect.left = edit_rect.right;
        ui::draw(draw->hDC, L"✎", -1, &edit_rect,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        ui::draw(draw->hDC, L"×", -1, &delete_rect,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        if (action_font && prior_action_font) SelectObject(draw->hDC, prior_action_font);
        if (action_font) DeleteObject(action_font);
      } else {
        draw_mixed_text(draw->hDC, state->font, state->cjk_font, text,
                        text_rect, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
      }
      if (original_font) SelectObject(draw->hDC, original_font);
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
        const int format_index = static_cast<int>(SendMessageW(state->format, CB_GETCURSEL, 0, 0));
        const int slot = encoding_slot_for(state, format_index);
        if (format_index >= 0) {
          state->encoding_values[static_cast<size_t>(slot)] = value;
        }
        const bool compression = is_compression_slot(slot);
        ui::set_text(state->quality_value,
                       (std::to_wstring(value) + (compression ? L"" : L"%")).c_str());
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case IDC_TITLE:
          if (HIWORD(wparam) == STN_CLICKED) {
            HMENU menu = CreatePopupMenu();
            if (!menu) return 0;
            AppendMenuW(menu, MF_STRING | (!ui::chinese ? MF_CHECKED : 0), 1, L"English");
            AppendMenuW(menu, MF_STRING | (ui::chinese ? MF_CHECKED : 0), 2, L"简体中文");
            RECT title_rect{};
            GetWindowRect(state->title, &title_rect);
            const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                                               title_rect.left, title_rect.bottom, 0, window, nullptr);
            DestroyMenu(menu);
            if (choice == 0 || (choice == 2) == ui::chinese) return 0;
            ui::chinese = choice == 2;
            ui::save_language();
            ui::apply_dialog_language();
            ui::refresh();
            apply_fonts(state);
            if (state->preview_renderer) state->preview_renderer->refresh_fonts();
            refresh_inspector(state);
            refresh_activity(state);
            set_base_name_cue(state, state->inputs.size() > 1u ? HB_UI("Per-source base names")
                                                               : HB_UI("Base name"));
            RECT client{};
            GetClientRect(window, &client);
            layout(state, client.right, client.bottom);
            if (state->preview_renderer) InvalidateRect(state->preview_renderer->window(), nullptr, TRUE);
            RedrawWindow(window, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
            RedrawWindow(state->title, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
          }
          return 0;
        case IDC_VERSION:
          if (HIWORD(wparam) == STN_CLICKED) start_update_check(state, true);
          return 0;
        case IDC_PREVIEW_TOGGLE:
          state->preview_enabled = !state->preview_enabled;
          if (state->preview_enabled && !state->input.empty()) {
            request_preview(state, state->input);
          } else if (state->preview_enabled && state->preview_renderer) {
            state->preview_renderer->show(true);
          } else if (state->preview_renderer) {
            cancel_preview(state);
            state->preview_renderer->show(false);
          }
          ui::set_text(state->preview_toggle,
                         state->preview_enabled ? HB_UI("Hide") : HB_UI("Preview"));
          break;
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
        case IDC_TASKS:
          if (HIWORD(wparam) == LBN_DBLCLK) {
            const int selected = static_cast<int>(SendMessageW(state->tasks, LB_GETCURSEL, 0, 0));
            if (selected != LB_ERR && static_cast<size_t>(selected) < state->conversion_tasks.size()) {
              const auto& task = state->conversion_tasks[static_cast<size_t>(selected)];
              if (task.status == QueueStatus::success && std::filesystem::exists(task.output)) {
                ShellExecuteW(window, L"open", task.output.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
              }
            }
          }
          break;
        case ID_TASK_DELETE: {
          if (state->running) break;
          const int selected = static_cast<int>(SendMessageW(state->tasks, LB_GETCURSEL, 0, 0));
          if (selected != LB_ERR && static_cast<size_t>(selected) < state->conversion_tasks.size()) {
            state->conversion_tasks.erase(state->conversion_tasks.begin() + selected);
            if (selected == state->editing_task) cancel_task_edit(state);
            else if (selected < state->editing_task) --state->editing_task;
            rebuild_task_list(state);
            EnableWindow(state->cancel_button, !state->conversion_tasks.empty());
            EnableWindow(state->remove_all_tasks, !state->conversion_tasks.empty());
          }
          break;
        }
        case ID_TASK_EDIT: {
          if (state->running) break;
          const int selected = static_cast<int>(SendMessageW(state->tasks, LB_GETCURSEL, 0, 0));
          if (selected != LB_ERR && static_cast<size_t>(selected) < state->conversion_tasks.size()) {
            const auto& task = state->conversion_tasks[static_cast<size_t>(selected)];
            state->editing_task = selected;
            state->input = task.source;
            const std::wstring task_source_text = task.source.wstring();
            ui::send(state->source_path, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(task_source_text.c_str()));
            const int format_index = index_for_mode(task.options.mode);
            const bool task_frame_png =
                format_index == 7 && task.options.conversion.frame_format == "png";
            SendMessageW(state->format, CB_SETCURSEL, format_index, 0);
            if (format_index == 7) {
              SendMessageW(state->frame_export_format, CB_SETCURSEL,
                           task_frame_png ? 1 : 0, 0);
            }
            SendMessageW(state->lossless, BM_SETCHECK,
                         task.options.conversion.lossless ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(state->copy_exif, BM_SETCHECK,
                         task.options.conversion.copy_exif ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(state->copy_xmp, BM_SETCHECK,
                         task.options.conversion.copy_xmp ? BST_CHECKED : BST_UNCHECKED, 0);
            const int quality = (format_index == 1 || task_frame_png)
                ? task.options.conversion.png_compression_level
                : format_index == 2 ? task.options.conversion.tiff_compression_level
                : task.options.conversion.base_quality;
            if (format_index == 2) {
              state->tiff_compressed = task.options.conversion.tiff_compressed;
              SendMessageW(state->tiff_compression, CB_SETCURSEL,
                           state->tiff_compressed ? 0 : 1, 0);
            }
            else if (format_index != 7) state->lossless_setting = task.options.conversion.lossless;
            state->encoding_values[static_cast<size_t>(task_frame_png ? 8 : format_index)] = quality;
            state->last_encoding_slot = -1;
            const int desired_transfer =
                task.options.conversion.output_representation == "gainmap" ? 2 :
                task.options.conversion.output_transfer == "hlg" ? 1 : 0;
            const int scale_index = task.options.conversion.gainmap_scale == 1 ? 2 :
                                    task.options.conversion.gainmap_scale == 4 ? 0 : 1;
            SendMessageW(state->gainmap_resolution, CB_SETCURSEL, scale_index, 0);
            SendMessageW(state->gainmap_channels, CB_SETCURSEL,
                         task.options.conversion.multi_channel_gainmap ? 1 : 0, 0);
            const auto& gm_format = task.options.conversion.gainmap_export_format;
            SendMessageW(state->gainmap_export_format, CB_SETCURSEL,
                         gm_format == "png" ? 1 : gm_format == "tiff" ? 2 :
                         gm_format == "jpeg" ? 3 : 0, 0);
            state->use_source_folder = task.options.use_source_folder;
            state->selected_folder = task.options.selected_folder;
            SendMessageW(state->source_folder, BM_SETCHECK,
                         state->use_source_folder ? BST_CHECKED : BST_UNCHECKED, 0);
            ui::set_text(state->folder_path,
                           (state->use_source_folder ? task.source.parent_path()
                                                     : state->selected_folder).c_str());
            ui::set_text(state->convert, HB_UI("Update task"));
            if (format_index == 7) {
              state->frame_gainmap_resolution = scale_index;
              state->frame_gainmap_channels = task.options.conversion.multi_channel_gainmap ? 1 : 0;
            } else {
              state->normal_gainmap_resolution = scale_index;
              state->uhdr_gainmap_channels = task.options.conversion.multi_channel_gainmap ? 1 : 0;
              state->normal_copy_exif = task.options.conversion.copy_exif;
              state->normal_copy_xmp = task.options.conversion.copy_xmp;
            }
            state->last_format_index = -1;
            state->last_representation_index = -1;
            state->last_frame_export_index = -1;
            update_mode_ui(state, false);
            SendMessageW(state->transfer, CB_SETCURSEL, desired_transfer, 0);
            update_mode_ui(state, false);
            const auto& gamut = task.options.conversion.output_gamut;
            const int desired_cicp = gamut == "p3" ? 12 : gamut == "rec2020" ? 9 : -1;
            const int gamut_count = static_cast<int>(SendMessageW(state->gamut, CB_GETCOUNT, 0, 0));
            for (int item = 0; item < gamut_count; ++item) {
              if (static_cast<int>(SendMessageW(state->gamut, CB_GETITEMDATA, item, 0)) == desired_cicp) {
                SendMessageW(state->gamut, CB_SETCURSEL, item, 0);
                break;
              }
            }
            set_rich_editable_text(state, state->base_name, task.options.base_name);
            set_base_name_cue(state, HB_UI("Base name"));
            set_rich_editable_text(state, state->name_suffix, task.options.suffix);
            SendMessageW(state->use_suffix, BM_SETCHECK, task.options.use_suffix ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow(state->convert, TRUE);
            update_output_preview(state);
          }
          break;
        }
        case IDC_REMOVE: remove_selected_source(state); break;
        case IDC_CLEAR: clear_sources(state); break;
        case IDC_RESET_LAYOUT: {
          state->right_panel_width = 620;
          state->activity_panel_height = 230;
          state->files_panel_height = 270;
          RECT rect{};
          GetClientRect(window, &rect);
          layout(state, rect.right, rect.bottom);
          if (state->preview_renderer) state->preview_renderer->reset_layout();
          save_layout(state);
          break;
        }
        case IDC_RESET_SETTINGS:
          reset_conversion_settings(state);
          save_layout(state);
          break;
        case IDC_FORMAT: if (HIWORD(wparam) == CBN_SELCHANGE) update_mode_ui(state); break;
        case IDC_TRANSFER:
        case IDC_GAMUT:
          if (HIWORD(wparam) == CBN_SELCHANGE) update_mode_ui(state);
          break;
        case IDC_REPRESENTATION:
          if (HIWORD(wparam) == CBN_SELCHANGE) update_mode_ui(state);
          break;
        case IDC_GAINMAP_CHANNELS:
          if (HIWORD(wparam) == CBN_SELCHANGE &&
              (SendMessageW(state->format, CB_GETCURSEL, 0, 0) == 0 ||
               SendMessageW(state->format, CB_GETCURSEL, 0, 0) == 7)) {
            const int selected_channels = std::clamp(static_cast<int>(
                SendMessageW(state->gainmap_channels, CB_GETCURSEL, 0, 0)), 0, 1);
            if (SendMessageW(state->format, CB_GETCURSEL, 0, 0) == 7) state->frame_gainmap_channels = selected_channels;
            else state->uhdr_gainmap_channels = selected_channels;
          }
          break;
        case IDC_LOSSLESS: update_mode_ui(state); break;
        case IDC_TIFF_COMPRESSION:
          if (HIWORD(wparam) == CBN_SELCHANGE) {
            state->tiff_compressed = SendMessageW(state->tiff_compression,
                                                   CB_GETCURSEL, 0, 0) != 1;
            update_mode_ui(state);
          }
          break;
        case IDC_GAINMAP_EXPORT_FORMAT:
          if (HIWORD(wparam) == CBN_SELCHANGE) {
            state->last_format_index = -1;
            update_mode_ui(state);
          }
          break;
        case IDC_FRAME_EXPORT_FORMAT:
          if (HIWORD(wparam) == CBN_SELCHANGE) update_mode_ui(state);
          break;
        case IDC_SOURCE_FOLDER: {
          state->use_source_folder =
              SendMessageW(state->source_folder, BM_GETCHECK, 0, 0) == BST_CHECKED;
          const auto& folder_source = (state->editing_task >= 0 &&
              static_cast<size_t>(state->editing_task) < state->conversion_tasks.size())
              ? state->conversion_tasks[static_cast<size_t>(state->editing_task)].source
              : state->input;
          if (state->use_source_folder && !folder_source.empty()) {
            ui::set_text(state->folder_path, folder_source.parent_path().c_str());
          } else if (!state->use_source_folder && !state->selected_folder.empty()) {
            ui::set_text(state->folder_path, state->selected_folder.c_str());
          }
          update_output_preview(state);
          break;
        }
        case IDC_CHOOSE_FOLDER: choose_output_folder(state); break;
        case IDC_BASE_NAME:
        case IDC_NAME_SUFFIX:
          if (HIWORD(wparam) == EN_CHANGE) update_output_preview(state);
          break;
        case IDC_COLLISION:
          if (HIWORD(wparam) == CBN_SELCHANGE) update_output_preview(state);
          break;
        case IDC_USE_SUFFIX:
          update_output_preview(state);
          break;
        case IDC_CONVERT: add_or_update_task(state); break;
        case IDC_CANCEL:
          if (state->running) {
            state->cancel.store(true);
            append_log(state, HB_UI("Cancellation requested"));
          } else {
            start_task_queue(state);
          }
          break;
        case IDC_REMOVE_ALL_TASKS:
          if (!state->running) {
            state->conversion_tasks.clear();
            cancel_task_edit(state);
            rebuild_task_list(state);
            EnableWindow(state->cancel_button, FALSE);
            EnableWindow(state->remove_all_tasks, FALSE);
          }
          break;
      }
      return 0;
    case WM_APP_PREVIEW_READY: {
      std::unique_ptr<PreviewPayload> payload(
          reinterpret_cast<PreviewPayload*>(lparam));
      if (payload->generation == state->preview_generation &&
          state->preview_renderer && state->preview_enabled) {
        state->preview_renderer->set_image(
            std::move(payload->image), std::move(payload->mip_levels));
      }
      return 0;
    }
    case WM_APP_PREVIEW_ERROR: {
      std::unique_ptr<PreviewErrorPayload> payload(
          reinterpret_cast<PreviewErrorPayload*>(lparam));
      if (payload->generation == state->preview_generation &&
          state->preview_renderer && state->preview_enabled) {
        state->preview_source.clear();
        state->preview_renderer->clear();
        append_log(state, HB_UI("PREVIEW  ") + ui::mark_known(payload->error));
      }
      return 0;
    }
    case WM_APP_PROGRESS: {
      std::unique_ptr<std::wstring> stage(reinterpret_cast<std::wstring*>(lparam));
      ShowWindow(state->progress, SW_SHOW);
      SendMessageW(state->progress, PBM_SETPOS, wparam, 0);
      ui::set_text(state->status, ui::mark_progress(*stage).c_str());
      append_log(state, std::to_wstring(wparam) + L"%  " + ui::mark_progress(*stage));
      return 0;
    }
    case WM_APP_TASK_PROGRESS: {
      std::unique_ptr<TaskProgressPayload> item(
          reinterpret_cast<TaskProgressPayload*>(lparam));
      if (item->index < state->conversion_tasks.size()) {
        state->conversion_tasks[item->index].progress =
            std::clamp(item->item_progress, 0, 100);
        InvalidateRect(state->tasks, nullptr, FALSE);
      }
      ShowWindow(state->progress, SW_SHOW);
      SendMessageW(state->progress, PBM_SETPOS, item->overall_progress, 0);
      ui::set_text(state->status, ui::mark_progress(item->stage).c_str());
      return 0;
    }
    case WM_APP_TASK_OUTPUT: {
      std::unique_ptr<TaskOutputPayload> item(
          reinterpret_cast<TaskOutputPayload*>(lparam));
      if (item->index < state->conversion_tasks.size()) {
        state->conversion_tasks[item->index].output = std::move(item->output);
        rebuild_task_list(state);
      }
      return 0;
    }
    case WM_APP_ITEM_STATUS: {
      std::unique_ptr<ItemStatusPayload> item(
          reinterpret_cast<ItemStatusPayload*>(lparam));
      if (item->index < state->conversion_tasks.size()) {
        state->conversion_tasks[item->index].status = item->status;
        state->conversion_tasks[item->index].progress =
            item->status == QueueStatus::success ? 100 :
            item->status == QueueStatus::running ? 0 :
            state->conversion_tasks[item->index].progress;
        state->conversion_tasks[item->index].reason = item->reason;
        rebuild_task_list(state);
      }
      if (item->status == QueueStatus::skipped) {
        append_log(state, HB_UI("SKIPPED  ") + state->conversion_tasks[item->index].source.filename().wstring() +
                          L" \u2014 " + ui::mark_known(item->reason));
      } else if (item->status == QueueStatus::failed) {
        append_log(state, HB_UI("FAILED  ") + state->conversion_tasks[item->index].source.filename().wstring() +
                          L" \u2014 " + ui::mark_known(item->reason));
      }
      return 0;
    }
    case WM_APP_SUCCESS: {
      std::unique_ptr<SuccessPayload> payload(reinterpret_cast<SuccessPayload*>(lparam));
      set_running(state, false);
      SendMessageW(state->progress, PBM_SETPOS, 100, 0);
      SetTimer(window, kProgressResetTimer, 850, nullptr);
      const std::wstring summary =
          HB_UI("Complete \u2014 ") + std::to_wstring(payload->succeeded) + HB_UI(" succeeded / ") +
          std::to_wstring(payload->skipped) + HB_UI(" skipped / ") +
          std::to_wstring(payload->failed) + HB_UI(" failed");
      ui::set_text(state->status, summary.c_str());
      append_log(state, HB_UI("BATCH SUMMARY  ") + std::to_wstring(payload->succeeded) +
                        HB_UI(" succeeded / ") + std::to_wstring(payload->skipped) +
                        HB_UI(" skipped / ") + std::to_wstring(payload->failed) + HB_UI(" failed"));
      if (payload->results.empty()) {
        state->output.clear();
        ui::set_text(state->output_path, HB_UI("No output generated"));
        return 0;
      }
      state->output = payload->results.back().output_path;
      return 0;
    }
    case WM_APP_RESULT: {
      std::unique_ptr<hdrbridge::ConversionResult> owned(reinterpret_cast<hdrbridge::ConversionResult*>(lparam));
      const auto& result = *owned;
      const auto& v = result.verification;
      std::wostringstream line;
      line << HB_UI("PASS  ") << result.output_path.wstring() << L"  •  " << v.width << L"×" << v.height << L"  " << widen(v.pixel_format)
           << L"  •  " << result.output_bytes << HB_UI(" bytes  •  SHA-256 ") << widen(result.sha256);
      append_log(state, line.str());
      if (v.max_channel_nits > 0.0) {
        std::wostringstream diagnostics;
        diagnostics.setf(std::ios::fixed); diagnostics.precision(2);
        diagnostics << HB_UI("HDR NUMBERS  max channel ") << v.max_channel_nits << HB_UI(" nit  •  max luminance ")
                    << v.max_luminance_nits << L" nit  •  P99.9 " << v.percentile_99_9_nits
                    << L" nit  •  P99.99 " << v.percentile_99_99_nits << HB_UI(" nit  •  chosen peak ")
                    << v.chosen_target_peak_nits << L" nit";
        append_log(state, diagnostics.str());
        append_log(state, HB_UI("PEAK REASON  ") + widen(v.peak_choice_reason));
      }
      if (v.hdr_capacity_max > 0.0 || v.reconstruction_rmse > 0.0) {
        std::wostringstream reconstruction;
        reconstruction.setf(std::ios::fixed); reconstruction.precision(6);
        reconstruction << HB_UI("GAIN MAP  capacity ") << v.hdr_capacity_max << HB_UI("×  •  linear RMSE ")
                       << v.reconstruction_rmse << HB_UI("  •  max abs error ") << v.reconstruction_max_abs_error;
        append_log(state, reconstruction.str());
      }
      const auto& timing = result.timings;
      std::wostringstream performance;
      performance.setf(std::ios::fixed); performance.precision(1);
      performance << HB_UI("TIMING ms  decode ") << timing.decode_ms
                  << HB_UI(" • orientation ") << timing.orientation_ms
                  << HB_UI(" • color ") << timing.color_conversion_ms
                  << HB_UI(" • gain map ") << timing.gain_map_ms
                  << HB_UI(" • encode ") << timing.encode_ms
                  << HB_UI(" • verify ") << timing.verification_ms
                  << HB_UI(" • total ") << timing.total_ms
                  << (timing.canonical_cache_hit ? HB_UI(" • canonical cache HIT") : HB_UI(" • canonical cache MISS"));
      append_log(state, performance.str());
      return 0;
    }
    case WM_APP_ERROR: {
      std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lparam));
      set_running(state, false);
      const bool cancelled = state->cancel.load();
      if (cancelled) {
        for (auto& task : state->conversion_tasks) {
          if (task.status == QueueStatus::running) {
            task.status = QueueStatus::failed;
            task.reason = L"Interrupted";
          }
        }
        rebuild_task_list(state);
      }
      KillTimer(window, kProgressResetTimer);
      SendMessageW(state->progress, PBM_SETPOS, 0, 0);
      ShowWindow(state->progress, SW_HIDE);
      ui::set_text(state->status, cancelled ? HB_UI("Cancelled") : HB_UI("Conversion failed"));
      append_log(state, (cancelled ? HB_UI("CANCELLED  ") : HB_UI("ERROR  ")) + *error);
      if (!cancelled) ui::message_box(window, error->c_str(), HB_UI("HDR Bridge conversion error"), MB_OK | MB_ICONERROR);
      return 0;
    }
    case WM_TIMER:
      if (wparam == kProgressResetTimer) {
        KillTimer(window, kProgressResetTimer);
        SendMessageW(state->progress, PBM_SETPOS, 0, 0);
        ShowWindow(state->progress, SW_HIDE);
        return 0;
      }
      if (wparam == kUpdateCheckTimer) {
        KillTimer(window, kUpdateCheckTimer);
        start_update_check(state, false);
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
      } else if (wparam == 2) {
        state->activity_panel_height = rect.bottom - point.y;
      } else {
        state->files_panel_height = point.y - 88;
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
    case WM_APP_QUEUE_SELECT_ALL: {
      refresh_convert_button(state);
      if (state->input.empty() && !state->inputs.empty()) {
        std::optional<size_t> candidate;
        for (size_t index = 0; index < state->inputs.size(); ++index) {
          const bool failed = index < state->input_statuses.size() &&
              state->input_statuses[index].status == QueueStatus::failed;
          if (failed) continue;
          if (index < state->input_infos.size() && state->input_infos[index]) {
            candidate = index;
            break;
          }
          if (!candidate) candidate = index;
        }
        if (candidate) {
          SendMessageW(state->queue, LB_SETCARETINDEX, *candidate, FALSE);
          load_source(state, state->inputs[*candidate]);
        }
      }
      return 0;
    }
    case WM_CLOSE:
      if (state->running) {
        state->cancel.store(true);
        ui::message_box(window, HB_UI("Cancellation requested. Close HDR Bridge after the conversion worker stops."), L"HDR Bridge", MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      cancel_preview(state);
      if (state->preview_renderer) state->preview_renderer->show(false);
      ShowWindow(window, SW_HIDE);
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      save_layout(state);
      if (state->preview_cancel) state->preview_cancel->store(true);
      state->preview_worker.shutdown();
      RemoveWindowSubclass(state->queue, queue_subclass, 1);
      RemoveWindowSubclass(state->tasks, queue_subclass, 2);
      RemoveWindowSubclass(state->vertical_splitter, splitter_subclass, 1);
      RemoveWindowSubclass(state->horizontal_splitter, splitter_subclass, 1);
      RemoveWindowSubclass(state->files_splitter, splitter_subclass, 1);
      for (HWND label : {state->title, state->version, state->queue_label,
                         state->output_label, state->inspector_label,
                         state->activity_label, state->tasks_label,
                         state->source_path, state->quality_value,
                         state->folder_path, state->status, state->output_path}) {
        RemoveWindowSubclass(label, mixed_static_subclass, 1);
      }
      for (HWND combo : {state->format, state->gamut, state->transfer,
                         state->representation, state->gainmap_resolution,
                         state->gainmap_channels,
                         state->tiff_compression, state->gainmap_export_format,
                         state->frame_export_format,
                         state->collision}) {
        RemoveWindowSubclass(combo, combo_mixed_subclass, 1);
      }
      for (HWND rich : {state->inspector, state->log, state->base_name, state->name_suffix}) {
        RemoveWindowSubclass(rich, rich_edit_subclass, 1);
      }
      if (state->font) DeleteObject(state->font);
      if (state->cjk_font) DeleteObject(state->cjk_font);
      if (state->small_font) DeleteObject(state->small_font);
      if (state->small_cjk_font) DeleteObject(state->small_cjk_font);
      if (state->title_font) DeleteObject(state->title_font);
      if (state->version_font) DeleteObject(state->version_font);
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
      if (control == state->vertical_splitter || control == state->horizontal_splitter ||
          control == state->files_splitter) {
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                      COINIT_DISABLE_OLE1DDE);
  if (FAILED(com_result)) return 1;
  const bool launched_by_setup = std::wstring_view(command_line) == L"--post-install";
  if (launched_by_setup && hdrbridge::app_update::relaunch_from_explorer_if_elevated()) {
    CoUninitialize();
    return 0;
  }
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
  InitCommonControlsEx(&controls);
  WNDCLASSEXW cls{sizeof(cls)};
  cls.style = CS_HREDRAW | CS_VREDRAW;
  cls.lpfnWndProc = window_proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  cls.lpszClassName = kWindowClass;
  cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
  if (!cls.hIcon) cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101),
      IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  if (!cls.hIconSm) cls.hIconSm = cls.hIcon;
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
  if (!launched_by_setup && arguments && argument_count > 1) {
    PostMessageW(window, WM_APP_OPEN_SOURCE, 0,
                 reinterpret_cast<LPARAM>(new std::filesystem::path(arguments[1])));
  }
  if (arguments) LocalFree(arguments);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
      HWND root = GetAncestor(message.hwnd, GA_ROOT);
      wchar_t class_name[64]{};
      wchar_t focus_class[64]{};
      if (root && root != message.hwnd &&
          GetClassNameW(root, class_name, 64) > 0 &&
          wcscmp(class_name, L"HDRBridgePreviewWindow") == 0 &&
          !(GetClassNameW(message.hwnd, focus_class, 64) > 0 &&
            _wcsicmp(focus_class, L"Edit") == 0)) {
        SendMessageW(root, WM_KEYDOWN, message.wParam, message.lParam);
        continue;
      }
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  const int exit_code = static_cast<int>(message.wParam);
  CoUninitialize();
  return exit_code;
}
