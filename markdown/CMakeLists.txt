# Build markdown.wasm —— md4c（CommonMark + GFM）standalone wasm 包装。
#
# md4c 源经 build.zig.zon 声明 + zig fetch 拉取，路径走 MD4C_ROOT env 注入
# （由 build.zig 的 addCmakePlugin 设置；同 barcode 的 ZXING_CPP_ROOT）。
# 纯计算、无 throw、无 I/O：
#   - 不要 -fwasm-exceptions（不像 barcode/zxing）→ 产物无 EH 指令，任意 wasm
#     运行模式都能跑（不锁 classic 解释器）。
#   - 不要 FILESYSTEM / JS glue：STANDALONE_WASM 单文件，alloc/free/dispatch
#     由 aglet_plugin_sdk 的 __attribute__((export_name(...))) 显式导出。
#
# 用法（仓库根，自动 fetch md4c + 注入 MD4C_ROOT）：
#   zig build markdown

cmake_minimum_required(VERSION 3.16)
project(AgletMarkdownPlugin C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED ENV{MD4C_ROOT})
    message(FATAL_ERROR "MD4C_ROOT env must point at md4c source root (use `zig build markdown`)")
endif()
set(MD4C_DIR $ENV{MD4C_ROOT}/src)

# Aglet plugin SDK (header-only) —— sits at ../sdk/c/ in the repo.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../sdk/c
                 ${CMAKE_BINARY_DIR}/aglet_plugin_sdk_c)

add_executable(markdown
    src/wrapper.cpp
    ${MD4C_DIR}/md4c.c
    ${MD4C_DIR}/md4c-html.c
    ${MD4C_DIR}/entity.c
)
target_include_directories(markdown PRIVATE ${MD4C_DIR})
target_link_libraries(markdown aglet_plugin_sdk_c)
set_target_properties(markdown PROPERTIES SUFFIX ".wasm")

# Standalone wasm：
#   -s STANDALONE_WASM=1     不出 JS glue，纯 .wasm
#   -s FILESYSTEM=0          不要 emscripten fs shim
#   -s ALLOW_MEMORY_GROWTH=1 大文档动态扩内存（host 端 emscripten_notify_memory_growth stub）
#   -s INITIAL_MEMORY=2MB    markdown 轻量，2MB 起步
#   --no-entry               不需要 main()
set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} \
     -s STANDALONE_WASM=1 \
     -s FILESYSTEM=0 \
     -s ALLOW_MEMORY_GROWTH=1 \
     -s INITIAL_MEMORY=2MB \
     -Wl,--no-entry")
