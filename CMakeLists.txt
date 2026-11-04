# TBox TIME - TBOX 统一时间源管理与系统校时服务
#
# TBOX-TIME-DSN-CR-001：单实例 TIME 服务 + 只读 client SDK。
#   - 复用 framework SDK（ipc/config/log/application），禁止 add_subdirectory
#   - 依赖：yaml-cpp（配置）、nlohmann_json（IPC 载荷）、GTest（HOST 单元测试）
#   - 本 CMake 仅负责 HOST 开发构建；BUILD 统一 Orin 交叉编译入口待接入
#     （登记 docs/implementation_notes.md）
#   - 后端二选一：自研 Linux 系统时钟后端（本次实现）/ 复用 chrony（扩展点）

cmake_minimum_required(VERSION 3.16)
project(tbox_time VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
if(NOT CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# ===========================================================================
# 依赖
# ===========================================================================
if(NOT TARGET TBoxFramework::ipc)
    find_package(TboxFramework CONFIG REQUIRED)
endif()
find_package(yaml-cpp CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest QUIET)

# ===========================================================================
# 内部库（daemon 核心 + client 实现；安装/SDK 契约待 BUILD 统一登记）
# ===========================================================================
set(TIME_LIB_SOURCES
    src/core/time_core.cpp
    src/core/types.cpp
    src/core/source_sample.cpp
    src/core/time_snapshot.cpp
    src/sources/ntp_source.cpp
    src/sources/source_manager.cpp
    src/backends/mock_backend.cpp
    src/backends/linux_system_clock_backend.cpp
    src/ipc/time_json.cpp
    src/ipc/time_ipc_server.cpp
    src/ipc/time_client.cpp
    src/config/time_config.cpp
)
add_library(tbox-time-lib STATIC ${TIME_LIB_SOURCES})
target_include_directories(tbox-time-lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(tbox-time-lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tbox-time-lib PUBLIC
    TBoxFramework::ipc
    TBoxFramework::application
    nlohmann_json::nlohmann_json
)
target_link_libraries(tbox-time-lib PRIVATE yaml-cpp::yaml-cpp)

# ===========================================================================
# Daemon
# ===========================================================================
add_executable(tbox_time
    src/main.cpp
    src/app/time_application.cpp
)
target_include_directories(tbox_time PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tbox_time PRIVATE tbox-time-lib TBoxFramework::log)

# ===========================================================================
# 单元/集成测试（HOST；GTest 位于 ~/.local）
# ===========================================================================
if(GTest_FOUND)
    enable_testing()
    set(TEST_SOURCES
        tests/test_time_state_machine.cpp
        tests/test_source_selection.cpp
        tests/test_holdover.cpp
        tests/test_snapshot.cpp
        tests/test_backend.cpp
        tests/test_ntp_client.cpp
        tests/test_config.cpp
        tests/test_time_client.cpp
        integration/test_daemon_e2e.cpp
    )
    foreach(TEST_SOURCE ${TEST_SOURCES})
        get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
        add_executable(${TEST_NAME} ${TEST_SOURCE})
        target_link_libraries(${TEST_NAME} PRIVATE
            tbox-time-lib GTest::GTest GTest::gmock GTest::Main)
        target_include_directories(${TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()
endif()
