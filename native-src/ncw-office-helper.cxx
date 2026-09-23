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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
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

void sendJson(const std::string& json) {
  std::lock_guard<std::mutex> guard(g_writeMutex);
  uint32_t length = static_cast<uint32_t>(json.size());
  unsigned char header[5] = {
    static_cast<unsigned char>(length >> 24), static_cast<unsigned char>(length >> 16),
    static_cast<unsigned char>(length >> 8), static_cast<unsigned char>(length), 0
  };
  writeAll(g_protocolOut, reinterpret_cast<const char*>(header), 5);
  writeAll(g_protocolOut, json.data(), json.size());
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
    stage("open: loading " + format);
    lok::Document* doc = office_->documentLoad(fileUrl(path).c_str(), "Language=en-US");
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
    for (int i = 0; i < 100 && events_.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
      if (cv_.wait_until(lock, deadline) == std::cv_status::timeout && results_.empty()) {
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
    cv_.wait_for(lock, std::chrono::milliseconds(1000), [this] { return !searches_.empty(); });
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
    cv_.wait_for(lock, std::chrono::milliseconds(1000), [this] { return !searches_.empty(); });
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
    for (int i = 0; i < 100 && doc_->getParts() != expected; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    failWith("io", "slide order after the move does not match the request");
  }
};

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
  std::string frame;
  while (readFrame(frame)) {
    Json request;
    try {
      request = ncw::parseJson(frame);
    } catch (const std::exception&) {
      continue;  // 没有 id 可回,丢弃
    }
    const Json* idValue = request.get("id");
    if (idValue == nullptr || !idValue->isNumber()) continue;
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
      } else if (method == "document.saveAs") {
        engine.saveAs(p.str("path"), p.str("format"));
        respondOk(id, "{}");
      } else if (method == "shutdown") {
        respondOk(id, "{}");
        break;
      } else {
        respondError(id, "invalid_operation", "unknown method " + method);
      }
    } catch (const HelperError& error) {
      respondError(id, error.code, error.message);
    } catch (const std::exception& error) {
      respondError(id, "io", error.what());
    }
  }
  /*
    ★ _exit 而不是 return:LibreOffice 的静态析构在进程退出时会去碰已经拆掉的系统剪贴板
    (实测崩溃栈停在 SwDLL 析构里)。文档都由宿主保存过了,这里没有要落盘的东西。
  */
  std::fflush(stderr);
  std::_Exit(0);
}
