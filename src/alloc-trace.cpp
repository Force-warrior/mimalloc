//
// Created by 张光明 on 2026/4/22.
//
// Hot path (TraceStack/TraceFree): only backtrace + POD record + map update.
// dladdr / __cxa_demangle / I/O only in FinishFlush (tracing disabled).
//
#include "mimalloc/alloc-trace.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <execinfo.h>
#include <cxxabi.h>

#if defined(MI_MIMALLOC_C_SOURCES_AS_CXX)
bool _mi_preloading(void);
#else
extern "C" bool _mi_preloading(void);
#endif

static std::atomic<bool> g_mi_trace_alloc_enabled{false};

static thread_local bool g_tls_in_alloc_trace{false};

struct mi_trace_recursion_guard {
  mi_trace_recursion_guard() { g_tls_in_alloc_trace = true; }
  ~mi_trace_recursion_guard() { g_tls_in_alloc_trace = false; }
};

extern "C" {
void _mi_trace_on_malloc(size_t size, void* addr) {
  if (_mi_preloading()) {
    return;
  }
  if (!g_mi_trace_alloc_enabled.load(std::memory_order_acquire)) {
    return;
  }
  if (g_tls_in_alloc_trace) {
    return;
  }
  mi_trace_recursion_guard guard;
  TraceAllocStack::GetInstance()->TraceStack(size, addr);
}

void _mi_trace_on_free(void* addr) {
  if (addr == NULL) {
    return;
  }
  if (_mi_preloading()) {
    return;
  }
  if (!g_mi_trace_alloc_enabled.load(std::memory_order_acquire)) {
    return;
  }
  if (g_tls_in_alloc_trace) {
    return;
  }
  mi_trace_recursion_guard guard;
  TraceAllocStack::GetInstance()->TraceFree(addr);
}
}  // extern "C"

static void line_append(char* line, size_t line_size, const char* piece) {
  if (!piece || line_size < 1) {
    return;
  }
  if (line[0] == '\0') {
    (void)snprintf(line, line_size, "%s", piece);
  } else {
    const size_t u = strnlen(line, line_size);
    if (u < line_size) {
      (void)snprintf(line + u, line_size - u, ";%s", piece);
    }
  }
}

static void demangle_to_buffer(const char* name, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  if (!name || name[0] == '\0') {
    (void)snprintf(out, out_size, "?");
    return;
  }
  if (name[0] != '_') {
    (void)snprintf(out, out_size, "%s", name);
    return;
  }
  int status = 0;
  char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
  if (status == 0 && demangled) {
    (void)snprintf(out, out_size, "%s", demangled);
    free(demangled);
    return;
  }
  if (demangled) {
    free(demangled);
  }
  (void)snprintf(out, out_size, "%s", name);
}

static bool should_skip_frame_name(const char* name) {
  if (!name) {
    return false;
  }
  return (strstr(name, "_mi_trace_on") != NULL || strstr(name, "TraceStack") != NULL ||
          strstr(name, "TraceFree") != NULL || strstr(name, "backtrace") != NULL);
}

void TraceAllocStack::format_record_to_line(const Record& rec, char* line, size_t line_size) {
  line[0] = '\0';
  if (line_size < 2 || rec.nframes <= 0) {
    return;
  }
  char show[256];
  char hex[32];
  for (int i = rec.nframes - 1; i >= 0; i--) {
    const void* frame = rec.frames[i];
    Dl_info info;
    if (dladdr(frame, &info) && info.dli_sname != NULL && info.dli_sname[0] != '\0') {
      if (should_skip_frame_name(info.dli_sname)) {
        continue;
      }
      demangle_to_buffer(info.dli_sname, show, sizeof(show));
      line_append(line, line_size, show);
    } else {
      (void)snprintf(hex, sizeof(hex), "%p", frame);
      line_append(line, line_size, hex);
    }
  }
}

TraceAllocStack* TraceAllocStack::GetInstance() {
  static TraceAllocStack obj;
  return &obj;
}

TraceAllocStack::~TraceAllocStack() {
  UnInit();
}

void TraceAllocStack::Init(const char* cache_path) {
  bool expect = false;
  if (!running_.compare_exchange_strong(expect, true)) {
    return;
  }

  cache_path_ = cache_path;
  g_mi_trace_alloc_enabled.store(true, std::memory_order_release);
}

size_t TraceAllocStack::GetNoFreeRecordSize(){
  return trace_data_.size();
}

int TraceAllocStack::UnInit() {
  g_mi_trace_alloc_enabled.store(false, std::memory_order_release);
  bool expect = true;
  if (!running_.compare_exchange_strong(expect, false)) {
    return -1;
  }
  return FinishFlush();
}

void TraceAllocStack::TraceStack(size_t alloc_bytes, void* addr) {
  if(!running_.load(std::memory_order_acquire)){
    return;
  }

  void* buf[kMaxStackFrames_];
  const int n = backtrace(buf, kMaxStackFrames_);
  Record rec;
  rec.alloc_size = alloc_bytes;
  rec.nframes = 0;
  if (n > kBacktraceSkip_) {
    const int avail = n - kBacktraceSkip_;
    for (int i = 0; i < avail && i < kMaxStackFrames_; i++) {
      rec.frames[i] = buf[kBacktraceSkip_ + i];
      rec.nframes++;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    trace_data_[addr] = rec;
  }
}

void TraceAllocStack::TraceFree(void* addr) {
  if(!running_.load(std::memory_order_acquire)){
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  (void)trace_data_.erase(addr);
}

int TraceAllocStack::FinishFlush() {
  StackData data;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    data.swap(trace_data_);
  }

  if (data.empty()) {
    return -2;
  }

  auto now = std::chrono::system_clock::now();
  const auto ts =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  const std::string timestamp_str = std::to_string(ts);

  const std::filesystem::path traceStackDir = cache_path_;

  if (!std::filesystem::exists(traceStackDir)) {
    (void)std::filesystem::create_directory(traceStackDir);
  }
  const std::filesystem::path filePath = traceStackDir / (timestamp_str + ".txt");

  std::ofstream outputFile(filePath, std::ios::out | std::ios::app);
  if (!outputFile) {
    return -4;
  }
  char line[1024];
  for (const auto& e : data) {
    const void* addr = e.first;
    const Record& rec = e.second;
    format_record_to_line(rec, line, sizeof(line));
    outputFile << line << "  " << rec.alloc_size << std::endl;
  }
  outputFile.close();
  return 0;
}
