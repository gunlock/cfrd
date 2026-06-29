# CFR Downloader
cmake_minimum_required(VERSION 3.25)

project(cfrd VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# fmt 12.1.0
# ---------------------------------------------------------------------------
FetchContent_Declare(
  fmt
  URL      https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz
  URL_HASH SHA256=ea7de4299689e12b6dddd392f9896f08fb0777ac7168897a244a6d6085043fea
)
FetchContent_MakeAvailable(fmt)

# ---------------------------------------------------------------------------
# yaml-cpp 0.9.0
# ---------------------------------------------------------------------------
FetchContent_Declare(
  yaml-cpp
  URL      https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.9.0.tar.gz
  URL_HASH SHA256=25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a
)
FetchContent_MakeAvailable(yaml-cpp)

# ---------------------------------------------------------------------------
# CLI11 v2.6.2
# ---------------------------------------------------------------------------
FetchContent_Declare(
  cli11
  URL      https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.6.2.tar.gz
  URL_HASH SHA256=c6ea6b2e5608b3ea8617999bd5f47420c71b2ebdb8dc4767c1034d1da5785711
)
FetchContent_MakeAvailable(cli11)

# ---------------------------------------------------------------------------
# OpenSSL (system) — required for HTTPS
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# libxml2 + libxslt (system) — required for govinfo XML parsing and XSL
# ---------------------------------------------------------------------------
find_package(LibXml2 REQUIRED)
find_package(LibXslt REQUIRED)

# ---------------------------------------------------------------------------
# cpp-httplib v0.48.0
# ---------------------------------------------------------------------------
FetchContent_Declare(
  httplib
  URL      https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.48.0.tar.gz
  URL_HASH SHA256=d9ed142d319c6e19a961f477257e67f846909ce15288502188df2281941be84e
)
FetchContent_MakeAvailable(httplib)

# ---------------------------------------------------------------------------
# Version header — generated from src/config.hpp.in into the build tree
# ---------------------------------------------------------------------------
configure_file(src/config.hpp.in src/config.hpp @ONLY)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(cfrd
  src/main.cpp
  src/ecfr_xsl.cpp
)

target_include_directories(cfrd PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src)

target_link_libraries(cfrd PRIVATE
  fmt::fmt
  yaml-cpp::yaml-cpp
  httplib::httplib
  OpenSSL::SSL
  OpenSSL::Crypto
  CLI11::CLI11
  LibXml2::LibXml2
  LibXslt::LibXslt
)
