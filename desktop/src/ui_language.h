#pragma once

#include <Windows.h>
#include <commctrl.h>
#include <algorithm>
#include <map>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

// Only presentation literals are marked. Paths, metadata, editable values and
// messages received from the conversion engine remain ordinary, untouched text.
#define HB_UI(text) L"\xE000" L##text L"\xE001"

namespace hdrbridge::desktop::i18n {

struct Translation { std::wstring_view english; std::wstring_view chinese; };
inline constexpr Translation translations[] = {
  {L"Reset layout", L"重置布局"}, {L"Reset settings", L"重置设置"},
  {L"Preview", L"预览"}, {L"Hide", L"隐藏"}, {L"FILES", L"文件"},
  {L"OUTPUT", L"输出"}, {L"SOURCE INSPECTOR", L"源文件信息"},
  {L"ACTIVITY", L"活动记录"}, {L"TASKS", L"任务"}, {L"Add files", L"添加文件"},
  {L"Drop or add supported HDR still images", L"拖入或添加支持的 HDR 静态图像"},
  {L"Remove", L"移除"}, {L"Clear", L"清空"}, {L"No file selected", L"未选择文件"},
  {L"Extract Gain Map", L"提取 Gain Map"},
  {L"Gain map: 1/4", L"Gain Map：1/4"},
  {L"Gain map: 1/2", L"Gain Map：1/2"},
  {L"Gain map: Full resolution — default", L"Gain Map：全分辨率 — 默认"},
  {L"Channels: Mono", L"通道：单通道"},
  {L"Channels: RGB — default", L"通道：RGB — 默认"},
  {L"Compression: Deflate — default", L"压缩：Deflate — 默认"},
  {L"Compression: Uncompressed", L"压缩：不压缩"},
  {L"Format: Original — default", L"格式：原始格式 — 默认"},
  {L"Format: PNG", L"格式：PNG"}, {L"Format: TIFF", L"格式：TIFF"},
  {L"Format: JPEG", L"格式：JPEG"},
  {L"Format: Ultra HDR JPEG", L"格式：Ultra HDR JPEG"},
  {L"Lossless", L"无损"},
  {L"Source folder", L"源文件夹"}, {L"Choose...", L"选取…"},
  {L"Base name", L"输出文件名"}, {L"Per-source base names", L"按各源文件名命名"},
  {L"If file exists: auto-number", L"文件已存在：自动编号"},
  {L"If file exists: overwrite", L"文件已存在：覆盖"},
  {L"Use suffix", L"使用后缀"}, {L"Add to queue", L"加入队列"},
  {L"Update task", L"更新任务"}, {L"Remove all", L"全部移除"},
  {L"Start all", L"全部开始"}, {L"Stop", L"停止"}, {L"Ready", L"就绪"},
  {L"No output yet", L"尚无输出"}, {L"No output generated", L"未生成输出"},
  {L"Ready. Detailed conversion activity appears here.", L"就绪。转换活动详情将显示在此处。"},
  {L"Check for updates", L"检查更新"},
  {L"Switch to Simplified Chinese", L"Switch to English / 切换为英语"},
  {L"PQ / ST2084 — default", L"PQ / ST2084 — 默认"}, {L"Gain Map", L"Gain Map"},
  {L"Display P3 — default", L"Display P3 — 默认"},
  {L"Rec.2020 — default", L"Rec.2020 — 默认"},
  {L"Photo Frame", L"照片边框"}, {L"Image", L"图像"},
  {L"Pending", L"待处理"}, {L"PROCESSING", L"处理中"},
  {L"Complete", L"完成"}, {L"Skipped", L"已跳过"}, {L"Failed", L"失败"},
  {L" (Gain Map)", L"（Gain Map）"},
  {L" • Metadata pending", L" • 待读取元数据"}, {L"    Converted", L"    已转换"},
  {L"    Skipped — No HDR data", L"    已跳过 — 无 HDR 数据"},
  {L"    Failed — Decode error", L"    失败 — 解码错误"},
  {L"Inspecting source…", L"正在检查源文件…"}, {L"Source ready", L"源文件已就绪"},
  {L"Inspection failed", L"检查失败"}, {L"Source inspected: ", L"已检查源文件："},
  {L"Skipped unsupported file: ", L"已跳过不支持的文件："},
  {L"Batch queue: ", L"批量队列："}, {L" file(s), sequential processing", L" 个文件，依次处理"},
  {L"SOURCE INSPECTOR\r\n\r\nNo image selected", L"源文件信息\r\n\r\n未选择图像"},
  {L"Converting…", L"正在转换…"}, {L"Starting ", L"开始 "},
  {L"Cancellation requested", L"已请求取消"}, {L"Cancelled", L"已取消"},
  {L"Conversion failed", L"转换失败"}, {L"HDR Bridge conversion error", L"HDR Bridge 转换错误"},
  {L"Cancellation requested. Close HDR Bridge after the conversion worker stops.", L"已请求取消。请在转换任务停止后关闭 HDR Bridge。"},
  {L"Open file", L"打开文件"}, {L"Open containing folder", L"打开所在文件夹"},
  {L"Remove completed", L"移除已完成任务"}, {L"Start task", L"开始任务"},
  {L"Clear task status", L"清除任务状态"}, {L"Delete task", L"删除任务"},
  {L"Open source file", L"打开源文件"}, {L"Open source folder", L"打开源文件夹"},
  {L"Open converted file", L"打开输出文件"}, {L"Open converted folder", L"打开输出文件夹"},
  {L"The Windows file picker could not be created.", L"无法创建 Windows 文件选择器。"},
  {L"The Windows file picker failed.", L"Windows 文件选择器出现错误。"},
  {L"The Windows folder picker could not be created.", L"无法创建 Windows 文件夹选择器。"},
  {L"The Windows folder picker failed.", L"Windows 文件夹选择器出现错误。"},
  {L"All supported HDR images", L"所有支持的 HDR 图像"},
  {L"All files (*.*)", L"所有文件 (*.*)"}, {L"Add HDR images", L"添加 HDR 图像"},
  {L"Choose output folder", L"选择输出文件夹"}, {L"Add", L"添加"}, {L"Select folder", L"选择文件夹"},
  {L"An update check or download is already in progress.", L"正在检查或下载更新。"},
  {L"Finish the current conversion before installing.", L"请在当前转换完成后安装更新。"},
  {L"The installer was verified. Launch it now?", L"安装程序已通过验证。立即运行？"},
  {L"Update ready", L"更新已就绪"}, {L"Update failed", L"更新失败"},
  {L"Update check", L"检查更新"}, {L"Update download failed", L"更新下载失败"},
  {L"The release version could not be compared.", L"无法比较版本号。"},
  {L"You have the latest version.", L"当前已是最新版本。"},
  {L" • update available", L" • 有可用更新"}, {L"Update available: ", L"有可用更新："},
  {L". Click the version to download.", L"。点击版本号即可下载。"},
  {L"Version ", L"新版本 "}, {L" is available. Download the installer?", L" 已发布，是否下载安装程序？"},
  {L"Update available", L"有可用更新"},
  {L"Please wait ", L"请等待 "}, {L" seconds before checking again.", L" 秒后再检查。"},
  {L"Update installer ready. Click the version after conversion to install.", L"更新安装程序已就绪。转换完成后点击版本号安装。"},
  {L"HDR Bridge — Preview", L"HDR Bridge — 预览"},
  {L"HDR Bridge — Preview — ", L"HDR Bridge — 预览 — "},
  {L"HDR Bridge — Preview (unavailable)", L"HDR Bridge — 预览（不可用）"},
  {L"Could not create the HDR preview window.", L"无法创建 HDR 预览窗口。"},
  {L"Fit", L"适应"}, {L"HDR preview is unavailable", L"HDR 预览不可用"},
  {L"HDR preview unavailable\r\n", L"HDR 预览不可用\r\n"},
  {L"Yes", L"是"}, {L"No", L"否"}, {L"OK", L"确定"},
  {L"Present", L"有"}, {L"Absent", L"无"}, {L"Read error", L"读取错误"},
  {L"Unsupported", L"不支持"}, {L"BT.709 / sRGB primaries", L"BT.709 / sRGB 原色"},
  {L"Unspecified / other primaries", L"未指定 / 其他原色"},
  {L"sRGB transfer", L"sRGB 传递函数"}, {L"Linear", L"线性"},
  {L"Unspecified / other transfer", L"未指定 / 其他传递函数"},
  {L"RGB / identity", L"RGB / Identity"},
  {L"BT.2020 non-constant luminance", L"BT.2020 非恒定亮度"}, {L"Matrix ", L"矩阵 "},
  {L"IMAGE\r\n", L"图像\r\n"}, {L"\r\n\r\nCONTAINER\r\n", L"\r\n\r\n容器\r\n"},
  {L"\r\n\r\nBASE RENDITION\r\n", L"\r\n\r\nBASE 图像\r\n"},
  {L" channels", L" 通道"}, {L"Unknown range", L"未知范围"},
  {L"Full range", L"Full range"}, {L"Limited range", L"Limited range"},
  {L"\r\nSource color  ", L"\r\n源色彩  "},
  {L"\r\n\r\nGAIN MAP\r\nFamily  ", L"\r\n\r\nGAIN MAP\r\n类型  "},
  {L"\r\nMap  ", L"\r\n尺寸  "}, {L"RGB (3 channels)", L"RGB（3 通道）"},
  {L"Mono (1 channel)", L"单通道（1 通道）"}, {L"Unknown channels", L"未知通道"},
  {L"\r\nScale  ", L"\r\n缩放比例  "}, {L"\r\nCapacity  ", L"\r\n容量  "},
  {L"\r\nGain min/max  R ", L"\r\n增益 min/max  R "}, {L"\r\nGamma  ", L"\r\nGamma  "},
  {L"\r\nItems  base ", L"\r\n图像项  base "}, {L" / map ", L" / map "},
  {L"\r\nAuxiliary type  ", L"\r\n辅助图类型  "},
  {L"\r\n\r\nRECONSTRUCTED HDR\r\n", L"\r\n\r\n重建 HDR\r\n"},
  {L"\r\n\r\nPRIMARY ITEM\r\n", L"\r\n\r\n主图像\r\n"},
  {L"Grid ", L"网格 "}, {L"\r\nTile ", L"\r\n分块 "}, {L"Single image", L"单张图像"},
  {L"\r\n\r\nSOURCE SIGNAL\r\n", L"\r\n\r\n源信号\r\n"},
  {L"\r\n\r\nNATIVE SIGNAL\r\n", L"\r\n\r\n原生信号\r\n"},
  {L"CICP/NCLX: Absent / Unspecified", L"CICP/NCLX：无 / 未指定"},
  {L"\r\n\r\nICC SIGNAL\r\n", L"\r\n\r\nICC 信号\r\n"},
  {L"Present\r\nProfile  ", L"有\r\n配置文件  "},
  {L"Unreadable / unnamed", L"无法读取 / 未命名"}, {L"\r\nICC version  ", L"\r\nICC 版本  "},
  {L"\r\nICC CICP  Absent", L"\r\nICC CICP  无"},
  {L"\r\nICC transform  ", L"\r\nICC 变换  "}, {L"\r\nInterpretation  ", L"\r\n解释  "},
  {L"\r\n\r\nRESOLVED COLOR\r\n", L"\r\n\r\n解析色彩\r\n"},
  {L"\r\nHDR signaling  ", L"\r\nHDR 信号来源  "},
  {L" — Color signaling conflict", L" — 色彩信号冲突"},
  {L"\r\nRepresentation  ", L"\r\n表示方式  "}, {L"\r\nPrimaries  ", L"\r\n原色  "},
  {L"\r\nTransfer   ", L"\r\n传递函数   "}, {L"\r\nMatrix     ", L"\r\n矩阵     "},
  {L"\r\n\r\nSOURCE METADATA\r\nEXIF  ", L"\r\n\r\n源元数据\r\nEXIF  "},
  {L"\r\nOrientation  ", L"\r\n方向  "}, {L"  •  source ", L"  •  原始 "},
  {L" -> canonical ", L" -> 校正后 "},
  {L"PREVIEW  ", L"预览  "}, {L"SKIPPED  ", L"已跳过  "}, {L"FAILED  ", L"失败  "},
  {L"Complete — ", L"完成 — "}, {L" succeeded / ", L" 成功 / "},
  {L" skipped / ", L" 跳过 / "}, {L" failed", L" 失败"}, {L"BATCH SUMMARY  ", L"批量汇总  "},
  {L"PASS  ", L"通过  "}, {L" bytes  •  SHA-256 ", L" 字节  •  SHA-256 "},
  {L"HDR NUMBERS  max channel ", L"HDR 数值  通道峰值 "},
  {L" nit  •  max luminance ", L" nits  •  最大亮度 "},
  {L" nit  •  chosen peak ", L" nits  •  目标峰值 "}, {L"PEAK REASON  ", L"峰值依据  "},
  {L"GAIN MAP  capacity ", L"GAIN MAP  容量 "}, {L"×  •  linear RMSE ", L"×  •  线性 RMSE "},
  {L"  •  max abs error ", L"  •  最大绝对误差 "},
  {L"TIMING ms  decode ", L"耗时（ms）  解码 "}, {L" • orientation ", L" • 方向 "},
  {L" • color ", L" • 色彩 "}, {L" • gain map ", L" • Gain Map "},
  {L" • encode ", L" • 编码 "}, {L" • verify ", L" • 校验 "}, {L" • total ", L" • 总计 "},
  {L" • canonical cache HIT", L" • HDR 母版缓存命中"},
  {L" • canonical cache MISS", L" • HDR 母版缓存未命中"},
  {L"CANCELLED  ", L"已取消  "}, {L"ERROR  ", L"错误  "},
  {L"No HDR data", L"无 HDR 数据"},
  {L"No HDR gain map or direct HDR signal", L"无 Gain Map 或单层 HDR 信号"},
  {L"Skipped — No HDR data", L"已跳过 — 无 HDR 数据"},
  {L"Failed — Decode error", L"失败 — 解码错误"},
  {L"Select a supported HDR HIF/HEIF, UHDR, PNG, TIFF, AVIF, JXL, JXR, or HSP asset.", L"请选择支持的 HDR 图像（HIF/HEIF、UHDR、PNG、TIFF、AVIF、JXL、JXR 或 HSP）。"},
  {L"conversion cancelled", L"转换已取消"},
  {L"Interrupted", L"已中断"},
  {L"Reading Panasonic HSP container", L"正在读取松下 HSP 容器"},
  {L"Inspecting HEIF container", L"正在检查 HEIF 容器"},
  {L"Color signaling conflict: preserving container/codec YUV decode while using the resolved HDR RGB interpretation", L"色彩信号冲突：保留容器/编解码器 YUV 解码，并采用解析后的 HDR RGB 色彩解释"},
  {L"Matching native and ICC HDR signaling resolved", L"已解析匹配的原生与 ICC HDR 信号"},
  {L"Decoding 20-tile HEVC image at 16-bit precision", L"正在以 16-bit 精度解码 20 分块 HEVC 图像"},
  {L"BT.2100 HLG OETF/OOTF reconstructed to linear HDR (1000-nit reference)", L"已通过 BT.2100 HLG OETF/OOTF 重建线性 HDR（1000 nits 参考）"},
  {L"High-precision Rec.2020/PQ RGB ready", L"高精度 Rec.2020/PQ RGB 已就绪"},
  {L"Transforming Rec.2020/PQ to linear FP16 scRGB", L"正在将 Rec.2020/PQ 转换为线性 FP16 scRGB"},
  {L"Linear FP16 scRGB transform complete", L"线性 FP16 scRGB 转换完成"},
  {L"Resampling linear HDR preview", L"正在重采样线性 HDR 预览"},
  {L"Parsing AVIF base/gain-map/tmap item graph and ISO metadata", L"正在解析 AVIF Base/Gain Map/tmap 结构与 ISO 元数据"},
  {L"Adobe tmap gain-map reconstructed to canonical high-precision HDR master", L"已将 Adobe tmap Gain Map 重建为高精度 HDR 母版"},
  {L"Parsing TIFF base/gain-map SubIFD and ISO metadata", L"正在解析 TIFF Base/Gain Map SubIFD 与 ISO 元数据"},
  {L"Adobe TIFF gain map reconstructed to canonical high-precision HDR master", L"已将 Adobe TIFF Gain Map 重建为高精度 HDR 母版"},
  {L"Parsing Apple HEIC base/auxiliary/tmap item graph and metadata", L"正在解析 Apple HEIC Base/辅助图/tmap 结构与元数据"},
  {L"Apple HEIC gain map reconstructed to canonical high-precision HDR master", L"已将 Apple HEIC Gain Map 重建为高精度 HDR 母版"},
  {L"Inspecting high-dynamic-range input", L"正在检查 HDR 输入"},
  {L"Parsing JPEG XL jhgm base, gain-map codestream, and ISO metadata", L"正在解析 JPEG XL jhgm Base、Gain Map 码流与 ISO 元数据"},
  {L"JPEG XL jhgm reconstructed to canonical high-precision HDR master", L"已将 JPEG XL jhgm 重建为高精度 HDR 母版"},
  {L"Parsing Apple JPEG base/MPF gain map and ISO metadata", L"正在解析 Apple JPEG Base/MPF Gain Map 与 ISO 元数据"},
  {L"Canonical high-precision HDR master ready", L"高精度 HDR 母版已就绪"},
  {L"Source decoded; no HDR representation found", L"源文件已解码；未检测到 HDR 数据"},
  {L"Reusing cached canonical high-precision HDR master", L"正在复用已缓存的高精度 HDR 母版"},
  {L"Parsing Gain Map container and metadata", L"正在解析 Gain Map 容器与元数据"},
  {L"Copying original Gain Map image", L"正在复制原始 Gain Map"},
  {L"Writing decoded Gain Map image", L"正在写入解码后的 Gain Map"},
  {L"Gain Map export committed", L"Gain Map 导出已保存"},
  {L"Compositing source alpha over black in linear light for JPEG output", L"正在线性光下将 Alpha 通道合成至黑色背景以输出 JPEG"},
  {L"Compositing source alpha over black in linear light for RGB-only encoder path", L"正在线性光下将 Alpha 通道合成至黑色背景以供纯 RGB 编码"},
  {L"Encoding SDR base and ISO 21496-1 jhgm JPEG XL gain map", L"正在编码 SDR Base 与 ISO 21496-1 jhgm JPEG XL Gain Map"},
  {L"Verifying JPEG XL jhgm metadata and HDR reconstruction", L"正在校验 JPEG XL jhgm 元数据与 HDR 重建"},
  {L"Encoding JPEG XL BT.2100 HLG master", L"正在编码 JPEG XL BT.2100 HLG 母版"},
  {L"Encoding JPEG XL lossless PQ master", L"正在编码 JPEG XL 无损 PQ 母版"},
  {L"Verifying JPEG XL decode and RGB16 equality", L"正在校验 JPEG XL 解码与 RGB16 一致性"},
  {L"Encoding JPEG XR FP16 scRGB with jxrlib", L"正在使用 jxrlib 编码 JPEG XR FP16 scRGB"},
  {L"Encoding JPEG XR FP16 scRGB with Windows WIC", L"正在使用 Windows WIC 编码 JPEG XR FP16 scRGB"},
  {L"Checking portable JPEG XR FP16 output", L"正在检查跨平台兼容的 JPEG XR FP16 输出"},
  {L"Verifying JPEG XR through WIC FP16 decode", L"正在通过 WIC FP16 解码校验 JPEG XR"},
  {L"Encoding faithful SDR base and ISO 21496-1 gain map with measured capacity", L"正在按实测容量编码保真 SDR Base 与 ISO 21496-1 Gain Map"},
  {L"Resampling linear HDR to the JPEG 65500-pixel codec limit", L"正在将线性 HDR 重采样至 JPEG 65500 像素尺寸上限内"},
  {L"Verifying gain-map capacity and linear-light HDR reconstruction", L"正在校验 Gain Map 容量与线性光 HDR 重建"},
  {L"Encoding direct HDR TIFF RGB16 with Deflate and HDR CICP ICC", L"正在编码单层 HDR TIFF RGB16（Deflate + HDR CICP ICC）"},
  {L"Encoding uncompressed direct HDR TIFF RGB16 with HDR CICP ICC", L"正在编码未压缩单层 HDR TIFF RGB16（HDR CICP ICC）"},
  {L"Verifying TIFF RGB16 equality and direct-HDR ICC", L"正在校验 TIFF RGB16 一致性与单层 HDR ICC"},
  {L"Encoding SDR base and ISO 21496-1 gain map AVIF", L"正在编码 SDR Base 与 ISO 21496-1 Gain Map AVIF"},
  {L"Verifying AVIF gain-map item graph and HDR reconstruction", L"正在校验 AVIF Gain Map 结构与 HDR 重建"},
  {L"Encoding direct 10-bit 4:4:4 BT.2100 HLG AVIF", L"正在编码单层 10-bit 4:4:4 BT.2100 HLG AVIF"},
  {L"Encoding direct 10-bit 4:4:4 PQ AVIF compatibility candidate", L"正在编码单层 10-bit 4:4:4 PQ AVIF 兼容候选格式"},
  {L"Verifying AV1 10-bit 4:4:4 and HDR CICP signaling", L"正在校验 AV1 10-bit 4:4:4 与 HDR CICP 信号"},
  {L"Verified output committed atomically", L"输出已校验并安全写入完成"},
  {L"Encoding direct HDR PNG RGB16 with BT.2100 HLG cICP", L"正在以 BT.2100 HLG cICP 编码单层 HDR PNG RGB16"},
  {L"Encoding direct HDR PNG RGB16 with PQ cICP and ICC", L"正在以 PQ cICP 和 ICC 编码单层 HDR PNG RGB16"},
  {L"Encoding PNG RGB16 with standard PQ cICP only", L"正在仅以标准 PQ cICP 编码 PNG RGB16"},
  {L"Verifying PNG RGB16 equality and cICP signaling", L"正在校验 PNG RGB16 一致性与 cICP 信号"},
  {L"Encoding direct HDR PNG RGBA16 with BT.2100 HLG cICP", L"正在以 BT.2100 HLG cICP 编码单层 HDR PNG RGBA16"},
  {L"Encoding direct HDR PNG RGBA16 with PQ cICP and ICC", L"正在以 PQ cICP 和 ICC 编码单层 HDR PNG RGBA16"},
  {L"Encoding PNG RGBA16 with standard PQ cICP only", L"正在仅以标准 PQ cICP 编码 PNG RGBA16"},
  {L"Verifying PNG RGBA16 equality and cICP signaling", L"正在校验 PNG RGBA16 一致性与 cICP 信号"},
  {L"Could not initialize HTTPS client.", L"无法初始化 HTTPS 客户端。"},
  {L"Could not connect to the release host.", L"无法连接发布服务器。"},
  {L"Could not create HTTPS request.", L"无法创建 HTTPS 请求。"},
  {L"HTTPS request failed.", L"HTTPS 请求失败。"},
  {L"Unexpected redirect from the release host.", L"发布服务器返回了非预期的重定向。"},
  {L"Download redirect did not include a valid location.", L"下载重定向未包含有效位置。"},
  {L"Download redirect was not HTTPS.", L"下载重定向未使用 HTTPS。"},
  {L"Download redirected to an untrusted host.", L"下载被重定向到不受信任的服务器。"},
  {L"Download exceeds the configured size limit.", L"下载超出了配置的大小限制。"},
  {L"Could not read HTTPS response.", L"无法读取 HTTPS 响应。"},
  {L"HTTPS response exceeded the total time limit.", L"HTTPS 响应超出了总时间限制。"},
  {L"Could not initialize SHA-256 verification.", L"无法初始化 SHA-256 验证。"},
  {L"Could not open installer for verification.", L"无法打开安装程序进行验证。"},
  {L"Could not hash installer.", L"无法计算安装程序的哈希值。"},
  {L"Rendering frame background", L"正在渲染边框背景"},
  {L"Encoding framed image", L"正在编码边框图像"},
  {L"Frame output verified and committed", L"照片边框输出已校验并保存"},
  {L"HDR transfer/gamut resolved from ICC; codec/VUI remains responsible for stored YUV-to-RGB decoding", L"已从 ICC 解析 HDR 传递函数/色域；存储的 YUV 到 RGB 解码仍由编解码器/VUI 处理"},
  {L"base name cannot be empty", L"文件名不能为空"},
  {L"base name contains a Windows-invalid character", L"文件名含有 Windows 不允许的字符"},
  {L"suffix contains a Windows-invalid character", L"后缀含有 Windows 不允许的字符"},
  {L"source path is missing", L"缺少源文件路径"},
  {L"output folder is missing", L"未选择输出文件夹"},
  {L"output path must not overwrite the source file", L"输出路径不能覆盖源文件"},
  {L"Could not read installer for verification.", L"无法读取安装程序进行验证。"},
  {L"Could not finish installer SHA-256.", L"无法完成安装程序 SHA-256 计算。"},
  {L"SHA256SUMS contains an invalid installer entry.", L"SHA256SUMS 含有无效的安装程序条目。"},
  {L"SHA256SUMS contains ambiguous installer entries.", L"SHA256SUMS 含有无法确定的安装程序条目。"},
  {L"SHA256SUMS does not contain exactly one stable Windows setup asset.", L"SHA256SUMS 未包含唯一的稳定版 Windows 安装程序。"},
  {L"Latest release is a draft or prerelease.", L"最新发布为草稿或预发布版本。"},
  {L"Release tag is not a stable numeric version.", L"发布标签不是稳定的数字版本号。"},
  {L"Release page URL does not match the trusted repository.", L"发布页面地址与受信任的仓库不匹配。"},
  {L"Invalid release version.", L"发布版本无效。"},
  {L"Release assets are malformed.", L"发布资源格式错误。"},
  {L"Installer asset URL or size is invalid.", L"安装程序的地址或大小无效。"},
  {L"Release contains duplicate installer assets.", L"发布包含重复的安装程序。"},
  {L"Release is missing the installer or its SHA-256 digest.", L"发布缺少安装程序或其 SHA-256 摘要。"},
  {L"Could not parse GitHub release metadata.", L"无法解析 GitHub 发布元数据。"},
  {L"Installer metadata failed validation.", L"安装程序元数据验证失败。"},
  {L"Installer URL is not trusted.", L"安装程序地址不受信任。"},
  {L"Installer URL path does not match its release metadata.", L"安装程序的地址路径与发布元数据不匹配。"},
  {L"Downloaded installer size does not match release metadata.", L"下载的安装程序大小与发布元数据不匹配。"},
  {L"Could not calculate installer SHA-256.", L"无法计算安装程序的 SHA-256。"},
  {L"Installer SHA-256 verification failed.", L"安装程序 SHA-256 验证失败。"},
  {L"Could not locate the temporary directory.", L"无法找到临时目录。"},
  {L"Could not create a unique update directory.", L"无法创建独立的更新目录。"},
  {L"Could not save verified installer.", L"无法保存已验证的安装程序。"},
  {L"Could not flush saved installer.", L"无法将安装程序写入磁盘。"},
  {L"Could not close saved installer.", L"无法关闭已保存的安装程序。"},
  {L"Saved installer failed SHA-256 verification.", L"保存后的安装程序未通过 SHA-256 验证。"},
  {L"Could not finalize verified installer.", L"无法完成已验证安装程序的保存。"},
  {L"Expected SHA-256 is invalid.", L"预期的 SHA-256 无效。"},
  {L"Could not launch the installer.", L"无法启动安装程序。"},
  {L"GitHub API check failed (", L"GitHub API 检查失败（"},
  {L"); checksum fallback failed (", L"）；校验和备用检查失败（"},
  {L").", L"）。"},
  {L"Release host returned HTTP ", L"发布服务器返回 HTTP "},
};

inline bool chinese = false;
inline constexpr wchar_t registry_path[] = L"Software\\HDR Bridge\\Desktop\\Language";

inline void save_language() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, registry_path, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
    const DWORD value = chinese ? 1 : 0;
    RegSetValueExW(key, L"SimplifiedChinese", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
  }
}

inline void apply_dialog_language() {
  const wchar_t english[] = L"en-US\0";
  const wchar_t simplified[] = L"zh-CN\0";
  ULONG count = 0;
  SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, chinese ? simplified : english, &count);
}

inline bool is_simplified_chinese(std::wstring_view name) {
  std::wstring value(name);
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return ch == L'_' ? L'-' : static_cast<wchar_t>(towlower(ch));
  });
  if (value.rfind(L"zh-", 0) != 0) return false;
  const auto has_part = [&](std::wstring_view part) {
    size_t begin = 3;
    while (begin < value.size()) {
      const auto end = value.find(L'-', begin);
      if (std::wstring_view(value).substr(begin, end == std::wstring::npos ? end : end - begin) == part) return true;
      if (end == std::wstring::npos) break;
      begin = end + 1;
    }
    return false;
  };
  if (has_part(L"hans")) return true;
  if (has_part(L"hant")) return false;
  return has_part(L"cn") || has_part(L"sg") || has_part(L"my");
}

inline void load_language() {
  DWORD value = 0, bytes = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, registry_path, L"SimplifiedChinese", RRF_RT_REG_DWORD,
                   nullptr, &value, &bytes) == ERROR_SUCCESS && value <= 1) {
    chinese = value == 1;
  } else {
    // Windows language IDs distinguish Simplified Chinese (including Singapore)
    // from Traditional Chinese. Use the UI language, not the regional format.
    const LANGID language = GetUserDefaultUILanguage();
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0);
    chinese = is_simplified_chinese(name);
    save_language(); // Persist English too; later OS-language changes do not reset it.
  }
  apply_dialog_language();
}

inline std::wstring render(std::wstring_view text) {
  std::wstring result;
  size_t cursor = 0;
  while (cursor < text.size()) {
    const auto begin = text.find(L'\xE000', cursor);
    if (begin == std::wstring_view::npos) { result.append(text.substr(cursor)); break; }
    result.append(text.substr(cursor, begin - cursor));
    const auto end = text.find(L'\xE001', begin + 1);
    if (end == std::wstring_view::npos) { result.append(text.substr(begin)); break; }
    const auto english = text.substr(begin + 1, end - begin - 1);
    auto translated = english;
    if (chinese) for (const auto& entry : translations) {
      if (entry.english == english) { translated = entry.chinese; break; }
    }
    result.append(translated);
    cursor = end + 1;
  }
  return result;
}

// Exact matching is used only at explicit UI boundaries for worker stages and
// updater messages. Unknown diagnostics remain verbatim, including file paths.
inline std::wstring mark_known(std::wstring_view text) {
  for (const auto& entry : translations) if (entry.english == text) {
    return L"\xE000" + std::wstring(text) + L"\xE001";
  }
  return std::wstring(text);
}

inline std::wstring mark_dialog_text(std::wstring_view text) {
  constexpr std::wstring_view prefix = L"GitHub API check failed (";
  constexpr std::wstring_view middle = L"); checksum fallback failed (";
  if (text.starts_with(prefix) && text.ends_with(L").")) {
    const auto split = text.find(middle, prefix.size());
    if (split != std::wstring_view::npos)
      return mark_known(prefix) + mark_dialog_text(text.substr(prefix.size(), split - prefix.size())) +
          mark_known(middle) + mark_dialog_text(text.substr(split + middle.size(), text.size() - split - middle.size() - 2)) +
          mark_known(L").");
  }
  constexpr std::wstring_view http = L"Release host returned HTTP ";
  if (text.starts_with(http)) return mark_known(http) + std::wstring(text.substr(http.size()));
  return mark_known(text);
}

inline std::wstring mark_progress(std::wstring_view text) {
  if (!text.empty() && text.front() == L'[') {
    const auto end = text.find(L"] ");
    if (end != std::wstring_view::npos)
      return std::wstring(text.substr(0, end + 2)) + mark_known(text.substr(end + 2));
  }
  return mark_known(text);
}

inline std::map<HWND, std::wstring> captions;
inline std::map<HWND, std::vector<std::wstring>> items;

inline BOOL set_text(HWND control, const wchar_t* text) {
  if (!control) return FALSE;
  const std::wstring canonical = text ? text : L"";
  if (canonical.find(L'\xE000') != std::wstring::npos) captions[control] = canonical;
  else captions.erase(control);
  return SetWindowTextW(control, render(canonical).c_str());
}

inline LRESULT send(HWND control, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_SETTEXT) return set_text(control, reinterpret_cast<const wchar_t*>(lparam));
  if (message == CB_RESETCONTENT || message == LB_RESETCONTENT) items[control].clear();
  if (message == CB_DELETESTRING || message == LB_DELETESTRING) {
    auto& list = items[control];
    if (wparam < list.size()) list.erase(list.begin() + wparam);
  }
  if (message == CB_ADDSTRING || message == CB_INSERTSTRING ||
      message == LB_ADDSTRING || message == LB_INSERTSTRING) {
    const std::wstring canonical = reinterpret_cast<const wchar_t*>(lparam);
    const auto visible = render(canonical);
    const auto index = SendMessageW(control, message, wparam, reinterpret_cast<LPARAM>(visible.c_str()));
    auto& list = items[control];
    if (index >= 0 && static_cast<size_t>(index) <= list.size()) list.insert(list.begin() + index, canonical);
    return index;
  }
  return SendMessageW(control, message, wparam, lparam);
}

inline int draw(HDC dc, const wchar_t* text, int length, RECT* rect, UINT format) {
  const auto visible = render(std::wstring_view(text, length < 0 ? wcslen(text) : length));
  return DrawTextW(dc, visible.c_str(), static_cast<int>(visible.size()), rect, format);
}

inline BOOL append_menu(HMENU menu, UINT flags, UINT_PTR id, const wchar_t* text) {
  return AppendMenuW(menu, flags, id, text ? render(text).c_str() : nullptr);
}

inline int message_box(HWND owner, const wchar_t* text, const wchar_t* caption, UINT flags) {
  const auto visible = render(mark_dialog_text(text ? text : L""));
  const auto title = render(caption ? caption : L"HDR Bridge");
  const auto yes = render(HB_UI("Yes")), no = render(HB_UI("No")), ok = render(HB_UI("OK"));
  const TASKDIALOG_BUTTON buttons[] = {{IDYES, yes.c_str()}, {IDNO, no.c_str()}, {IDOK, ok.c_str()}};
  TASKDIALOGCONFIG config{sizeof(config)};
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  config.pszWindowTitle = title.c_str();
  config.pszContent = visible.c_str();
  config.pszMainIcon = (flags & MB_ICONMASK) == MB_ICONERROR ? TD_ERROR_ICON :
      (flags & MB_ICONMASK) == MB_ICONWARNING ? TD_WARNING_ICON : TD_INFORMATION_ICON;
  const bool question = (flags & MB_TYPEMASK) == MB_YESNO;
  config.pButtons = question ? buttons : buttons + 2;
  config.cButtons = question ? 2 : 1;
  config.nDefaultButton = question ? IDNO : IDOK;
  int result = 0;
  if (SUCCEEDED(TaskDialogIndirect(&config, &result, nullptr, nullptr))) return result;
  return MessageBoxExW(owner, visible.c_str(), title.c_str(), flags,
      MAKELANGID(chinese ? LANG_CHINESE : LANG_ENGLISH,
                 chinese ? SUBLANG_CHINESE_SIMPLIFIED : SUBLANG_ENGLISH_US));
}

// Refresh existing controls without running option handlers. Selection, item
// data, typed combo values and list scroll positions survive the language change.
inline void refresh() {
  for (const auto& [control, canonical] : captions) {
    if (!IsWindow(control)) continue;
    wchar_t cls[64]{};
    GetClassNameW(control, cls, 64);
    // An editable preview combo may contain an uncommitted typed zoom value.
    // Its list refresh below owns both the selection and displayed text.
    if (_wcsicmp(cls, L"ComboBox") == 0) continue;
    const bool edit = _wcsicmp(cls, L"Edit") == 0;
    DWORD start = 0, end = 0;
    const auto first_line = edit ? SendMessageW(control, EM_GETFIRSTVISIBLELINE, 0, 0) : 0;
    if (edit) SendMessageW(control, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    SetWindowTextW(control, render(canonical).c_str());
    if (edit) {
      SendMessageW(control, EM_SETSEL, start, end);
      SendMessageW(control, EM_LINESCROLL, 0, first_line - SendMessageW(control, EM_GETFIRSTVISIBLELINE, 0, 0));
    }
  }
  for (const auto& [control, list] : items) {
    if (!IsWindow(control) || list.empty()) continue;
    wchar_t cls[64]{};
    GetClassNameW(control, cls, 64);
    const bool combo = _wcsicmp(cls, L"ComboBox") == 0;
    const UINT get_data = combo ? CB_GETITEMDATA : LB_GETITEMDATA;
    const UINT set_data = combo ? CB_SETITEMDATA : LB_SETITEMDATA;
    const auto selection = SendMessageW(control, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0);
    const auto top = SendMessageW(control, combo ? CB_GETTOPINDEX : LB_GETTOPINDEX, 0, 0);
    const auto caret = combo ? 0 : SendMessageW(control, LB_GETCARETINDEX, 0, 0);
    const auto anchor = combo ? 0 : SendMessageW(control, LB_GETANCHORINDEX, 0, 0);
    const auto style = GetWindowLongPtrW(control, GWL_STYLE);
    const bool multi = !combo && (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL));
    const int text_length = GetWindowTextLengthW(control);
    std::wstring typed(static_cast<size_t>(text_length) + 1, L'\0');
    GetWindowTextW(control, typed.data(), text_length + 1);
    typed.resize(text_length);
    std::vector<LPARAM> data;
    std::vector<bool> selected;
    for (size_t index = 0; index < list.size(); ++index) {
      data.push_back(SendMessageW(control, get_data, index, 0));
      selected.push_back(multi && SendMessageW(control, LB_GETSEL, index, 0) > 0);
    }
    const bool visible_control = (style & WS_VISIBLE) != 0;
    // WM_SETREDRAW(TRUE) can add WS_VISIBLE; do not send it to hidden options.
    if (visible_control) SendMessageW(control, WM_SETREDRAW, FALSE, 0);
    SendMessageW(control, combo ? CB_RESETCONTENT : LB_RESETCONTENT, 0, 0);
    for (size_t index = 0; index < list.size(); ++index) {
      const auto visible = render(list[index]);
      const auto inserted = SendMessageW(control, combo ? CB_ADDSTRING : LB_ADDSTRING, 0,
          reinterpret_cast<LPARAM>(visible.c_str()));
      SendMessageW(control, set_data, inserted, data[index]);
      if (selected[index]) SendMessageW(control, LB_SETSEL, TRUE, inserted);
    }
    if (!multi) SendMessageW(control, combo ? CB_SETCURSEL : LB_SETCURSEL, selection, 0);
    if (combo && selection == CB_ERR && (style & CBS_DROPDOWNLIST) != CBS_DROPDOWNLIST)
      SetWindowTextW(control, typed.c_str());
    if (!combo) {
      SendMessageW(control, LB_SETCARETINDEX, caret, FALSE);
      if (multi) SendMessageW(control, LB_SETANCHORINDEX, anchor, 0);
    }
    SendMessageW(control, combo ? CB_SETTOPINDEX : LB_SETTOPINDEX, top, 0);
    if (visible_control) SendMessageW(control, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(control, nullptr, TRUE);
  }
}

} // namespace hdrbridge::desktop::i18n
