PROJECT(ffmpeg_tutorial)
CMAKE_MINIMUM_REQUIRED(VERSION 3.10)

# RGA Hardware Acceleration Control
option(ENABLE_RGA_HARDWARE "Enable RGA hardware acceleration (disable for software-only mode)" ON)

if(ENABLE_RGA_HARDWARE)
    add_definitions(-DENABLE_RGA_HARDWARE=1)
    message(STATUS "RGA hardware acceleration ENABLED")
else()
    add_definitions(-DENABLE_RGA_HARDWARE=0)
    message(STATUS "RGA hardware acceleration DISABLED - using software-only processing")
endif()

# Optimization flags for performance
add_definitions(-O3 -DNDEBUG -funwind-tables -rdynamic)

# Enable ARM NEON optimizations if available
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    add_definitions(-D__ARM_NEON)
    # For aarch64, NEON is always available, no need for -mfpu flag
    message(STATUS "ARM NEON optimizations enabled")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_definitions(-D_GLIBCXX_USE_C99=1)

set(CMAKE_C_FLAGS "-Wno-attributes -Wno-write-strings -Wno-deprecated-declarations ${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS "-Wno-attributes -Wno-write-strings -Wno-deprecated-declarations ${CMAKE_CXX_FLAGS}")

# Use system library paths
set(CMAKE_EXE_LINKER_FLAGS "-Wl,-rpath=/usr/lib/aarch64-linux-gnu ${CMAKE_EXE_LINKER_FLAGS}")

# Get all source files except main programs
file(GLOB SRC_LIST "*.cpp")
list(REMOVE_ITEM SRC_LIST "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
list(REMOVE_ITEM SRC_LIST "${CMAKE_CURRENT_SOURCE_DIR}/multi_stream_main.cpp")

# Find system packages
find_package(PkgConfig REQUIRED)

# OpenCV - use system installation
find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

# FFmpeg - use system installation
pkg_check_modules(LIBAV REQUIRED libavcodec libavformat libavutil libswscale libswresample libavfilter libavdevice)
include_directories(${LIBAV_INCLUDE_DIRS})
link_directories(${LIBAV_LIBRARY_DIRS})

# rockchip
include_directories(${PROJECT_SOURCE_DIR}/rockchip)
aux_source_directory(./rockchip RK_SRCS)
list(APPEND SRC_LIST ${RK_SRCS})

# System library paths for Rockchip hardware acceleration
link_directories(/usr/lib/aarch64-linux-gnu)
link_directories(/usr/lib)

# Include system headers for Rockchip libraries
include_directories(/usr/include/rga)

# Single stream executable
add_executable(ffmpeg_tutorial ${SRC_LIST} main.cpp)

# Multi-stream executable
add_executable(multi_stream_tutorial ${SRC_LIST} multi_stream_main.cpp)

# Link libraries for single stream executable
target_link_libraries(ffmpeg_tutorial ${OpenCV_LIBS})
target_link_libraries(ffmpeg_tutorial ${LIBAV_LIBRARIES})
target_link_libraries(ffmpeg_tutorial rga drm rknn_api)
target_link_libraries(ffmpeg_tutorial rockchip_mpp)
target_link_libraries(ffmpeg_tutorial pthread dl GL)

# Link libraries for multi-stream executable
target_link_libraries(multi_stream_tutorial ${OpenCV_LIBS})
target_link_libraries(multi_stream_tutorial ${LIBAV_LIBRARIES})
target_link_libraries(multi_stream_tutorial rga drm rknn_api)
target_link_libraries(multi_stream_tutorial rockchip_mpp)
target_link_libraries(multi_stream_tutorial pthread dl GL)

INSTALL(TARGETS ffmpeg_tutorial multi_stream_tutorial DESTINATION bin)
