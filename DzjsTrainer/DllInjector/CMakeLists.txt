cmake_minimum_required(VERSION 3.20)

project(DllInjector LANGUAGES CXX)

add_library(DllInjector STATIC
    DllInjector.cpp
    DllInjector.h
)

target_compile_features(DllInjector PUBLIC cxx_std_17)
target_compile_definitions(DllInjector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DllInjector PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(DllInjector PRIVATE /W4 /permissive- /utf-8)
endif()
