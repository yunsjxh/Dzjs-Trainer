cmake_minimum_required(VERSION 3.20)

project(PhoneDialogProbe LANGUAGES CXX)

add_library(PhoneDialogProbe SHARED PhoneDialogProbe.cpp)

target_compile_features(PhoneDialogProbe PRIVATE cxx_std_17)
target_compile_definitions(PhoneDialogProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(PhoneDialogProbe PRIVATE user32)

if(MSVC)
    target_compile_options(PhoneDialogProbe PRIVATE /W4 /permissive- /utf-8)
endif()
