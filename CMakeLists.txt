cmake_minimum_required(VERSION 3.20)

project(hmawl_native LANGUAGES C CXX)

include(CTest)
add_subdirectory(native/recompiler)

if(WIN32)
    add_subdirectory(native/windows)
endif()
