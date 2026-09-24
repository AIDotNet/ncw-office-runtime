// ncw-office-helper —— NextCoWork 文档引擎的原生 helper,承载 LibreOfficeKit。
//
// 为了什么需求建的:
//   办公插件要让编辑器画布与 Agent 工具操作「同一个活动文档模型」。引擎是 LibreOffice,
//   而它不能进 Electron 主进程(原生崩溃会带走整个应用)。所以每份打开的文档由一个本
//   helper 进程承载,宿主经 stdio 分帧协议 v1 与它说话(宿主侧见
//   src/main/document-engine/native-host.ts,帧格式见 native-frame.ts)。
//
// 这个文件拥有的不变式:
//   1. stdout 只属于协议。启动第一件事就把原 stdout 复制成协议专用 fd,再把 fd 1 指向
//      stderr —— LibreOffice / fontconfig 会往 stdout 打日志,不这样做它们会插进帧中间,
//      宿主的解码器失步后只能杀掉 helper(症状:大文档偶发「protocol error」)。
//   2. 一批操作先整体校验、再执行。校验不过 → invalid_operation(文档未改动);
//      执行到一半失败 → io(宿主把它记为「结果未知」,不会重放)。
//   3. 只做经过实测的操作。LibreOfficeKit 的 postUnoCommand 回执里的 success 标志对
//      InsertText / EnterString 这类命令并不可靠(实测改了文档仍报 false),所以能用
//      结构读回核对的操作都读回核对(页数、工作表名、幻灯片顺序)。
//
// 故意不做的:
//   - 不执行宏(runMacro)。宏执行需要独立的权限与隔离设计,这一版 capabilities 如实报 false。
//   - 不渲染画布 tile。协议的二进制帧已就位,但 tile 通道要等视图会话通道接上再做。
//   - PDF 由 LibreOffice Draw 导入,保存会整份重写 PDF,不是原样编辑 —— 所以 PDF 的
//     canSave 报 false,只允许导出。

#define LOK_USE_UNSTABLE_API
#ifdef _WIN32
// LibreOfficeKitInit.h 自己会 include <windows.h>;不先关掉 min/max 宏,
// 后面任何一处 std::min / std::max 都会被宏展开成语法错误
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif
#include "third_party/libreofficekit/LibreOfficeKit.hxx"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <cwchar>
// 消息循环与窗口子类化(namespace win)用的是 user32;写在源码里,不依赖构建脚本记得加链接参数
#pragma comment(lib, "user32.lib")
#define ncw_dup _dup
#define ncw_dup2 _dup2
#define ncw_read _read
#define ncw_write _write
#else
#include <unistd.h>
#define ncw_dup dup
#define ncw_dup2 dup2
#define ncw_read read
#define ncw_write write
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "json.hpp"

namespace {

using ncw::Json;
using ncw::quote;

constexpr int kProtocolVersion = 1;
constexpr uint32_t kMaxFrameBytes = 64u * 1024u * 1024u;
constexpr int kCommandTimeoutMs = 15000;
constexpr size_t kMaxQueryChars = 200000;

int g_protocolOut = -1;
std::mutex g_writeMutex;

// ─────────────────────────── 分帧 ───────────────────────────

void writeAll(int fd, const char* data, size_t size) {
  while (size > 0) {
    auto written = ncw_write(fd, data, static_cast<unsigned>(size));
    if (written <= 0) std::_Exit(70);  // 宿主已经不在了,没有可以回报的对象
    data += written;
    size -= static_cast<size_t>(written);
  }
}

void writeFrame(unsigned char type, const char* data, size_t size) {
  uint32_t length = static_cast<uint32_t>(size);
  unsigned char header[5] = {
    static_cast<unsigned char>(length >> 24), static_cast<unsigned char>(length >> 16),
    static_cast<unsigned char>(length >> 8), static_cast<unsigned char>(length), type
  };
  writeAll(g_protocolOut, reinterpret_cast<const char*>(header), 5);
  writeAll(g_protocolOut, data, size);
}

void sendJson(const std::string& json) {
  std::lock_guard<std::mutex> guard(g_writeMutex);
  writeFrame(0, json.data(), json.size());
}

/**
 * 带二进制附件的回执:先发附件帧(类型 1),紧接着发回执 JSON(`result.attachment: true`)。
 *
 * ★ 两帧在同一把锁里连续写:宿主按「回执之前最近的那一个二进制帧」配对附件。中间要是插进
 *   另一条回执,附件就会配给错的请求 —— 表现为画布上出现另一页的内容。
 */
void sendWithAttachment(const std::string& json, const std::vector<unsigned char>& bytes) {
  std::lock_guard<std::mutex> guard(g_writeMutex);
  writeFrame(1, reinterpret_cast<const char*>(bytes.data()), bytes.size());
  writeFrame(0, json.data(), json.size());
}

bool readExact(char* buffer, size_t size) {
  while (size > 0) {
    auto n = ncw_read(0, buffer, static_cast<unsigned>(size));
    if (n <= 0) return false;
    buffer += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

// 返回 false = 宿主关闭了管道。二进制帧(类型 1)宿主目前不会发,读掉丢弃。
bool readFrame(std::string& json) {
  for (;;) {
    unsigned char header[5];
    if (!readExact(reinterpret_cast<char*>(header), 5)) return false;
    uint32_t length = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) | (uint32_t(header[2]) << 8) | uint32_t(header[3]);
    if (length > kMaxFrameBytes) return false;
    std::string payload(length, '\0');
    if (length > 0 && !readExact(&payload[0], length)) return false;
    if (header[4] == 0) { json.swap(payload); return true; }
  }
}

void respondOkWithAttachment(double id, const std::string& resultJson, const std::vector<unsigned char>& bytes) {
  char idbuf[32];
  std::snprintf(idbuf, sizeof idbuf, "%.0f", id);
  sendWithAttachment(std::string("{\"v\":1,\"id\":") + idbuf + ",\"ok\":true,\"result\":" + resultJson + "}", bytes);
}

void respondOk(double id, const std::string& resultJson) {
  char idbuf[32];
  std::snprintf(idbuf, sizeof idbuf, "%.0f", id);
  sendJson(std::string("{\"v\":1,\"id\":") + idbuf + ",\"ok\":true,\"result\":" + resultJson + "}");
}

void respondError(double id, const std::string& code, const std::string& message) {
  char idbuf[32];
  std::snprintf(idbuf, sizeof idbuf, "%.0f", id);
  sendJson(std::string("{\"v\":1,\"id\":") + idbuf + ",\"ok\":false,\"error\":{\"code\":" + quote(code) + ",\"message\":" + quote(message) + "}}");
}

struct HelperError {
  std::string code;
  std::string message;
};

[[noreturn]] void failWith(const std::string& code, const std::string& message) { throw HelperError{code, message}; }

// ─────────────────────────── 路径 ───────────────────────────

std::string fileUrl(const std::string& path) {
  std::string normalized = path;
  for (char& c : normalized) if (c == '\\') c = '/';
  std::string out = "file://";
  if (!normalized.empty() && normalized[0] != '/') out += '/';  // Windows 盘符路径
  static const char* kSafe = "-._~/:";
  for (unsigned char c : normalized) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || std::strchr(kSafe, c) != nullptr) {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string executableDir() {
  std::string path;
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size + 1, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) == 0) path = buf.data();
#elif defined(_WIN32)
  wchar_t wbuf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, wbuf, static_cast<DWORD>(sizeof wbuf / sizeof wbuf[0]));
  int bytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
  path.assign(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(n), &path[0], bytes, nullptr, nullptr);
#else
  std::vector<char> buf(4096, '\0');
  auto n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
  if (n > 0) path.assign(buf.data(), static_cast<size_t>(n));
#endif
  auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

/**
 * LibreOffice 运行时在哪。优先 `--lo-path=`(开发时指向本机安装),否则按插件包布局
 * 在 helper 旁边找 —— 插件发行时 LibreOffice 随包放在 helper 同目录下。
 */
std::string libreOfficePath(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--lo-path=", 0) == 0) return arg.substr(10);
  }
#if defined(__APPLE__)
  return executableDir() + "/LibreOffice.app/Contents/Frameworks";
#else
  return executableDir() + "/libreoffice/program";
#endif
}

// ─────────────────────────── Windows:在 LibreOffice 主线程上处理请求 ───────────────────────────

#ifdef _WIN32
/*
  需求:Windows 上 LibreOfficeKit 只有线程模式 —— unipoll 只在无头后端(svpinst.cxx)里实现,
  Windows 后端(vcl/win)不支持。线程模式下 LibreOffice 的主循环跑在它自己的 lo_startmain 线程,
  文档窗口也归那个线程所有;helper 若在自己的线程里调 LibreOfficeKit,Calc / Impress / Draw
  建视图时会对那些窗口调 SetWindowPos —— 这是一次跨线程的同步 SendMessage,而主循环线程正在
  等 helper 线程持有的 SolarMutex:死锁,documentLoad 永不返回(CI 上用 cdb 抓栈确认,
  线程 0 停在 NtUserSetWindowPos,调用方分别是 Calc 公式栏与 SfxViewShell::SetBorderPixel)。
  普通的 LibreOffice 不会遇到它,因为所有界面操作本来就在主线程。

  做法:找到 VCL 在主线程上建的隐藏窗口(类名 SALCOMWND),子类化它;stdin 读到的请求
  排进队列并向它发一条注册消息,于是请求在 LibreOffice 自己的消息循环里、在主线程上被处理。
  处理中需要等回调时不能阻塞(回调也由这个线程的消息循环派发),改为边跑消息循环边检查
  (见 Engine::waitUntil 与 pause)。macOS / Linux 不走这条路,行为不变。
*/
namespace win {

using RequestHandler = void (*)(const std::string& frame);

UINT g_requestMessage = 0;
HWND g_comWindow = nullptr;
DWORD g_mainThread = 0;
WNDPROC g_originalProc = nullptr;
RequestHandler g_handler = nullptr;
std::mutex g_queueMutex;
std::deque<std::string> g_queue;
bool g_draining = false;  // 只在 LibreOffice 主线程上读写

bool onLibreOfficeMainThread() { return g_mainThread != 0 && GetCurrentThreadId() == g_mainThread; }

/** 跑一会儿消息循环:LibreOffice 的定时器、异步派发、回调刷新都靠它推进 */
void pump(int ms) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= end) return;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - now).count();
    MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(remaining), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
}

void drain() {
  // ★ 嵌套保护:处理请求时 pump 会把后续请求的消息也派发进来,那时不能开始处理下一个 ——
  //   两个请求交错执行,修订号和回执就对不上了。它们留在队列里,由外层这个循环接着处理。
  if (g_draining) return;
  g_draining = true;
  for (;;) {
    std::string frame;
    {
      std::lock_guard<std::mutex> guard(g_queueMutex);
      if (g_queue.empty()) break;
      frame = std::move(g_queue.front());
      g_queue.pop_front();
    }
    g_handler(frame);
  }
  g_draining = false;
}

LRESULT CALLBACK comWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == g_requestMessage) {
    drain();
    return 0;
  }
  return CallWindowProcW(g_originalProc, hwnd, message, wParam, lParam);
}

BOOL CALLBACK findComWindow(HWND hwnd, LPARAM lParam) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId()) return TRUE;  // 别的进程(例如并行的另一个 helper)的窗口
  wchar_t name[32] = {};
  if (GetClassNameW(hwnd, name, 32) > 0 && std::wcscmp(name, L"SALCOMWND") == 0) {
    *reinterpret_cast<HWND*>(lParam) = hwnd;
    return FALSE;
  }
  return TRUE;
}

/** 找到 VCL 主线程的隐藏窗口并接管它的消息。找不到返回 false,调用方退回旧路径 */
bool attach(RequestHandler handler) {
  HWND found = nullptr;
  EnumWindows(&findComWindow, reinterpret_cast<LPARAM>(&found));
  if (found == nullptr) return false;
  g_requestMessage = RegisterWindowMessageW(L"NcwOfficeHelperRequest");
  if (g_requestMessage == 0) return false;
  g_handler = handler;
  g_comWindow = found;
  g_mainThread = GetWindowThreadProcessId(found, nullptr);
  // ★ 先取原窗口过程再替换:反过来的话,替换后、赋值前到达的消息会 CallWindowProc(nullptr)
  g_originalProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(found, GWLP_WNDPROC));
  if (g_originalProc == nullptr) return false;
  SetWindowLongPtrW(found, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&comWindowProc));
  return true;
}

void post(std::string frame) {
  {
    std::lock_guard<std::mutex> guard(g_queueMutex);
    g_queue.push_back(std::move(frame));
  }
  PostMessageW(g_comWindow, g_requestMessage, 0, 0);
}

}  // namespace win
#endif

/** 让出一段时间。Windows 上身处 LibreOffice 主线程时跑消息循环,否则睡眠 */
void pause(int ms) {
#ifdef _WIN32
  if (win::onLibreOfficeMainThread()) {
    win::pump(ms);
    return;
  }
#endif
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ─────────────────────────── 引擎 ───────────────────────────

enum class DocKind { Text, Spreadsheet, Presentation, Drawing, Other };

const char* kindName(DocKind kind) {
  switch (kind) {
    case DocKind::Text: return "text";
    case DocKind::Spreadsheet: return "spreadsheet";
    case DocKind::Presentation: return "presentation";
    case DocKind::Drawing: return "drawing";
    default: return "other";
  }
}

class Engine {
 public:
  explicit Engine(lok::Office* office) : office_(office) {}

  // LibreOfficeKit 回调跑在引擎自己的线程上。只做记录与唤醒,不在这里调 LOK。
  static void onCallback(int type, const char* payload, void* data) {
    auto* self = static_cast<Engine*>(data);
    self->events_.fetch_add(1);
    {
      // 诊断用:最近的回调类型。命令超时时写进错误信息(Windows 上排查回执丢失,见 runUno)
      std::lock_guard<std::mutex> guard(self->mutex_);
      self->recentTypes_.push_back(type);
      if (self->recentTypes_.size() > 16) self->recentTypes_.pop_front();
      if (type == LOK_CALLBACK_UNO_COMMAND_RESULT) {
        self->unoResults_ += 1;
        self->lastUnoResult_ = payload != nullptr ? std::string(payload).substr(0, 300) : "";
      }
    }
    if (payload == nullptr) return;
    if (type == LOK_CALLBACK_SEARCH_RESULT_SELECTION || type == LOK_CALLBACK_SEARCH_NOT_FOUND) {
      std::lock_guard<std::mutex> guard(self->mutex_);
      self->searches_.push_back({type == LOK_CALLBACK_SEARCH_RESULT_SELECTION, payload});
      self->cv_.notify_all();
      return;
    }
    if (type != LOK_CALLBACK_UNO_COMMAND_RESULT) return;
    std::lock_guard<std::mutex> guard(self->mutex_);
    self->results_.emplace_back(payload);
    self->cv_.notify_all();
  }

  bool hasDocument() const { return doc_ != nullptr; }
  DocKind kind() const { return kind_; }

  std::string open(const std::string& path, const std::string& format) {
    if (doc_ != nullptr) failWith("invalid_operation", "a document is already open in this helper");
    // Language=en-US:单元格输入(小数点、布尔、函数名)按固定区域设置解释,
    // 不随用户系统区域漂移 —— 否则同一个 cells.set 在德语系统上会把 1.5 读成文本。
    // Batch=true:无人值守 —— 加载期间的对话框一律静默取消(DialogCancelMode::LOKSilent)。
    // ★ 没人能点一个 helper 进程里弹出的对话框。Windows 上 LibreOfficeKit 用的是真实窗口后端,
    //   Calc / Impress / Draw 加载完导入后卡在建视图那一步(导入进度已到 100%,
    //   documentLoad 永不返回,CI 实测);soffice --headless 在同一台机器上正常。
    stage("open: loading " + format);
    lok::Document* doc = office_->documentLoad(fileUrl(path).c_str(), "Language=en-US,Batch=true");
    if (doc == nullptr) {
      char* error = office_->getError();
      failWith("unsupported_format", std::string("LibreOffice could not load the document: ") + (error != nullptr ? error : "unknown error"));
    }
    doc_ = doc;
    format_ = format;
    switch (doc->getDocumentType()) {
      case LOK_DOCTYPE_TEXT: kind_ = DocKind::Text; break;
      case LOK_DOCTYPE_SPREADSHEET: kind_ = DocKind::Spreadsheet; break;
      case LOK_DOCTYPE_PRESENTATION: kind_ = DocKind::Presentation; break;
      case LOK_DOCTYPE_DRAWING: kind_ = DocKind::Drawing; break;
      default: kind_ = DocKind::Other;
    }
    /*
      ★ 回调必须在 initializeForRendering **之前**注册,并等到第一批事件到达再接受命令。
      实测:否则第一条 postUnoCommand 有概率既不执行也不回执(15 秒超时),
      而同样的命令第二次就好了 —— 典型的「偶现」。
    */
    doc->registerCallback(&Engine::onCallback, this);
    stage("open: loaded, type " + std::to_string(doc->getDocumentType()));
    doc->initializeForRendering("{}");
    for (int i = 0; i < 100 && events_.load() == 0; ++i) pause(20);
    pause(200);
    stage("open: initialized, " + std::to_string(events_.load()) + " callbacks");
#ifdef _WIN32
    activateDocumentFrame();
    stage("open: document frame activated");
#endif
    if (kind_ == DocKind::Text) loadParagraphStyles();
    return describe();
  }

  /**
   * 让这份文档的窗口成为桌面的「活动框架」。
   *
   * 需求:postUnoCommand 经 comphelper::dispatchCommand 派发,目标取的是
   * Desktop::getActiveFrame()。macOS / Linux 上加载完的隐藏窗口会自动成为活动框架;
   * Windows 上隐藏窗口收不到激活消息,活动框架为空,命令落到桌面上找不到处理者,
   * 表现为「每条编辑命令都 Failed to dispatch / 等不到回执」(Windows CI 实测,回调照常在发)。
   *
   * 只在 Windows 上调用:macOS / Linux 不需要,而且在 macOS CI 上加了它之后 PDF 导出会让
   * LibreOffice 崩溃("Unspecified Application Error",本机未复现)。
   *
   * SfxLokHelper::setView 在**切换到非当前视图**时会调用 Desktop::setActiveFrame,
   * 对当前视图则直接返回。所以借一个临时视图:建新视图(它成为当前)→ 切回原视图
   * (触发 setActiveFrame)→ 销毁临时视图。★ 看起来多余,删掉它 Windows 就不能编辑。
   */
  static void stage(const std::string& message) {
    std::fprintf(stderr, "[ncw-office-helper] %s\n", message.c_str());
    std::fflush(stderr);
  }

  void activateDocumentFrame() {
    int original = doc_->getView();
    int temporary = doc_->createView();
    if (temporary < 0 || temporary == original) return;
    doc_->setView(original);
    doc_->destroyView(temporary);
    doc_->setView(original);
  }

  std::string describe() {
    std::string ops;
    bool canSave = true;
    switch (kind_) {
      case DocKind::Text: ops = "\"text.insert\",\"text.findReplace\",\"paragraph.style\",\"paragraph.insert\""; break;
      case DocKind::Spreadsheet: ops = "\"cells.set\",\"cells.formula\",\"sheet.insert\""; break;
      case DocKind::Presentation: ops = "\"slide.insert\",\"slide.move\""; break;
      case DocKind::Drawing: canSave = false; break;  // PDF 经 Draw 导入,保存会重写,见文件头
      default: canSave = false;
    }
    /*
      可导出格式:PDF + 同类的无宏 OOXML。原先报空,是因为宿主还没有导出方法与路径策略;
      现在宿主有了 `export`(目标路径由核心给、不覆盖已有文件),所以如实报出来。
      ★ 不报跨类格式(docx → xlsx):LibreOffice 会拒绝或产出空壳,画出来就是一个必失败的按钮。
    */
    std::string out = "{\"capabilities\":{\"operations\":[" + ops + "],\"canSave\":" + (canSave ? "true" : "false") +
                      ",\"canExport\":[" + exportList() + "],\"canUndo\":false,\"macros\":{\"list\":false,\"run\":false}}";
    out += ",\"documentType\":" + quote(kindName(kind_)) + ",\"parts\":" + std::to_string(doc_->getParts()) + ",\"partNames\":[";
    int parts = doc_->getParts();
    for (int i = 0; i < parts && i < 1000; ++i) {
      if (i > 0) out += ',';
      out += quote(takeString(doc_->getPartName(i)));
    }
    return out + "]}";
  }

  // ─────── 修改 ───────

  // 返回与 operations 一一对应的结果数组(JSON)
  std::string apply(const Json& operations) {
    requireDocument();
    if (!operations.isArray() || operations.array.empty()) failWith("invalid_operation", "operations must be a non-empty array");
    // 不变式 2:先整批校验,一条不合法就一条都不执行。
    // ★ 校验要「模拟」前面操作对页数 / 表名的影响:同一批里先插两页再挪第 3 页是合法的,
    //   拿执行前的页数去判会把它误拒(实测踩过)。
    ValidationState state{doc_->getParts(), {}};
    for (int i = 0; i < state.parts; ++i) state.names.push_back(takeString(doc_->getPartName(i)));
    for (const Json& op : operations.array) validate(op, state);
    size_t done = 0;
    std::string results = "[";
    try {
      for (const Json& op : operations.array) {
        std::string result = execute(op);
        results += (done > 0 ? "," : "") + result;
        ++done;
      }
    } catch (const HelperError& error) {
      /*
        ★ 只有第一条就被拒、且拒绝来自「执行前的核对」(匹配数不符等)时才算文档未改动。
        按文字定位的操作,匹配数要到执行时才能数 —— 前面的操作可能改变了它 —— 所以后面
        那几条的核对失败只能报 io,宿主据此记为「结果未知」。
      */
      if (done == 0 && error.code == "invalid_operation") throw;
      failWith("io", error.message + " (after " + std::to_string(done) + " operation(s) had been applied)");
    }
    return results + "]";
  }

  // ─────── 查询 ───────

  std::string query(const Json& params) {
    requireDocument();
    std::string kind = params.str("kind");
    size_t maxChars = kMaxQueryChars;
    if (const Json* limit = params.get("maxChars"); limit != nullptr && limit->isNumber() && limit->number > 0 && limit->number < kMaxQueryChars) {
      maxChars = static_cast<size_t>(limit->number);
    }
    if (kind == "outline") return describe();
    if (kind == "layout") return layout(params);
    if (kind == "text") {
      if (kind_ != DocKind::Text) failWith("unsupported_operation", "text query is only available for word processing documents");
      runUno(".uno:SelectAll", "{}");
      std::string text = selectionText();
      doc_->resetSelection();
      return clipped(text, maxChars);
    }
    if (kind == "cells") {
      if (kind_ != DocKind::Spreadsheet) failWith("unsupported_operation", "cells query is only available for spreadsheets");
      selectSheet(params.str("sheet"));
      std::string range = params.str("range");
      if (!isRange(range)) failWith("invalid_operation", "range must look like A1 or A1:C10");
      runUno(".uno:GoToCell", "{\"ToPoint\":{\"type\":\"string\",\"value\":" + quote(range) + "}}");
      return clipped(selectionText(), maxChars);
    }
    failWith("invalid_operation", "unknown query kind: " + kind);
  }

  // ─────── 渲染 ───────

  /*
    需求:办公编辑器要像 Office 一样显示真实页面(排版、字体、图片都由引擎算),所以视图
    向引擎要「这一块区域画成多少像素」的位图。坐标用 twips(1/1440 英寸,LibreOffice 的
    文档坐标),像素尺寸由视图按缩放决定 —— 引擎只负责画,不关心窗口多大。

    ★ 单次渲染的像素上限:一次 2048×2048 RGBA 是 16 MiB,已经是分帧上限的四分之一。
      更大的区域视图要按块(tile)切开要,否则一次 8K 渲染就让 helper 分配 256 MiB。
  */
  static constexpr int kMaxRenderPixels = 2048;
  static constexpr long kMaxTwips = 1L << 30;

  /**
   * 版面:当前 / 指定部分的文档尺寸(twips);Writer 另给每一页的矩形。
   * ★ 量尺寸要切到那个部分(getDocumentSize 只回答当前部分),量完切回原来的部分 ——
   *   查询不应该改变编辑状态,否则 Agent 查一次版面,接下来的单元格写入就落到了别的表上。
   */
  std::string layout(const Json& params) {
    int parts = doc_->getParts();
    int original = doc_->getPart();
    int part = original;
    if (const Json* value = params.get("part"); value != nullptr) {
      if (kind_ == DocKind::Text) failWith("invalid_operation", "word processing documents have no parts; omit part");
      if (!value->isNumber() || value->number < 0 || value->number >= parts) failWith("invalid_operation", "part is out of range");
      part = static_cast<int>(value->number);
    }
    if (part != original) doc_->setPart(part);
    long width = 0;
    long height = 0;
    doc_->getDocumentSize(&width, &height);
    if (part != original) doc_->setPart(original);
    std::string out = "{\"documentType\":" + quote(kindName(kind_)) + ",\"parts\":" + std::to_string(parts) +
                      ",\"part\":" + std::to_string(part) + ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height);
    if (kind_ == DocKind::Text) out += ",\"pages\":" + pageRectangles();
    return out + "}";
  }

  /** Writer 的页面矩形,`[{x,y,width,height}]`(twips)。解析不了的片段跳过 */
  std::string pageRectangles() {
    std::string raw = takeString(doc_->getPartPageRectangles());
    std::string out = "[";
    size_t start = 0;
    int count = 0;
    while (start < raw.size() && count < 10000) {
      size_t end = raw.find(';', start);
      std::string item = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
      long v[4] = {0, 0, 0, 0};
      if (std::sscanf(item.c_str(), " %ld , %ld , %ld , %ld", &v[0], &v[1], &v[2], &v[3]) == 4 && v[2] > 0 && v[3] > 0) {
        out += std::string(count > 0 ? "," : "") + "{\"x\":" + std::to_string(v[0]) + ",\"y\":" + std::to_string(v[1]) +
               ",\"width\":" + std::to_string(v[2]) + ",\"height\":" + std::to_string(v[3]) + "}";
        ++count;
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
    return out + "]";
  }

  struct Rendered {
    int width;
    int height;
    std::vector<unsigned char> rgba;
  };

  /**
   * 把一块文档区域画成 RGBA(非预乘,每行 width*4 字节,无填充)—— 视图直接塞进 ImageData。
   *
   * ★ 引擎给的是**预乘** alpha 的 BGRA(或 RGBA,看 getTileMode):不转成非预乘,半透明的
   *   抗锯齿边缘在 canvas 上会发暗发脏;不换通道,整页红蓝对调。
   */
  Rendered render(const Json& params) {
    requireDocument();
    auto number = [&](const char* key, double min, double max) -> long {
      const Json* value = params.get(key);
      if (value == nullptr || !value->isNumber() || value->number < min || value->number > max || value->number != static_cast<double>(static_cast<long>(value->number))) {
        failWith("invalid_operation", std::string(key) + " must be an integer between " + std::to_string(static_cast<long>(min)) + " and " + std::to_string(static_cast<long>(max)));
      }
      return static_cast<long>(value->number);
    };
    long x = number("x", 0, kMaxTwips);
    long y = number("y", 0, kMaxTwips);
    long tileWidth = number("tileWidth", 1, kMaxTwips);
    long tileHeight = number("tileHeight", 1, kMaxTwips);
    int width = static_cast<int>(number("width", 1, kMaxRenderPixels));
    int height = static_cast<int>(number("height", 1, kMaxRenderPixels));
    Rendered out{width, height, std::vector<unsigned char>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0)};
    const Json* partValue = params.get("part");
    if (partValue != nullptr && kind_ != DocKind::Text) {
      int part = static_cast<int>(number("part", 0, doc_->getParts() - 1));
      doc_->paintPartTile(out.rgba.data(), part, 0, width, height, static_cast<int>(x), static_cast<int>(y), static_cast<int>(tileWidth), static_cast<int>(tileHeight));
    } else {
      if (partValue != nullptr) failWith("invalid_operation", "word processing documents have no parts; omit part");
      doc_->paintTile(out.rgba.data(), width, height, static_cast<int>(x), static_cast<int>(y), static_cast<int>(tileWidth), static_cast<int>(tileHeight));
    }
    const bool bgra = doc_->getTileMode() == LOK_TILEMODE_BGRA;
    for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
      unsigned char* px = &out.rgba[i];
      if (bgra) std::swap(px[0], px[2]);
      unsigned a = px[3];
      if (a != 0 && a != 255) {
        for (int c = 0; c < 3; ++c) px[c] = static_cast<unsigned char>(std::min(255u, (px[c] * 255u + a / 2) / a));
      }
    }
    return out;
  }

  // ─────── 保存 ───────

  std::vector<std::string> exportFormats() const {
    switch (kind_) {
      case DocKind::Text: return {"pdf", "docx"};
      case DocKind::Spreadsheet: return {"pdf", "xlsx"};
      case DocKind::Presentation: return {"pdf", "pptx"};
      case DocKind::Drawing: return {"pdf"};
      default: return {};
    }
  }

  std::string exportList() const {
    std::string out;
    for (const std::string& format : exportFormats()) out += (out.empty() ? "" : ",") + quote(format);
    return out;
  }

  /**
   * 写到宿主给的路径。允许的格式 = 文档自己的格式(保存)+ exportFormats(导出)。
   * ★ 其余一律拒绝:把 docx 文档按 xlsx 过滤器写出去,LibreOffice 可能报成功却产出空壳文件。
   */
  void saveAs(const std::string& path, const std::string& format) {
    requireDocument();
    bool allowed = format == format_;
    for (const std::string& candidate : exportFormats()) allowed = allowed || format == candidate;
    if (!allowed) failWith("unsupported_format", "cannot write a " + std::string(kindName(kind_)) + " document as ." + format);
    if (!doc_->saveAs(fileUrl(path).c_str(), format.c_str(), nullptr)) {
      char* error = office_->getError();
      failWith("io", std::string("LibreOffice failed to save: ") + (error != nullptr ? error : "unknown error"));
    }
  }

 private:
  lok::Office* office_;
  lok::Document* doc_ = nullptr;
  DocKind kind_ = DocKind::Other;
  std::string format_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> results_;
  struct SearchEvent {
    bool found;
    std::string payload;
  };
  std::deque<SearchEvent> searches_;
  std::vector<std::string> paragraphStyles_;
  std::deque<int> recentTypes_;
  int unoResults_ = 0;
  std::string lastUnoResult_;
  std::atomic<int> events_{0};

  void requireDocument() const {
    if (doc_ == nullptr) failWith("invalid_operation", "no document is open");
  }

  std::string takeString(char* value) {
    std::string out = value != nullptr ? value : "";
    if (value != nullptr) office_->freeError(value);
    return out;
  }

  std::string clipped(const std::string& text, size_t maxChars) {
    bool truncated = text.size() > maxChars;
    std::string body = truncated ? text.substr(0, maxChars) : text;
    // 按字节截断可能切在 UTF-8 中间:退到最近的字符起点,宿主不会收到半个汉字
    while (truncated && !body.empty() && (static_cast<unsigned char>(body.back()) & 0xC0) == 0x80) body.pop_back();
    if (truncated && !body.empty() && (static_cast<unsigned char>(body.back()) & 0x80) != 0) body.pop_back();
    return "{\"text\":" + quote(body) + ",\"truncated\":" + (truncated ? "true" : "false") + "}";
  }

  std::string selectionText() {
    char* used = nullptr;
    char* text = doc_->getTextSelection("text/plain;charset=utf-8", &used);
    if (used != nullptr) office_->freeError(used);
    return takeString(text);
  }

  /**
   * 等条件成立或超时。
   *
   * ★ Windows 上请求在 LibreOffice 主线程里处理,而回调也由这个线程的消息循环派发 ——
   *   在这里阻塞等条件变量,回调永远不会来。所以那种情况下改为边跑消息循环边检查。
   */
  template <class Predicate>
  bool waitUntil(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline, Predicate predicate) {
#ifdef _WIN32
    if (win::onLibreOfficeMainThread()) {
      for (;;) {
        if (predicate()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        lock.unlock();
        win::pump(10);
        lock.lock();
      }
    }
#endif
    return cv_.wait_until(lock, deadline, predicate);
  }

  /**
   * 派发一条 UNO 命令并等它的完成回执。
   *
   * ★ 按 commandName 配对,而不是「收到任意一条就算」:引擎有时会为之前的命令迟到地
   * 补一条回执,拿它当这一条的结果,后面的读回核对就全错位了。
   */
  std::string runUno(const std::string& command, const std::string& args) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      results_.clear();
    }
    doc_->postUnoCommand(command.c_str(), args.c_str(), true);
    /*
      ★ 派发失败时 LibreOffice 不会发回执,只在 getError 里留一句 "Failed to dispatch"
      (doc_postUnoCommand → comphelper::dispatchCommand 返回 false)。不先查它的话,
      表现是每条命令都干等满 15 秒超时 —— Windows CI 上首次就是这样把整组测试拖到超时的。
    */
    if (char* error = office_->getError()) {
      std::string message = error;
      office_->freeError(error);
      if (message.find("Failed to dispatch") != std::string::npos) failWith("io", command + ": " + message);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCommandTimeoutMs);
    for (;;) {
      while (!results_.empty()) {
        std::string payload = results_.front();
        results_.pop_front();
        try {
          if (ncw::parseJson(payload).str("commandName") == command) return payload;
        } catch (const std::exception&) {
          // 解析不了的回执不是我们等的那一条
        }
      }
      if (!waitUntil(lock, deadline, [this] { return !results_.empty(); })) {
        // 诊断信息:回调总数(0 = 事件循环没跑)、最近的回调类型、收到过几条命令回执及最后一条的内容
        std::string types;
        for (int t : recentTypes_) types += (types.empty() ? "" : ",") + std::to_string(t);
        failWith("timeout", command + " did not complete within the engine timeout (" + std::to_string(events_.load()) +
                                " engine callbacks seen; recent types [" + types + "]; " + std::to_string(unoResults_) +
                                " command results so far; last: " + lastUnoResult_ + ")");
      }
    }
  }

  // ─────── 查找 ───────

  static constexpr int kSearchFind = 0;
  static constexpr int kSearchFindAll = 1;
  static constexpr int kSearchReplaceAll = 3;

  /*
    ★ 字面查找,不是正则:显式把 AlgorithmType2 设成 ABSOLUTE(1)。LibreOffice 会从 profile
    里沿用上一次的查找选项;一旦有人在同一 profile 里开过正则,"1.5" 就会匹配到 "105"。
    TransliterateFlags 256 = IGNORE_CASE;区分大小写时传 0。
  */
  static std::string searchArgs(const std::string& find, const std::string& replace, int command, bool matchCase) {
    return "{\"SearchItem.SearchString\":{\"type\":\"string\",\"value\":" + quote(find) +
           "},\"SearchItem.ReplaceString\":{\"type\":\"string\",\"value\":" + quote(replace) +
           "},\"SearchItem.Backward\":{\"type\":\"boolean\",\"value\":false}" +
           ",\"SearchItem.Command\":{\"type\":\"long\",\"value\":" + std::to_string(command) +
           "},\"SearchItem.AlgorithmType2\":{\"type\":\"short\",\"value\":1}" +
           ",\"SearchItem.SearchFlags\":{\"type\":\"long\",\"value\":0}" +
           ",\"SearchItem.TransliterateFlags\":{\"type\":\"long\",\"value\":" + (matchCase ? "0" : "256") + "}}";
  }

  static bool flag(const Json& op, const char* key) {
    const Json* value = op.get(key);
    return value != nullptr && value->isBool() && value->boolean;
  }

  static bool contains(const std::string& haystack, const std::string& needle, bool matchCase) {
    if (matchCase) return haystack.find(needle) != std::string::npos;
    auto lower = [](std::string text) {
      for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      return text;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
  }

  /**
   * 全文命中数。FIND_ALL 之后所有命中处处于选中状态(调用方可以接着对它们做事)。
   *
   * 命中数取自 LOK_CALLBACK_SEARCH_RESULT_SELECTION 的 searchResultSelection 数组长度;
   * 没有命中时引擎发 SEARCH_NOT_FOUND。实测这些回调先于命令回执到达,但仍留一小段等待,
   * 不把「回调还没到」误判成「零命中」。
   */
  int countMatches(const std::string& find, bool matchCase) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      searches_.clear();
    }
    runUno(".uno:ExecuteSearch", searchArgs(find, "", kSearchFindAll, matchCase));
    std::unique_lock<std::mutex> lock(mutex_);
    waitUntil(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(1000), [this] { return !searches_.empty(); });
    for (auto it = searches_.rbegin(); it != searches_.rend(); ++it) {
      if (!it->found) return 0;
      try {
        Json parsed = ncw::parseJson(it->payload);
        const Json* selection = parsed.get("searchResultSelection");
        if (parsed.str("searchString") == find && selection != nullptr && selection->isArray()) return static_cast<int>(selection->array.size());
      } catch (const std::exception&) {}
    }
    failWith("io", "the engine did not report search results for: " + find);
  }

  /** 从光标处向后找下一处并选中它。找不到返回 false */
  bool findNext(const std::string& find, bool matchCase) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      searches_.clear();
    }
    runUno(".uno:ExecuteSearch", searchArgs(find, "", kSearchFind, matchCase));
    std::unique_lock<std::mutex> lock(mutex_);
    waitUntil(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(1000), [this] { return !searches_.empty(); });
    return !searches_.empty() && searches_.back().found;
  }

  /** 数命中,并按 expectedCount 与「至少一处」核对。核对失败时文档未改动。 */
  int checkedMatches(const Json& op, const std::string& find, bool matchCase) {
    int matches = countMatches(find, matchCase);
    if (matches == 0) failWith("invalid_operation", "no match for: " + find);
    const Json* expected = op.get("expectedCount");
    if (expected != nullptr && expected->isNumber() && static_cast<int>(expected->number) != matches) {
      failWith("invalid_operation", "expected " + std::to_string(static_cast<int>(expected->number)) + " match(es) but found " + std::to_string(matches) + ": " + find);
    }
    return matches;
  }

  void insertLines(const std::string& text) {
    size_t start = 0;
    for (;;) {
      size_t newline = text.find('\n', start);
      std::string line = text.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) runUno(".uno:InsertText", "{\"Text\":{\"type\":\"string\",\"value\":" + quote(line) + "}}");
      if (newline == std::string::npos) break;
      runUno(".uno:InsertPara", "{}");
      start = newline + 1;
    }
  }

  // Writer 的段落样式名(程序名,如 "Heading 1"),打开文档时读一次
  void loadParagraphStyles() {
    char* values = doc_->getCommandValues(".uno:StyleApply");
    std::string raw = takeString(values);
    try {
      Json parsed = ncw::parseJson(raw);
      const Json* commandValues = parsed.get("commandValues");
      const Json* styles = commandValues != nullptr ? commandValues->get("ParagraphStyles") : nullptr;
      if (styles != nullptr && styles->isArray()) {
        for (const Json& style : styles->array) if (style.isString()) paragraphStyles_.push_back(style.string);
      }
    } catch (const std::exception&) {
      // 读不到样式表时 paragraph.style 会因「未知样式」被拒,不会误套
    }
  }

  std::string styleSample() const {
    std::string out;
    for (size_t i = 0; i < paragraphStyles_.size() && i < 12; ++i) out += (i > 0 ? ", " : "") + paragraphStyles_[i];
    return out.empty() ? "(none)" : out;
  }

  // ─────── 单元格地址 ───────

  static bool parseCell(const std::string& ref, int& col, int& row) {
    size_t i = 0;
    if (i < ref.size() && ref[i] == '$') ++i;
    col = 0;
    size_t letters = 0;
    while (i < ref.size() && ref[i] >= 'A' && ref[i] <= 'Z' && letters < 3) { col = col * 26 + (ref[i] - 'A' + 1); ++i; ++letters; }
    if (letters == 0) return false;
    if (i < ref.size() && ref[i] == '$') ++i;
    row = 0;
    size_t digits = 0;
    while (i < ref.size() && ref[i] >= '0' && ref[i] <= '9' && digits < 7) { row = row * 10 + (ref[i] - '0'); ++i; ++digits; }
    return digits > 0 && row > 0 && i == ref.size() && col <= 16384 && row <= 1048576;
  }

  static bool isRange(const std::string& range) {
    int c = 0, r = 0;
    auto colon = range.find(':');
    if (colon == std::string::npos) return parseCell(range, c, r);
    int c2 = 0, r2 = 0;
    return parseCell(range.substr(0, colon), c, r) && parseCell(range.substr(colon + 1), c2, r2) && c2 >= c && r2 >= r;
  }

  static std::string cellName(int col, int row) {
    std::string letters;
    while (col > 0) { int rem = (col - 1) % 26; letters.insert(letters.begin(), static_cast<char>('A' + rem)); col = (col - 1) / 26; }
    return letters + std::to_string(row);
  }

  static std::string numberText(double value) {
    char buf[64];
    for (int precision = 15; precision <= 17; ++precision) {
      std::snprintf(buf, sizeof buf, "%.*g", precision, value);
      if (std::strtod(buf, nullptr) == value) break;
    }
    return buf;
  }

  void selectSheet(const std::string& sheet) {
    if (sheet.empty()) failWith("invalid_operation", "sheet is required");
    int parts = doc_->getParts();
    for (int i = 0; i < parts; ++i) {
      if (takeString(doc_->getPartName(i)) == sheet) { doc_->setPart(i); return; }
    }
    failWith("invalid_operation", "no sheet named " + sheet);
  }

  void enterCell(const std::string& cell, const std::string& input) {
    runUno(".uno:GoToCell", "{\"ToPoint\":{\"type\":\"string\",\"value\":" + quote(cell) + "}}");
    runUno(".uno:EnterString", "{\"StringName\":{\"type\":\"string\",\"value\":" + quote(input) + "}}");
  }

  // ─────── 校验(不改动文档)───────

  void requireKind(DocKind kind, const std::string& op) const {
    if (kind_ != kind) failWith("unsupported_operation", op + " is not available for this document type");
  }

  struct ValidationState {
    int parts;
    std::vector<std::string> names;
  };

  static bool hasName(const ValidationState& state, const std::string& name) {
    for (const std::string& existing : state.names) if (existing == name) return true;
    return false;
  }

  void validate(const Json& op, ValidationState& state) {
    std::string kind = op.str("kind");
    if (kind == "text.findReplace" || kind == "paragraph.style" || kind == "paragraph.insert") {
      requireKind(DocKind::Text, kind);
      std::string find = op.str(kind == "paragraph.insert" ? "anchor" : "find");
      if (find.empty() || find.find_first_of("\r\n") != std::string::npos) failWith("invalid_operation", kind + " needs non-empty single-line search text");
      if (kind == "text.findReplace" && op.str("replace").find_first_of("\r\n") != std::string::npos) failWith("invalid_operation", "replace must be single-line text");
      if (kind == "paragraph.insert" && op.str("position") != "before" && op.str("position") != "after") failWith("invalid_operation", "position must be before or after");
      if (kind == "paragraph.style") {
        std::string style = op.str("style");
        bool known = false;
        for (const std::string& name : paragraphStyles_) known = known || name == style;
        // ★ 未知样式名拒掉:StyleApply 对不存在的样式也回 success,文档却纹丝不动
        if (!known) failWith("invalid_operation", "unknown paragraph style \"" + style + "\"; available: " + styleSample());
      }
      return;
    }
    if (kind == "text.insert") {
      requireKind(DocKind::Text, kind);
      const Json* target = op.get("target");
      if (target == nullptr || target->str("ref") != "document") failWith("invalid_operation", "text.insert target.ref must be \"document\" in this engine version");
      std::string position = op.str("position");
      if (position != "start" && position != "end") failWith("invalid_operation", "text.insert position must be start or end in this engine version");
      return;
    }
    if (kind == "cells.set" || kind == "cells.formula") {
      requireKind(DocKind::Spreadsheet, kind);
      std::string sheet = op.str("sheet");
      if (!hasName(state, sheet)) failWith("invalid_operation", "no sheet named " + sheet);
      if (kind == "cells.formula") {
        int c = 0, r = 0;
        if (!parseCell(op.str("cell"), c, r)) failWith("invalid_operation", "cell must look like B2");
        return;
      }
      std::string range = op.str("range");
      if (!isRange(range)) failWith("invalid_operation", "range must look like A1 or A1:C10");
      const Json* values = op.get("values");
      if (values == nullptr || !values->isArray() || values->array.empty()) failWith("invalid_operation", "values must be a 2D array");
      int c1 = 0, r1 = 0, c2 = 0, r2 = 0;
      auto colon = range.find(':');
      parseCell(range.substr(0, colon), c1, r1);
      if (colon != std::string::npos) {
        parseCell(range.substr(colon + 1), c2, r2);
        size_t cols = 0;
        for (const Json& row : values->array) cols = row.isArray() && row.array.size() > cols ? row.array.size() : cols;
        if (values->array.size() > static_cast<size_t>(r2 - r1 + 1) || cols > static_cast<size_t>(c2 - c1 + 1)) {
          failWith("invalid_operation", "values do not fit inside range " + range);
        }
      }
      return;
    }
    if (kind == "sheet.insert") {
      requireKind(DocKind::Spreadsheet, kind);
      std::string name = op.str("name");
      if (name.empty() || name.size() > 31 || name.find_first_of("[]*?:/\\") != std::string::npos) failWith("invalid_operation", "invalid sheet name");
      if (hasName(state, name)) failWith("invalid_operation", "a sheet named " + name + " already exists");
      const Json* index = op.get("index");
      if (index != nullptr && (!index->isNumber() || index->number < 0 || index->number > state.parts)) {
        failWith("invalid_operation", "sheet index is out of range");
      }
      state.names.insert(state.names.begin() + (index != nullptr ? static_cast<int>(index->number) : state.parts), name);
      state.parts += 1;
      return;
    }
    if (kind == "slide.insert") {
      requireKind(DocKind::Presentation, kind);
      if (op.get("layout") != nullptr) failWith("unsupported_operation", "slide layouts are not supported in this engine version");
      const Json* index = op.get("index");
      if (index == nullptr || !index->isNumber() || index->number < 0 || index->number > state.parts) failWith("invalid_operation", "slide index is out of range");
      state.parts += 1;
      return;
    }
    if (kind == "slide.move") {
      requireKind(DocKind::Presentation, kind);
      const Json* from = op.get("from");
      const Json* to = op.get("to");
      int parts = state.parts;
      // ★ 越界的 moveSelectedParts 会让引擎直接崩(实测 "Unspecified Application Error"),必须先挡
      if (from == nullptr || to == nullptr || !from->isNumber() || !to->isNumber() || from->number < 0 || to->number < 0 ||
          from->number >= parts || to->number >= parts) {
        failWith("invalid_operation", "slide indexes are out of range");
      }
      return;
    }
    failWith("unsupported_operation", "operation " + kind + " is not supported by this engine version");
  }

  // ─────── 执行(已校验)───────

  // 返回这一条的结果对象(JSON)。按文字定位的操作带 matches,其余为 {}
  std::string execute(const Json& op) {
    std::string kind = op.str("kind");
    if (kind == "text.insert") {
      runUno(op.str("position") == "start" ? ".uno:GoToStartOfDoc" : ".uno:GoToEndOfDoc", "{}");
      insertLines(op.str("text"));
      return "{}";
    }
    if (kind == "text.findReplace") {
      std::string find = op.str("find");
      std::string replace = op.str("replace");
      bool matchCase = flag(op, "matchCase");
      int matches = checkedMatches(op, find, matchCase);
      runUno(".uno:ExecuteSearch", searchArgs(find, replace, kSearchReplaceAll, matchCase));
      doc_->resetSelection();
      /*
        ★ 读回核对:替换之后原文应当一处不剩。回执里的 success 对这条命令并不说明
        「全部替换了」。替换串本身包含查找串时(例如 a → ab)跳过,那种情况下剩余数不是 0。
      */
      if (!contains(replace, find, matchCase) && countMatches(find, matchCase) != 0) {
        failWith("io", "text was still found after replacing: " + find);
      }
      doc_->resetSelection();
      return "{\"matches\":" + std::to_string(matches) + "}";
    }
    if (kind == "paragraph.style") {
      std::string find = op.str("find");
      bool matchCase = flag(op, "matchCase");
      int matches = checkedMatches(op, find, matchCase);
      /*
        ★ 逐处定位,并在套样式前把光标收拢到段尾。不能对 FIND_ALL 的选区直接 StyleApply:
        实测(LibreOffice 26.8)当命中处在段首时,StyleApply 会把选中的文字删掉 ——
        「第一章 总则」套完标题变成「 总则」,且回执 success=true。
        同一段里有多处命中时,收拢到段尾会跳过同段其余命中,所以循环次数以命中数为上限,
        找不到下一处就停;查找回绕到文首时重复套同一段样式,结果不变。
      */
      runUno(".uno:GoToStartOfDoc", "{}");
      for (int i = 0; i < matches; ++i) {
        if (!findNext(find, matchCase)) break;
        runUno(".uno:GoToEndOfPara", "{}");
        runUno(".uno:StyleApply", "{\"Style\":{\"type\":\"string\",\"value\":" + quote(op.str("style")) +
                                     "},\"FamilyName\":{\"type\":\"string\",\"value\":\"ParagraphStyles\"}}");
      }
      return "{\"matches\":" + std::to_string(matches) + "}";
    }
    if (kind == "paragraph.insert") {
      std::string anchor = op.str("anchor");
      bool matchCase = flag(op, "matchCase");
      int matches = countMatches(anchor, matchCase);
      // ★ 锚点必须恰好一处:多处时「插在哪一处旁边」只能靠猜,猜错就是把条款插进别的章节
      if (matches != 1) failWith("invalid_operation", "anchor must match exactly one place; found " + std::to_string(matches) + ": " + anchor);
      runUno(".uno:GoToStartOfDoc", "{}");
      if (!findNext(anchor, matchCase)) failWith("invalid_operation", "anchor was counted but could not be located: " + anchor);
      if (op.str("position") == "after") {
        runUno(".uno:GoToEndOfPara", "{}");
        runUno(".uno:InsertPara", "{}");
        insertLines(op.str("text"));
      } else {
        runUno(".uno:GoToStartOfPara", "{}");
        insertLines(op.str("text"));
        runUno(".uno:InsertPara", "{}");
      }
      return "{\"matches\":1}";
    }
    if (kind == "cells.formula") {
      selectSheet(op.str("sheet"));
      std::string formula = op.str("formula");
      enterCell(op.str("cell"), formula.rfind('=', 0) == 0 ? formula : "=" + formula);
      return "{}";
    }
    if (kind == "cells.set") {
      selectSheet(op.str("sheet"));
      std::string range = op.str("range");
      int col = 0, row = 0;
      parseCell(range.substr(0, range.find(':')), col, row);
      const Json* values = op.get("values");
      for (size_t r = 0; r < values->array.size(); ++r) {
        const Json& line = values->array[r];
        for (size_t c = 0; c < line.array.size(); ++c) {
          const Json& value = line.array[c];
          std::string cell = cellName(col + static_cast<int>(c), row + static_cast<int>(r));
          if (value.isNull() || (value.isString() && value.string.empty())) {
            runUno(".uno:GoToCell", "{\"ToPoint\":{\"type\":\"string\",\"value\":" + quote(cell) + "}}");
            runUno(".uno:ClearContents", "{\"Flags\":{\"type\":\"unsigned short\",\"value\":\"895\"}}");
          } else if (value.isNumber()) {
            enterCell(cell, numberText(value.number));
          } else if (value.isBool()) {
            enterCell(cell, value.boolean ? "=TRUE()" : "=FALSE()");
          } else {
            // ★ 前缀撇号 = 按文本存。不加的话 "007" 变成数字 7、"=1+1" 变成公式 ——
            //   cells.set 承诺的是写入字面值,公式走 cells.formula。
            enterCell(cell, "'" + value.string);
          }
        }
      }
      return "{}";
    }
    if (kind == "sheet.insert") {
      int before = doc_->getParts();
      std::string name = op.str("name");
      const Json* index = op.get("index");
      int position = index != nullptr ? static_cast<int>(index->number) + 1 : before + 1;  // Insert 的 Index 从 1 起
      runUno(".uno:Insert", "{\"Name\":{\"type\":\"string\",\"value\":" + quote(name) + "},\"Index\":{\"type\":\"long\",\"value\":" + std::to_string(position) + "}}");
      waitParts(before + 1);
      if (takeString(doc_->getPartName(position - 1)) != name) failWith("io", "sheet was not inserted where expected");
      return "{}";
    }
    if (kind == "slide.insert") {
      int before = doc_->getParts();
      int index = static_cast<int>(op.get("index")->number);
      // InsertPage 插在当前页之后;插到最前面时先插在第 0 页后,再挪到第 0 位
      doc_->setPart(index == 0 ? 0 : index - 1);
      runUno(".uno:InsertPage", "{}");
      waitParts(before + 1);
      if (index == 0) moveSlide(1, 0);
      return "{}";
    }
    if (kind == "slide.move") {
      moveSlide(static_cast<int>(op.get("from")->number), static_cast<int>(op.get("to")->number));
      return "{}";
    }
    // validate 已拦下所有未知操作;走到这里说明两边的操作表不同步了
    failWith("unsupported_operation", "operation " + kind + " passed validation but has no executor");
  }

  void waitParts(int expected) {
    for (int i = 0; i < 100 && doc_->getParts() != expected; ++i) pause(20);
    if (doc_->getParts() != expected) failWith("io", "the engine did not report the expected number of parts");
  }

  void moveSlide(int from, int to) {
    if (from == to) return;
    int parts = doc_->getParts();
    std::vector<std::string> before;
    for (int i = 0; i < parts; ++i) before.push_back(takeString(doc_->getPartHash(i)));
    doc_->setPart(from);
    for (int i = 0; i < parts; ++i) doc_->selectPart(i, i == from ? 1 : 0);
    /*
      ★ 实测语义(LibreOffice 26.8,六组移动逐一核对过):moveSelectedParts(n) 把选中页
      插到**原顺序里下标 n 那一页之后**,n = -1 表示移到最前。它不是「最终下标」——
      按最终下标去传,往前挪的请求会原地不动,而回执照样返回。
      所以要落到最终下标 `to`:往前挪传 to-1,往后挪传 to(后面的页会因为移走一页而左移)。
    */
    doc_->moveSelectedParts(to < from ? to - 1 : to, false);
    // 用内容哈希读回核对最终顺序,不信回执
    std::vector<std::string> expected = before;
    std::string moved = expected[static_cast<size_t>(from)];
    expected.erase(expected.begin() + from);
    expected.insert(expected.begin() + to, moved);
    for (int attempt = 0; attempt < 50; ++attempt) {
      bool same = doc_->getParts() == parts;
      for (int i = 0; same && i < parts; ++i) same = takeString(doc_->getPartHash(i)) == expected[static_cast<size_t>(i)];
      if (same) return;
      pause(20);
    }
    failWith("io", "slide order after the move does not match the request");
  }
};

/**
 * 在 helper 私有的 LibreOffice profile 里预置配置,必须在 lok_init 之前写。
 *
 * 需求:Windows 上 LibreOfficeKit 只有线程模式(Windows 后端不支持 unipoll)。Calc 加载完
 * 导入后会建公式栏(ScInputWindow,子窗口 id FID_INPUTLINE_STATUS = 26100),里面的
 * 名称框 ComboBox 在 helper 线程里对 LibreOffice 主循环线程拥有的窗口调 SetWindowPos,
 * 而主循环线程在等 helper 线程持有的 SolarMutex —— 死锁,documentLoad 永不返回
 * (CI 上用 cdb 抓栈确认:线程 0 停在 NtUserSetWindowPos ← ComboBox::Resize ← sclo)。
 * helper 不需要公式栏(编辑走 UNO 命令),所以让它根本不被创建。
 *
 * ★ 所有平台都写:macOS / Linux 上公式栏同样没用,一条代码路径比按平台分叉好测。
 *   已存在的配置文件不覆盖(宿主每个 helper 用新的工作目录,正常情况下不会已存在)。
 */
void seedProfile(const std::string& profileDir) {
  namespace fs = std::filesystem;
  std::error_code error;
  fs::path user = fs::u8path(profileDir) / "user";
  fs::create_directories(user, error);
  fs::path config = user / "registrymodifications.xcu";
  if (fs::exists(config, error)) return;
  std::ofstream out(config, std::ios::binary);
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<oor:items xmlns:oor=\"http://openoffice.org/2001/registry\" xmlns:xs=\"http://www.w3.org/2001/XMLSchema\""
         " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
         "<item oor:path=\"/org.openoffice.Office.Views/Windows\"><node oor:name=\"26100\" oor:op=\"replace\">"
         "<prop oor:name=\"Visible\" oor:op=\"fuse\"><value>false</value></prop></node></item>\n"
         "</oor:items>\n";
}

/** `--name=value` 形式的可选参数;没有返回空串 */
std::string optionValue(int argc, char** argv, const std::string& name) {
  const std::string prefix = "--" + name + "=";
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
  }
  return "";
}

#ifdef __APPLE__
std::string xmlEscape(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else out += c;
  }
  return out;
}

/**
 * macOS:让 LibreOfficeKit 找得到系统字体。必须在 lok_init 之前调。
 *
 * 需求:LibreOfficeKit 在 macOS 上走无头渲染后端,字体经它自带的 fontconfig 查找,而那份
 * fontconfig 没有任何字体目录配置;LibreOffice 自带的字体里又没有 CJK。结果是中文一律画成
 * 方框(实测:标题「第一章 总则」整行是豆腐块),日文、韩文同理。这里写一份只列系统字体目录
 * 的 fonts.conf,并用 FONTCONFIG_FILE 指给 LibreOffice。
 *
 * - 用户自己装的字体(~/Library/Fonts)不在里面:helper 的 HOME 是私有目录,拿不到真实家目录。
 * - 缓存目录优先用宿主给的 `--cache-dir=`(各 helper 共用,只有第一次打开要扫描系统字体;
 *   本机实测 hello+打开 从 3.8s 降到 3.4s);没给就放在 profile 里,每次打开都要重扫一遍。
 * - 调用方已经设了 FONTCONFIG_FILE 时不覆盖(开发者自己排查字体问题时要能换配置)。
 */
void configureFonts(const std::string& profileDir, const std::string& cacheDir) {
  if (std::getenv("FONTCONFIG_FILE") != nullptr) return;
  namespace fs = std::filesystem;
  std::error_code error;
  fs::path dir = fs::u8path(profileDir);
  fs::create_directories(dir, error);
  fs::path config = dir / "fonts.conf";
  std::string cache = cacheDir.empty() ? (dir / "fontconfig-cache").u8string() : cacheDir;
  std::ofstream out(config, std::ios::binary | std::ios::trunc);
  out << "<?xml version=\"1.0\"?>\n<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n<fontconfig>\n"
         "  <dir>/System/Library/Fonts</dir>\n"
         "  <dir>/Library/Fonts</dir>\n"
         "  <cachedir>" << xmlEscape(cache) << "</cachedir>\n"
         "</fontconfig>\n";
  out.close();
  if (out) setenv("FONTCONFIG_FILE", config.u8string().c_str(), 1);
}
#endif


/** 处理一帧请求。返回 false = 收到 shutdown,调用方应当退出 */
bool handleFrame(Engine& engine, const std::string& frame) {
  Json request;
  try {
    request = ncw::parseJson(frame);
  } catch (const std::exception&) {
    return true;  // 没有 id 可回,丢弃
  }
  const Json* idValue = request.get("id");
  if (idValue == nullptr || !idValue->isNumber()) return true;
  double id = idValue->number;
  std::string method = request.str("method");
  std::fprintf(stderr, "[ncw-office-helper] request %s\n", method.c_str());
  std::fflush(stderr);
  const Json* params = request.get("params");
  static const Json kEmpty;
  const Json& p = params != nullptr ? *params : kEmpty;
  try {
    if (method == "document.open") {
      respondOk(id, engine.open(p.str("path"), p.str("format")));
    } else if (method == "document.apply") {
      const Json* operations = p.get("operations");
      std::string results = engine.apply(operations != nullptr ? *operations : kEmpty);
      respondOk(id, "{\"warnings\":[],\"undoable\":false,\"results\":" + results + "}");
    } else if (method == "document.query") {
      respondOk(id, engine.query(p));
    } else if (method == "document.render") {
      Engine::Rendered image = engine.render(p);
      respondOkWithAttachment(id, "{\"width\":" + std::to_string(image.width) + ",\"height\":" + std::to_string(image.height) +
                                      ",\"format\":\"rgba\",\"attachment\":true}", image.rgba);
    } else if (method == "document.saveAs") {
      engine.saveAs(p.str("path"), p.str("format"));
      respondOk(id, "{}");
    } else if (method == "shutdown") {
      respondOk(id, "{}");
      return false;
    } else {
      respondError(id, "invalid_operation", "unknown method " + method);
    }
  } catch (const HelperError& error) {
    respondError(id, error.code, error.message);
  } catch (const std::exception& error) {
    respondError(id, "io", error.what());
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  // 不变式 1:stdout 只给协议用
  g_protocolOut = ncw_dup(1);
  ncw_dup2(2, 1);
#ifdef _WIN32
  _setmode(0, _O_BINARY);
  _setmode(g_protocolOut, _O_BINARY);
  // CRT 的 fd 1 改了还不够:LibreOffice 在 Windows 上也会直接写 Win32 的标准输出句柄
  SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
#endif

  std::string home = std::getenv("HOME") != nullptr ? std::getenv("HOME") : ".";
  seedProfile(home + "/lo-profile");
#ifdef __APPLE__
  configureFonts(home + "/lo-profile", optionValue(argc, argv, "cache-dir"));
#endif
  std::string profile = fileUrl(home + "/lo-profile");
  std::string loPath = libreOfficePath(argc, argv);
#ifdef _WIN32
  /*
    ★ LoadLibraryW(program\sofficeapp.dll) 只按默认顺序找依赖的 DLL,不会去
    program 目录里找。不加这一行,表现是「failed to open library ... 找不到指定的模块」,
    而 sofficeapp.dll 明明就在那里。宿主给 helper 的环境是白名单,不能靠改 PATH 解决。
  */
  {
    int wide = MultiByteToWideChar(CP_UTF8, 0, loPath.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wide > 0 ? wide : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, loPath.c_str(), -1, &wpath[0], wide);
    SetDllDirectoryW(wpath.c_str());
  }
#endif
  lok::Office* office = lok::lok_cpp_init(loPath.c_str(), profile.c_str());
  if (office == nullptr) {
    std::fprintf(stderr, "ncw-office-helper: cannot initialise LibreOffice at %s\n", loPath.c_str());
    return 2;
  }
  /*
    办公级回调只打到 stderr:加载文档期间的交互请求(密码、过滤器选项、提示框)走这里,
    文档级回调那时还没注册。加载卡住时,宿主截到的 stderr 尾部能看出引擎在等什么。
  */
  office->registerCallback([](int type, const char* payload, void*) {
    std::fprintf(stderr, "[ncw-office-helper] office callback %d %.200s\n", type, payload != nullptr ? payload : "");
    std::fflush(stderr);
  }, nullptr);
  std::string version = "LibreOffice";
  if (char* info = office->getVersionInfo()) {
    try {
      Json parsed = ncw::parseJson(info);
      version = parsed.str("ProductName", "LibreOffice") + " " + parsed.str("ProductVersion") + parsed.str("ProductExtension");
    } catch (const std::exception&) {}
    office->freeError(info);
  }
  sendJson("{\"v\":1,\"event\":\"hello\",\"data\":{\"protocol\":" + std::to_string(kProtocolVersion) + ",\"engineVersion\":" + quote(version) + "}}");

  Engine engine(office);
#ifdef _WIN32
  /*
    Windows:请求交给 LibreOffice 主线程处理(见 namespace win 的说明),本线程只负责读 stdin。
    找不到主线程窗口时退回下面的旧路径并在 stderr 里留一句 —— 那样 Calc / Impress / Draw 会卡住。
  */
  static Engine* s_engine = &engine;
  if (win::attach([](const std::string& frame) {
        if (!handleFrame(*s_engine, frame)) {
          std::fflush(stderr);
          std::_Exit(0);
        }
      })) {
    Engine::stage("requests are handled on the LibreOffice main thread");
    std::string frame;
    while (readFrame(frame)) win::post(frame);
    // 宿主关了 stdin:给已排队的请求一点时间写完回执,然后退出
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    std::fflush(stderr);
    std::_Exit(0);
  }
  Engine::stage("LibreOffice main-thread window not found; handling requests on the helper thread");
#endif
  std::string frame;
  while (readFrame(frame)) {
    if (!handleFrame(engine, frame)) break;
  }
  /*
    ★ _exit 而不是 return:LibreOffice 的静态析构在进程退出时会去碰已经拆掉的系统剪贴板
    (实测崩溃栈停在 SwDLL 析构里)。文档都由宿主保存过了,这里没有要落盘的东西。
  */
  std::fflush(stderr);
  std::_Exit(0);
}
