cmake_minimum_required(VERSION 3.16)
project(infra_test)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── 编译优化 ──────────────────────────────────────────────────
if (UNIX)
    # find_package(PkgConfig REQUIRED)

    # 通用警告
    add_compile_options(-Wall -Wunused-result)

    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
        add_compile_options(
            -O3
            -march=native
            -mtune=native
            -flto=auto
            -fno-omit-frame-pointer
            -funroll-loops
            -ffast-math
            -fprefetch-loop-arrays
        )
        # LTO 需要同步传给链接器
        add_link_options(-flto=auto)
    elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        add_compile_options(-O0 -g -rdynamic)
    else()
        # RelWithDebInfo 或未指定：保留调试信息 + 基础优化
        add_compile_options(-O2 -g -fno-omit-frame-pointer)
    endif()

    # CPU 亲和 / 高精度时钟需要 _GNU_SOURCE
    add_definitions(-D_GNU_SOURCE)
endif()

# infra
# set(USE_BINANCE ON)
# set(USE_BITGET ON)
add_subdirectory(exchangelink-hft-sdk)

add_executable(infra_test "test/main.cpp")
target_include_directories(
  infra_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/exchangelink-hft-sdk)
target_link_libraries(infra_test PRIVATE infra)
