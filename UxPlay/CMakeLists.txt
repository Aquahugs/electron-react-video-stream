# CMakeLists.txt for UxPlay with FFmpeg + libwebsockets

# Choose minimum CMake version based on OS
if (APPLE)
  cmake_minimum_required(VERSION 3.13)
else()
  cmake_minimum_required(VERSION 3.10)
endif()

project(uxplay)

message(STATUS "Project name: " ${PROJECT_NAME})

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 11)

# Optional old references (ZOOMFIX, etc.)
if(ZOOMFIX)
  message(STATUS "cmake option ZOOMFIX is no longer used (if needed, ZOOMFIX is automatically applied if X11 libraries are present).")
endif()

# X11 detection if on Linux or if USE_X11 is set
if((UNIX AND NOT APPLE) OR USE_X11)
  if(NOT NO_X11_DEPS)
    find_package(X11)
    if(X11_FOUND)
      message(STATUS "Will compile using X11 Libraries (use cmake option -DNO_X11_DEPS=ON if X11 is not wanted).")
      link_libraries(${X11_LIBRARIES})
      include_directories(${X11_INCLUDE_DIR})
    else()
      message(STATUS "X11 libraries not found, will compile without X11 dependence.")
    endif()
  else()
    message(STATUS "Will compile without X11 dependence (NO_X11_DEPS=ON).")
  endif()
endif()

# If on Linux/Unix (not Apple), define some flags
if(UNIX AND NOT APPLE)
  add_definitions(-DSUPPRESS_AVAHI_COMPAT_WARNING)
  add_definitions(-DFULL_RANGE_RGB_FIX)
else()
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

# Add subdirectories (the internal libs)
add_subdirectory(lib/llhttp)
add_subdirectory(lib/playfair)
add_subdirectory(lib)
add_subdirectory(renderers)

# Find required packages
find_package(PkgConfig REQUIRED)

# Find FFmpeg components
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
)

if(NOT FFMPEG_FOUND)
    message(FATAL_ERROR "FFmpeg not found. Please install FFmpeg development packages.")
endif()

message(STATUS "Found FFmpeg version: ${FFMPEG_VERSION}")

# Find libwebsockets
pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)
if(NOT LIBWEBSOCKETS_FOUND)
    message(FATAL_ERROR "libwebsockets not found. Please install libwebsockets development packages.")
endif()

# Include directories
include_directories(
    ${FFMPEG_INCLUDE_DIRS}
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/include
)

# Link directories
link_directories(
    ${FFMPEG_LIBRARY_DIRS}
    ${LIBWEBSOCKETS_LIBRARY_DIRS}
)

# Add compiler flags for FFmpeg
add_definitions(${FFMPEG_CFLAGS})

# Now build the uxplay executable
add_executable(uxplay uxplay.cpp)

# Link libraries
target_link_libraries(uxplay
    PRIVATE
        renderers
        airplay
        ${FFMPEG_LIBRARIES}
        ${LIBWEBSOCKETS_LIBRARIES}
        pthread
)

# If using pkg-config's IMPORTED_TARGET feature (modern CMake):
target_link_libraries(uxplay
    PRIVATE
        PkgConfig::FFMPEG
)

# Install the uxplay binary
install(TARGETS uxplay RUNTIME DESTINATION bin)

# Install manpage, docs, etc.
install(FILES uxplay.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES README.md README.txt README.html LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES lib/llhttp/LICENSE-MIT
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/llhttp)

# Uninstall target
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY
  )

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
  )
endif()

# Add compile definitions for FFmpeg implementation
add_definitions(-DUSE_FFMPEG)

# Print configuration summary
message(STATUS "")
message(STATUS "UxPlay Configuration Summary:")
message(STATUS "----------------------------")
message(STATUS "FFmpeg Version:      ${FFMPEG_VERSION}")
message(STATUS "libwebsockets:       ${LIBWEBSOCKETS_VERSION}")
message(STATUS "Install prefix:      ${CMAKE_INSTALL_PREFIX}")
if(X11_FOUND)
    message(STATUS "X11 Support:        Yes")
else()
    message(STATUS "X11 Support:        No")
endif()
message(STATUS "Build Type:         ${CMAKE_BUILD_TYPE}")
message(STATUS "----------------------------")
message(STATUS "")