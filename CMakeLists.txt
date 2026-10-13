cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# >>> template makes dep warnings errors, must be before compilerconfig
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)

include(compilerconfig)
include(defaults)
include(helpers)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)

# vvv json vvv
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

# vvv tls: mbedtls on windows, securetransport on mac, openssl on linux vvv
if(WIN32)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(GEN_FILES OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
  set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(mbedtls)
  # >>> ixwebsocket would look for an installed mbedtls otherwise
  set(USE_MBED_TLS ON CACHE BOOL "" FORCE)
  set(MBEDTLS_FOUND TRUE)
  set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include")
  set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
endif()

# vvv websocket + https client vvv
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ixwebsocket)
if(WIN32)
  # >>> its mbedtls 3 check sits inside the find block we skip
  target_compile_definitions(ixwebsocket PRIVATE IXWEBSOCKET_USE_MBED_TLS_MIN_VERSION_3)
endif()

# vvv plugin vvv
add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 COMPONENTS Widgets Core REQUIRED)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json
)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE mbedcrypto ws2_32 crypt32)
endif()

target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp src/settings.cpp src/obs-bridge.cpp src/relay.cpp src/rules.cpp src/ui.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
