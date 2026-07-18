# CMakeLists.txt
cmake_minimum_required(VERSION 3.19)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" VER_RAW)
string(STRIP ${VER_RAW} AQUAMARINE_VERSION)

add_compile_definitions(AQUAMARINE_VERSION="${AQUAMARINE_VERSION}")

project(
  aquamarine
  VERSION ${AQUAMARINE_VERSION}
  DESCRIPTION "A very light linux rendering backend library")

include(CTest)
include(CheckIncludeFile)
include(GNUInstallDirs)

set(PREFIX ${CMAKE_INSTALL_PREFIX})
set(INCLUDE ${CMAKE_INSTALL_FULL_INCLUDEDIR})
set(LIBDIR ${CMAKE_INSTALL_FULL_LIBDIR})

find_package(PkgConfig REQUIRED)

# ============================================================
# 查找 OpenGL ES 和 EGL（Anland 需要）
# ============================================================
find_package(OpenGL REQUIRED COMPONENTS GLES2 EGL)

find_package(hyprwayland-scanner 0.4.0 REQUIRED)

# ============================================================
# 设置 PKG_CONFIG_PATH 备用路径（针对交叉编译）
# ============================================================
if(NOT DEFINED ENV{PKG_CONFIG_PATH})
    set(ENV{PKG_CONFIG_PATH} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/local/lib/pkgconfig")
endif()

pkg_check_modules(
  deps
  REQUIRED
  IMPORTED_TARGET
  libseat>=0.8.0
  libinput>=1.26.0
  wayland-client
  wayland-protocols
  hyprutils>=0.8.0
  pixman-1
  libdrm
  gbm
  libudev
  libdisplay-info
  hwdata)

# ============================================================
# 手动查找关键库（以防 pkg-config 路径问题）
# ============================================================
find_library(HYPRUTILS_LIB
    NAMES hyprutils
    PATHS
        /usr/lib/aarch64-linux-gnu
        /usr/lib
        /usr/local/lib
        /usr/lib64
    )
if(HYPRUTILS_LIB)
    message(STATUS "Found hyprutils library at: ${HYPRUTILS_LIB}")
else()
    message(WARNING "hyprutils library not found, linking may fail")
endif()

find_library(GBM_LIB
    NAMES gbm
    PATHS
        /usr/lib/aarch64-linux-gnu
        /usr/lib
        /usr/local/lib
    )
if(GBM_LIB)
    message(STATUS "Found gbm library at: ${GBM_LIB}")
else()
    message(WARNING "gbm library not found, linking may fail")
endif()

find_library(DRM_LIB
    NAMES drm
    PATHS
        /usr/lib/aarch64-linux-gnu
        /usr/lib
        /usr/local/lib
    )
if(DRM_LIB)
    message(STATUS "Found drm library at: ${DRM_LIB}")
endif()

configure_file(aquamarine.pc.in aquamarine.pc @ONLY)

# ============================================================
# C++ 标准配置
# ============================================================
set(CMAKE_CXX_STANDARD 23)
add_compile_options(
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-unused-value
  -Wno-missing-field-initializers
  -Wpedantic)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

if(CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES DEBUG)
  message(STATUS "Configuring aquamarine in Debug")
  add_compile_definitions(AQUAMARINE_DEBUG)
else()
  add_compile_options(-O3)
  message(STATUS "Configuring aquamarine in Release")
endif()

# ============================================================
# 收集源文件（排除 anland，由子目录单独处理）
# ============================================================
file(GLOB_RECURSE SRCFILES CONFIGURE_DEPENDS 
    "src/*.cpp" 
    "include/*.hpp"
)
list(FILTER SRCFILES EXCLUDE REGEX "src/backend/anland/.*")

file(GLOB_RECURSE PUBLIC_HEADERS CONFIGURE_DEPENDS "include/*.hpp")

add_library(aquamarine SHARED ${SRCFILES})
target_include_directories(
  aquamarine
  PUBLIC "./include"
  PRIVATE "./src" "./src/include" "./protocols" "${CMAKE_BINARY_DIR}")
set_target_properties(aquamarine PROPERTIES VERSION ${AQUAMARINE_VERSION}
                                            SOVERSION 11)

# ============================================================
# 收集所有链接库
# ============================================================
set(AQUAMARINE_LIBRARIES)

# OpenGL ES 和 EGL（Anland 需要）
list(APPEND AQUAMARINE_LIBRARIES OpenGL::EGL OpenGL::GLES2)

# pkg-config 依赖
list(APPEND AQUAMARINE_LIBRARIES PkgConfig::deps)

# 手动找到的库
if(HYPRUTILS_LIB)
    list(APPEND AQUAMARINE_LIBRARIES ${HYPRUTILS_LIB})
endif()
if(GBM_LIB)
    list(APPEND AQUAMARINE_LIBRARIES ${GBM_LIB})
endif()
if(DRM_LIB)
    list(APPEND AQUAMARINE_LIBRARIES ${DRM_LIB})
endif()

check_include_file("sys/timerfd.h" HAS_TIMERFD)
pkg_check_modules(epoll IMPORTED_TARGET epoll-shim)
if(NOT HAS_TIMERFD AND epoll_FOUND)
  list(APPEND AQUAMARINE_LIBRARIES PkgConfig::epoll)
endif()

# ============================================================
# PipeWire 用于音频/摄像头（可选）
# ============================================================
set(PIPEWIRE_FOUND OFF)
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
if(PIPEWIRE_FOUND)
    list(APPEND AQUAMARINE_LIBRARIES PkgConfig::PIPEWIRE)
    target_compile_definitions(aquamarine PRIVATE HAVE_PIPEWIRE)
    message(STATUS "PipeWire found, audio/camera support enabled")
else()
    message(STATUS "PipeWire not found, audio/camera support disabled")
endif()

# ============================================================
# Wayland 协议生成
# ============================================================
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
message(STATUS "Found wayland-protocols at ${WAYLAND_PROTOCOLS_DIR}")
pkg_get_variable(WAYLAND_SCANNER_PKGDATA_DIR wayland-scanner pkgdatadir)
message(STATUS "Found wayland-scanner pkgdatadir at ${WAYLAND_SCANNER_PKGDATA_DIR}")

function(protocolNew protoPath protoName external)
  if(external)
    set(path ${CMAKE_SOURCE_DIR}/${protoPath})
  else()
    set(path ${WAYLAND_PROTOCOLS_DIR}/${protoPath})
  endif()
  add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/protocols/${protoName}.cpp
           ${CMAKE_SOURCE_DIR}/protocols/${protoName}.hpp
    COMMAND hyprwayland-scanner --client ${path}/${protoName}.xml
            ${CMAKE_SOURCE_DIR}/protocols/
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  target_sources(aquamarine PRIVATE protocols/${protoName}.cpp
                                    protocols/${protoName}.hpp)
endfunction()

function(protocolWayland)
  add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/protocols/wayland.cpp
           ${CMAKE_SOURCE_DIR}/protocols/wayland.hpp
    COMMAND hyprwayland-scanner --wayland-enums --client
            ${WAYLAND_SCANNER_PKGDATA_DIR}/wayland.xml ${CMAKE_SOURCE_DIR}/protocols/
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  target_sources(aquamarine PRIVATE protocols/wayland.cpp protocols/wayland.hpp)
endfunction()

protocolWayland()

protocolNew("stable/xdg-shell" "xdg-shell" false)
protocolNew("stable/linux-dmabuf" "linux-dmabuf-v1" false)

# ============================================================
# Anland 后端子目录
# ============================================================
set(ANLAND_DIR ${CMAKE_SOURCE_DIR}/src/backend/anland)
if(EXISTS ${ANLAND_DIR}/CMakeLists.txt)
    add_subdirectory(${ANLAND_DIR})
    target_compile_definitions(aquamarine PRIVATE HAS_ANLAND_BACKEND)
    message(STATUS "Anland backend enabled")
else()
    message(STATUS "Anland backend not found, skipping")
endif()

# ============================================================
# 链接所有库
# ============================================================
target_link_libraries(aquamarine PRIVATE ${AQUAMARINE_LIBRARIES})

# ============================================================
# 添加 rpath（针对交叉编译）
# ============================================================
set_target_properties(aquamarine PROPERTIES
    BUILD_RPATH "${CMAKE_INSTALL_PREFIX}/lib:/usr/lib/aarch64-linux-gnu:/usr/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

# ============================================================
# 生成 hwdata 信息
# ============================================================
pkg_get_variable(HWDATA_DIR hwdata pkgdatadir)
message(
  STATUS "Running ${CMAKE_SOURCE_DIR}/data/hwdata.sh < ${HWDATA_DIR}/pnp.ids")
execute_process(
  COMMAND /bin/sh -c
          "${CMAKE_SOURCE_DIR}/data/hwdata.sh < ${HWDATA_DIR}/pnp.ids"
  RESULT_VARIABLE HWDATA_PNP_RESULT
  OUTPUT_VARIABLE HWDATA_PNP_IDS ENCODING UTF8)

if(NOT HWDATA_PNP_RESULT MATCHES 0)
  message(WARNING "hwdata gathering pnps failed")
endif()

configure_file(data/hwdata.hpp.in hwdata.hpp @ONLY)

# ============================================================
# 测试
# ============================================================
add_custom_target(tests)

add_executable(simpleWindow "tests/SimpleWindow.cpp")
target_link_libraries(simpleWindow PRIVATE PkgConfig::deps aquamarine)
add_test(
  NAME "simpleWindow"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests
  COMMAND simpleWindow "simpleWindow")
add_dependencies(tests simpleWindow)

add_executable(attachments "tests/Attachments.cpp")
target_link_libraries(attachments PRIVATE PkgConfig::deps aquamarine)
add_test(
  NAME "attachments"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests
  COMMAND attachments "attachments")
add_dependencies(tests attachments)

# ============================================================
# 安装
# ============================================================
install(TARGETS aquamarine)
install(DIRECTORY "include/aquamarine" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_BINARY_DIR}/aquamarine.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# 安装 Anland 头文件
if(EXISTS ${ANLAND_DIR}/CMakeLists.txt)
    install(DIRECTORY ${ANLAND_DIR}/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aquamarine/backend/anland
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
        PATTERN "CMakeLists.txt" EXCLUDE
        PATTERN "*.c" EXCLUDE
        PATTERN "*.cpp" EXCLUDE
    )
endif()