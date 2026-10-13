cmake_minimum_required(VERSION 3.16)
project(obs-streamtranslate C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OBS REQUIRED IMPORTED_TARGET libobs)
find_package(Threads REQUIRED)

option(ST_STATIC_LIBWEBSOCKETS "Build a pinned libwebsockets into the plugin" ON)
if(ST_STATIC_LIBWEBSOCKETS)
  include(FetchContent)
  # Build from upstream source, not a distro archive that may lack PIC. Keep TLS
  # verification enabled; OBS and the operating system's TLS libraries remain dependencies.
  set(LWS_WITH_STATIC ON CACHE BOOL "" FORCE)
  set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
  set(LWS_WITH_SSL ON CACHE BOOL "" FORCE)
  set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
  set(LWS_WITH_LIBUV OFF CACHE BOOL "" FORCE)
  set(LWS_WITH_LIBEV OFF CACHE BOOL "" FORCE)
  set(LWS_WITH_LIBEVENT OFF CACHE BOOL "" FORCE)
  set(LWS_WITH_ZLIB OFF CACHE BOOL "" FORCE)
  set(LWS_WITH_PLUGINS OFF CACHE BOOL "" FORCE)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  FetchContent_Declare(lws
    URL https://codeload.github.com/warmcat/libwebsockets/tar.gz/refs/tags/v4.3.5
    URL_HASH SHA256=87f99ad32803ed325fceac5327aae1f5c1b417d54ee61ad36cffc8df5f5ab276)
  FetchContent_MakeAvailable(lws)
  set(ST_LWS_TARGET websockets)
else()
  pkg_check_modules(LWS REQUIRED IMPORTED_TARGET libwebsockets)
  set(ST_LWS_TARGET PkgConfig::LWS)
endif()

add_library(obs-streamtranslate MODULE plugin.c)
if(ST_STATIC_LIBWEBSOCKETS)
  # Match headers to the bundled archive even if OBS/pkg-config includes a
  # directory containing a different system libwebsockets version.
  target_include_directories(obs-streamtranslate BEFORE PRIVATE "${lws_SOURCE_DIR}/include" "${lws_BINARY_DIR}")
endif()
target_link_libraries(obs-streamtranslate PRIVATE PkgConfig::OBS ${ST_LWS_TARGET} Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(obs-streamtranslate PRIVATE _GNU_SOURCE)
# Do not export/interpose our private copy of lws into other OBS plugins.
if(ST_STATIC_LIBWEBSOCKETS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(obs-streamtranslate PRIVATE "LINKER:--exclude-libs,ALL")
endif()
set_target_properties(obs-streamtranslate PROPERTIES PREFIX "")
