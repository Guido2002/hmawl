include(FetchContent)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG f8745da6ff1ad1e7bab384bd1f9d742439278e99 # v1.5.7
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

add_library(hmawl_recompiler_core
    dol_image.cpp
    ppc_recompiler.cpp
    ppc_float.cpp
    dsp_init_recompiler.cpp
    rvz_image.cpp
    disc_import.cpp
    clz_image.cpp
    sha1.cpp
    rel_image.cpp
    native_dispatch.cpp
    native_dvd_reader.cpp
    native_dvd_queue.cpp
    native_sram.cpp
    native_gx.cpp
)
target_compile_features(hmawl_recompiler_core PUBLIC cxx_std_17)
target_include_directories(hmawl_recompiler_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hmawl_recompiler_core PRIVATE libzstd_static)

add_executable(hmawl_recompiler main.cpp)
target_link_libraries(hmawl_recompiler PRIVATE hmawl_recompiler_core)

set(HMAWL_ORIGINAL_US_DOL "" CACHE FILEPATH "Verified USA revision 0 DOL for the production original-startup runner")
if(HMAWL_ORIGINAL_US_DOL)
    get_filename_component(HMAWL_ORIGINAL_US_DOL "${HMAWL_ORIGINAL_US_DOL}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    add_executable(hmawl_generate_original_startup generate_original_startup.cpp)
    target_link_libraries(hmawl_generate_original_startup PRIVATE hmawl_recompiler_core)
    set(generated_original_startup "${CMAKE_CURRENT_BINARY_DIR}/generated_original_startup.h")
    add_custom_command(OUTPUT "${generated_original_startup}"
        COMMAND hmawl_generate_original_startup "${HMAWL_ORIGINAL_US_DOL}" "${generated_original_startup}"
        DEPENDS hmawl_generate_original_startup "${HMAWL_ORIGINAL_US_DOL}"
            original_startup_manifest.h
        VERBATIM
    )
    add_library(hmawl_original_startup_runtime original_startup.cpp "${generated_original_startup}")
    target_include_directories(hmawl_original_startup_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(hmawl_original_startup_runtime PUBLIC hmawl_recompiler_core)
    add_executable(hmawl_original_startup original_startup_main.cpp)
    target_link_libraries(hmawl_original_startup PRIVATE hmawl_original_startup_runtime)
    if(BUILD_TESTING)
        add_executable(hmawl_original_startup_tests original_startup_tests.cpp)
        target_link_libraries(hmawl_original_startup_tests PRIVATE hmawl_original_startup_runtime)
        add_test(NAME hmawl_original_startup_tests
            COMMAND hmawl_original_startup_tests "${HMAWL_ORIGINAL_US_DOL}")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(hmawl_recompiler_tests tests.cpp)
    target_link_libraries(hmawl_recompiler_tests PRIVATE hmawl_recompiler_core)
    add_test(NAME hmawl_recompiler_tests COMMAND hmawl_recompiler_tests)
    add_executable(hmawl_native_gx_tests native_gx_tests.cpp)
    target_link_libraries(hmawl_native_gx_tests PRIVATE hmawl_recompiler_core)
    add_test(NAME hmawl_native_gx_tests COMMAND hmawl_native_gx_tests)
    add_executable(hmawl_ppc_float_tests ppc_float_tests.cpp)
    target_link_libraries(hmawl_ppc_float_tests PRIVATE hmawl_recompiler_core)
    if(MSVC)
        target_compile_options(hmawl_ppc_float_tests PRIVATE /fp:strict)
    endif()
    add_test(NAME hmawl_ppc_float_tests COMMAND hmawl_ppc_float_tests)
    add_executable(hmawl_import_tests import_tests.cpp)
    target_link_libraries(hmawl_import_tests PRIVATE hmawl_recompiler_core libzstd_static)
    add_test(NAME hmawl_import_tests COMMAND hmawl_import_tests)
    add_executable(hmawl_rel_tests rel_tests.cpp)
    target_link_libraries(hmawl_rel_tests PRIVATE hmawl_recompiler_core)
    add_test(NAME hmawl_rel_tests COMMAND hmawl_rel_tests)
    set(HMAWL_US_REL_DIRECTORY "" CACHE PATH "Local verified DOL/REL directory for optional module link tests")
    if(HMAWL_US_REL_DIRECTORY)
        get_filename_component(HMAWL_US_REL_DIRECTORY "${HMAWL_US_REL_DIRECTORY}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        add_test(NAME hmawl_us_rel_tests
            COMMAND hmawl_rel_tests --us "${HMAWL_US_REL_DIRECTORY}")
    endif()

    add_executable(hmawl_generate_tests generate_tests.cpp)
    target_link_libraries(hmawl_generate_tests PRIVATE hmawl_recompiler_core)
    set(generated_tests "${CMAKE_CURRENT_BINARY_DIR}/generated_ppc_tests.h")
    add_custom_command(OUTPUT "${generated_tests}"
        COMMAND hmawl_generate_tests "${generated_tests}"
        DEPENDS hmawl_generate_tests
        VERBATIM
    )
    add_executable(hmawl_generated_tests generated_tests.cpp "${generated_tests}")
    target_include_directories(hmawl_generated_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(hmawl_generated_tests PRIVATE hmawl_recompiler_core)
    add_test(NAME hmawl_generated_tests COMMAND hmawl_generated_tests)

    if(HMAWL_US_REL_DIRECTORY)
        set(generated_rel "${CMAKE_CURRENT_BINARY_DIR}/generated_us_rel.h")
        add_custom_command(OUTPUT "${generated_rel}"
            COMMAND hmawl_generate_tests --rel "${HMAWL_US_REL_DIRECTORY}" "${generated_rel}"
            DEPENDS hmawl_generate_tests
                "${HMAWL_US_REL_DIRECTORY}/main.dol"
                "${HMAWL_US_REL_DIRECTORY}/wlp0.rel"
                "${HMAWL_US_REL_DIRECTORY}/wlp1.rel"
                "${HMAWL_US_REL_DIRECTORY}/wlp2.rel"
            VERBATIM
        )
        add_executable(hmawl_us_rel_native_tests us_rel_native_tests.cpp "${generated_rel}")
        target_include_directories(hmawl_us_rel_native_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(hmawl_us_rel_native_tests PRIVATE hmawl_recompiler_core)
        add_test(NAME hmawl_us_rel_native_tests
            COMMAND hmawl_us_rel_native_tests "${HMAWL_US_REL_DIRECTORY}")
        set(HMAWL_US_RVZ "" CACHE FILEPATH "Local US revision 0 RVZ for optional original resource tests")
        if(HMAWL_US_RVZ)
            get_filename_component(HMAWL_US_RVZ "${HMAWL_US_RVZ}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
            add_test(NAME hmawl_us_resource_tests
                COMMAND hmawl_us_rel_native_tests "${HMAWL_US_REL_DIRECTORY}" --rvz "${HMAWL_US_RVZ}")
            if(TARGET hmawl_original_startup_tests)
                add_test(NAME hmawl_original_disc_startup_tests
                    COMMAND hmawl_original_startup_tests "${HMAWL_ORIGINAL_US_DOL}" --rvz "${HMAWL_US_RVZ}")
            endif()
        endif()
    endif()

    set(HMAWL_US_DOL "" CACHE FILEPATH "Local verified US revision 0 DOL for optional native startup tests")
    if(HMAWL_US_DOL)
        get_filename_component(HMAWL_US_DOL "${HMAWL_US_DOL}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        set(generated_startup "${CMAKE_CURRENT_BINARY_DIR}/generated_us_startup.h")
        add_custom_command(OUTPUT "${generated_startup}"
            COMMAND hmawl_generate_tests --startup "${HMAWL_US_DOL}" "${generated_startup}"
            DEPENDS hmawl_generate_tests "${HMAWL_US_DOL}"
            VERBATIM
        )
        add_executable(hmawl_us_startup_tests startup_tests.cpp "${generated_startup}")
        target_include_directories(hmawl_us_startup_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(hmawl_us_startup_tests PRIVATE hmawl_recompiler_core)
        add_test(NAME hmawl_us_startup_tests COMMAND hmawl_us_startup_tests "${HMAWL_US_DOL}")
    endif()
endif()
