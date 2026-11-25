cmake_minimum_required(VERSION 3.21)
project(PopTappo LANGUAGES CXX RC)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(PopTappo WIN32 src/main.cpp src/engine.cpp src/tappo.cpp src/app.rc)
target_compile_features(PopTappo PRIVATE cxx_std_20)
target_compile_definitions(PopTappo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(PopTappo PRIVATE /utf-8 /W4 /permissive-)
target_link_options(PopTappo PRIVATE /MANIFEST:NO)
target_link_libraries(PopTappo PRIVATE ole32 oleaut32 shell32 user32 uuid wtsapi32)
install(TARGETS PopTappo RUNTIME DESTINATION .)
