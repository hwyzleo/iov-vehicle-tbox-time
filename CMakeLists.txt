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
include(CMakePackageConfigHelpers)

# ===========================================================================
# 依赖
# ===========================================================================
if(NOT TARGET TBoxFramework::ipc)
    find_package(TboxFramework CONFIG REQUIRED)
endif()
find_package(yaml-cpp CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest QUIET)

# system-observe（TBOX-TIME-DSN-CR-002）：libsystemd（sd-bus）仅 Linux 观测 I/O 需要。
#
# 两级探测，缺一不可：
#   1. pkg-config —— HOST 原生构建（开发机装了 libsystemd-dev）走这条；
#   2. find_path/find_library —— 交叉构建必须走这条。BUILD 的交叉环境把
#      PKG_CONFIG_SYSROOT_DIR 指向 TARGET 依赖 staging（供 vsomeip/protobuf 的
#      .pc 重根），sysroot 自身的 pkgconfig 目录不在 pkg-config 搜索路径内，
#      因此 pkg_check_modules 对 sysroot 库恒失败；而 find_* 走
#      CMAKE_FIND_ROOT_PATH（= sysroot，MODE_INCLUDE/LIBRARY=ONLY）可正确解析。
#      只有 pkg-config 一级时，交叉构建会静默编译掉观测路径并产出一个
#      恒 Unsupported 的 daemon —— 构建全绿但功能不存在。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBSYSTEMD QUIET libsystemd)
endif()

find_path(LIBSYSTEMD_INCLUDE_DIR systemd/sd-bus.h HINTS ${LIBSYSTEMD_INCLUDE_DIRS})
find_library(LIBSYSTEMD_LIBRARY NAMES systemd HINTS ${LIBSYSTEMD_LIBRARY_DIRS})

# 用普通库名 systemd 链接，不用 IMPORTED 目标：tbox-time-lib 是静态库，
# PRIVATE 链接项会以 $<LINK_ONLY:...> 形式写进导出的
# INTERFACE_LINK_LIBRARIES。若此处写 IMPORTED 目标名，消费方（SEC）工程里
# 不存在同名目标，CMake 会把它当普通库名退化成 -l<目标名> 并链接失败
# （实测：/usr/bin/ld: cannot find -ltbox_time_sdbus）。普通名 systemd 经
# --sysroot 可在消费方正常解析，且导出内容可重定位。
if(LIBSYSTEMD_INCLUDE_DIR AND LIBSYSTEMD_LIBRARY)
    set(TBOX_TIME_SDBUS_AVAILABLE TRUE)
else()
    set(TBOX_TIME_SDBUS_AVAILABLE FALSE)
endif()

# 发布构建应显式要求观测能力可用：缺 sd-bus 时宁可 configure 失败，
# 也不要产出一个"构建成功但 system-observe 恒 Unsupported"的 daemon。
# BUILD 经 manifests/services.yaml 的 cmake_cache_variables 打开本开关。
option(TBOX_TIME_REQUIRE_SDBUS
    "Fail configuration when libsystemd/sd-bus is unavailable (release builds \
of the system-observe backend)" OFF)

if(TBOX_TIME_SDBUS_AVAILABLE)
    message(STATUS "TIME: sd-bus observation ENABLED "
                   "(header=${LIBSYSTEMD_INCLUDE_DIR}, lib=${LIBSYSTEMD_LIBRARY})")
elseif(TBOX_TIME_REQUIRE_SDBUS)
    message(FATAL_ERROR
        "TBOX_TIME_REQUIRE_SDBUS=ON but libsystemd/sd-bus was not found. "
        "The system-observe backend reads systemd-timesyncd NTP evidence over "
        "D-Bus; without it SystemObserverSource can only report Unsupported. "
        "Provide libsystemd dev files (systemd/sd-bus.h + libsystemd.so) in the "
        "sysroot, or build with TBOX_TIME_REQUIRE_SDBUS=OFF to accept a "
        "non-observing daemon.")
else()
    message(WARNING
        "TIME: libsystemd/sd-bus NOT found -> system-observe D-Bus evidence "
        "path is compiled OUT; SystemObserverSource will report Unsupported "
        "and TIME will distribute no time facts on this platform.")
endif()

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
    src/backends/system_observer_backend.cpp
    src/ipc/time_json.cpp
    src/ipc/time_ipc_server.cpp
    src/ipc/time_client.cpp
    src/config/time_config.cpp
)
add_library(tbox-time-lib STATIC ${TIME_LIB_SOURCES})
# 导出目标名冻结为 tbox::time_client（与 tbox::sec_client / tbox::prov_client 一致），
# 本地 target 名 tbox-time-lib 不随 SDK 导出（TBOX-SEC-DSN-CR-024 依赖装配登记）。
set_target_properties(tbox-time-lib PROPERTIES EXPORT_NAME time_client)
target_include_directories(tbox-time-lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(tbox-time-lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tbox-time-lib PUBLIC
    TBoxFramework::ipc
    TBoxFramework::application
    nlohmann_json::nlohmann_json
)
target_link_libraries(tbox-time-lib PRIVATE yaml-cpp::yaml-cpp)
if(TBOX_TIME_SDBUS_AVAILABLE)
    target_compile_definitions(tbox-time-lib PRIVATE HAVE_LIBSYSTEMD=1)
    # PRIVATE 头路径不进 INTERFACE_INCLUDE_DIRECTORIES，消费方无需 sd-bus 头。
    target_include_directories(tbox-time-lib PRIVATE ${LIBSYSTEMD_INCLUDE_DIR})
    target_link_libraries(tbox-time-lib PRIVATE systemd)
endif()

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
# Install rules - 正式 SDK 与 daemon runtime（TBOX-SEC-DSN-CR-024 依赖装配）
#
#   time-sdk     -> staging: sdk      (公开头文件 / client 静态库 / CMake package)
#   time-runtime -> staging: rootfs   (daemon 二进制 / 默认配置)
#
# 消费方（SEC 等）经 find_package(TboxTimeClient CONFIG) 消费安装目录，
# 禁止 add_subdirectory(TIME)、私有头或 build-tree 库。
# 命名冻结登记：package=TboxTimeClient，target=tbox::time_client，头=tbox/time/*。
# ===========================================================================

# --- time-sdk: 公开头文件（仅 tbox/time 公共契约，不含 src/ 私有头） ---
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/tbox/time/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tbox/time
    COMPONENT time-sdk
    FILES_MATCHING PATTERN "*.h"
)

# --- time-sdk: client 静态库 + export ---
install(TARGETS tbox-time-lib
    EXPORT TboxTimeClientTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT time-sdk
)

install(EXPORT TboxTimeClientTargets
    FILE TboxTimeClientTargets.cmake
    NAMESPACE tbox::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TboxTimeClient
    COMPONENT time-sdk
)

# --- time-sdk: CMake package config (relocatable) ---
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TboxTimeClientConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/TboxTimeClientConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TboxTimeClient
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/TboxTimeClientConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/TboxTimeClientConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/TboxTimeClientConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TboxTimeClient
    COMPONENT time-sdk
)

# --- time-runtime: daemon 二进制（供 SEC/MQTT 真实进程联调） ---
install(TARGETS tbox_time
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT time-runtime
)

# --- time-runtime: systemd unit ---
# BUILD 侧 manifests/services.yaml 声明 runtime.systemd_units:
# [tbox-time.service]，并有 systemd-units-declared 打包门禁强制本文件实际安装
# （TBOX-BUILD-DSN-CR-006 §3.3：仅登记名称不算交付）。
install(FILES packaging/systemd/tbox-time.service
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
    COMPONENT time-runtime
)

# --- time-runtime: 默认配置模板（production 形态） ---
# 落到 conf.d/time.yaml：与 prov/sec/mqtt 同一部署策略
# （BUILD config-deployment: /etc/tbox/conf.d/*.yaml → replace），
# 平台专属值由 BUILD 的 configs/<platform>/rootfs overlay 整文件覆盖。
# 用 time.example.yaml 而非 config/time.yaml —— 后者是 test/mock 冒烟配置，
# 不得作为正式默认发布。
install(FILES config/time.example.yaml
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/tbox/conf.d
    RENAME time.yaml
    COMPONENT time-runtime
)

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
        tests/test_observer.cpp
        tests/test_observe_state_machine.cpp
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

    # installed-consumer 测试（TBOX-SEC-DSN-CR-024 依赖装配验收）：
    # 仅使用安装后的 SDK（find_package(TboxTimeClient CONFIG)）编译、链接、运行，
    # 证明 install/export 依赖闭包完整；不 add_subdirectory、不引用私有头/build-tree 库。
    add_test(NAME time_installed_consumer
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/integration/installed_consumer/run_test.sh
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/integration/installed_consumer
            ${CMAKE_CURRENT_BINARY_DIR}/installed_consumer_scratch
    )
endif()
