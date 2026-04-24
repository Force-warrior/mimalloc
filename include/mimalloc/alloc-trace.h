//
// Created by 张光明 on 2026/4/22.
//
// For MI_SHARED_LIB: TraceAllocStack must be exported like mi_malloc, or
// test executables (and DYLD clients) get undefined symbol at link time
// (default -fvisibility=hidden strips non-exported C++ members).
//
// Do not default-construct std::ofstream: mi_malloc can run during dyld
// initializers; a file stream constructor allocates and can re-enter / abort.
// Open the file only in Init() via std::optional<std::ofstream>::emplace.
//
#pragma once

#include "mimalloc.h"  // mi_decl_export
#include <atomic>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

// Hot path: raw PCs only; symbol resolution runs off hot path (FinishFlush).
class mi_decl_export TraceAllocStack {
 public:
  mi_decl_export static TraceAllocStack* GetInstance();
  mi_decl_export ~TraceAllocStack();
  bool isInit() { return running_.load(std::memory_order_acquire); }
  mi_decl_export void Init(const char* cache_path);
  mi_decl_export int UnInit();
  mi_decl_export size_t GetNoFreeRecordSize();
  mi_decl_export void TraceStack(size_t alloc_bytes, void* addr);
  mi_decl_export void TraceFree(void* addr);

 private:
  static constexpr size_t kBacktraceIfAllocLargerThanBytes_ = 1024u * 1024u;

  static constexpr int kMaxStackFrames_ = 32;
  static constexpr int kBacktraceSkip_   = 2;  // skip this file + backtrace

  struct Record {
    int nframes;
    void* frames[kMaxStackFrames_];
    size_t alloc_size;
  };
  using StackData = std::unordered_map<void*, Record>;
  TraceAllocStack() = default;

  static void format_record_to_line(const Record& rec, char* line, size_t line_size);

  int FinishFlush();

  std::atomic<bool> running_{false};
  std::mutex mutex_;
  StackData trace_data_;
  std::string cache_path_;
  std::shared_ptr<std::ofstream> outputFile_;
};
