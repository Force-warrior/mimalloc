# mac compile
cmake -S ./ -B build-mimalloc -DCMAKE_BUILD_TYPE=Release -DMI_BUILD_SHARED=ON -DMI_BUILD_STATIC=OFF -DMI_OVERRIDE=ON -DMI_OSX_INTERPOSE=ON -DMI_USE_CXX=ON
cmake --build build-mimalloc

# win compile
cmake -S ./ -B build-mimalloc -G "Visual Studio 17 2022" -A x64 -DMI_BUILD_STATIC=OFF -DMI_BUILD_SHARED=ON -DMI_OVERRIDE=ON -DMI_BUILD_TESTS=OFF -DMI_EXTRA_CPPDEFS="MI_USE_RTLGENRANDOM=1"
cmake --build build-mimalloc --config Release

# change compile with tag v3.3.1
- CMakeLists.txt
增加 MI_ENABLE_CXX 与 混合 C/C++ 构建逻辑。
把 src/alloc-trace.cpp 加入 mi_sources；在存在该文件或打开 MI_ENABLE_CXX 时 enable_language(CXX)。
在 MI_USE_CXX 时增加 MI_MIMALLOC_C_SOURCES_AS_CXX=1 等定义，以配合与预加载/链接名相关的 C++ 编译。

- include\mimalloc\internal.h
声明 extern "C" 包装下的 _mi_trace_on_malloc / _mi_trace_on_free。

- src\alloc.c（mi_malloc 热路径）
在返回指针前调用 _mi_trace_on_malloc(size, addr)，用于与 trace 层对接。

- src\free.c（mi_free_ex 快速路径内）
在合适条件下调用 _mi_trace_on_free(p)。

- src\init.c 末尾
提供 弱符号/默认空实现（非 MSVC 的 weak；注释里说明 MSVC 侧通过链接 alloc-trace.cpp 的强符号覆盖等思路）。