cmake_minimum_required(VERSION 3.22)
project(bili-live-obs
  VERSION 3.0.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- libobs ----
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

# ---- Qt5 ----
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Network)

# ---- system libs ----
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(LIBQRENCODE REQUIRED libqrencode)
pkg_check_modules(OPENSSL REQUIRED openssl)

# nlohmann/json is header-only
find_path(NLOHMANN_INCLUDE_DIR nlohmann/json.hpp
  PATHS ${CMAKE_SOURCE_DIR}/vendor /usr/include /usr/local/include
  REQUIRED
)

# ---- sources ----
set(SOURCES
  src/plugin-main.cpp
  src/config-manager.cpp
  src/bilibili-api.cpp
  src/auth-service.cpp
  src/bili-dock.cpp
)

set(HEADERS
  src/config-manager.h
  src/bilibili-api.h
  src/auth-service.h
  src/bili-dock.h
)

# ---- target ----
add_library(bili-live-obs MODULE ${SOURCES} ${HEADERS})

target_include_directories(bili-live-obs PRIVATE
  ${NLOHMANN_INCLUDE_DIR}
  ${LIBCURL_INCLUDE_DIRS}
  ${LIBQRENCODE_INCLUDE_DIRS}
  ${OPENSSL_INCLUDE_DIRS}
)

target_link_libraries(bili-live-obs PRIVATE
  OBS::libobs
  OBS::obs-frontend-api
  Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network
  ${LIBCURL_LIBRARIES}
  ${LIBQRENCODE_LIBRARIES}
  ${OPENSSL_LIBRARIES}
)

target_compile_definitions(bili-live-obs PRIVATE
  ${LIBCURL_CFLAGS_OTHER}
  ${LIBQRENCODE_CFLAGS_OTHER}
  ${OPENSSL_CFLAGS_OTHER}
  PROJECT_VERSION="${PROJECT_VERSION}"
)

set_target_properties(bili-live-obs PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# ---- install ----
install(TARGETS bili-live-obs
  LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/obs-plugins"
)

install(DIRECTORY data/
  DESTINATION "${CMAKE_INSTALL_PREFIX}/share/obs/obs-plugins/${CMAKE_PROJECT_NAME}"
  OPTIONAL
)

# ---- CPack / DEB ----
set(CPACK_PACKAGE_NAME "bili-live-obs")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "BiliLiveOBS")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "B站直播工具 OBS 插件 - 扫码登录/开播/停播/分区管理")
set(CPACK_PACKAGE_CONTACT "https://github.com/${GITHUB_REPOSITORY}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "BiliLiveOBS")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "obs-studio (>= 30.0), libqt5widgets5, libqt5network5, libcurl4, libqrencode4, libssl3")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

# For DEB packaging, install into /usr
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

include(CPack)
