cmake_minimum_required(VERSION 3.21)

set(POPTAPPO_VERSION "1.0.0" CACHE STRING "PopTappo version (MAJOR.MINOR.PATCH[-prerelease])")
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$" _poptappo_ver_match "${POPTAPPO_VERSION}")
if(NOT _poptappo_ver_match)
    message(FATAL_ERROR "Invalid POPTAPPO_VERSION: '${POPTAPPO_VERSION}'. Expected MAJOR.MINOR.PATCH[-prerelease].")
endif()
set(POPTAPPO_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(POPTAPPO_VERSION_MINOR "${CMAKE_MATCH_2}")
set(POPTAPPO_VERSION_PATCH "${CMAKE_MATCH_3}")

project(PopTappo VERSION ${POPTAPPO_VERSION_MAJOR}.${POPTAPPO_VERSION_MINOR}.${POPTAPPO_VERSION_PATCH} LANGUAGES CXX RC)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

add_executable(PopTappo WIN32 src/main.cpp src/engine.cpp src/tappo.cpp src/updater.cpp src/app.rc)
target_include_directories(PopTappo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(PopTappo PRIVATE cxx_std_20)
target_compile_definitions(PopTappo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(PopTappo PRIVATE /utf-8 /W4 /permissive-)
target_link_options(PopTappo PRIVATE /MANIFEST:NO)
target_link_libraries(PopTappo PRIVATE ole32 oleaut32 shell32 user32 uuid winhttp wtsapi32)
install(TARGETS PopTappo RUNTIME DESTINATION .)
