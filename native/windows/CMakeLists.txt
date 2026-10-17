add_executable(hmawl_renderer_prototype WIN32
    main.cpp
    renderer_d3d11.cpp
    renderer_d3d11.h
)
target_compile_features(hmawl_renderer_prototype PRIVATE cxx_std_17)
target_compile_definitions(hmawl_renderer_prototype PRIVATE UNICODE _UNICODE)
target_link_libraries(hmawl_renderer_prototype PRIVATE d3d11 dxgi d3dcompiler)

if(MSVC)
    target_compile_options(hmawl_renderer_prototype PRIVATE /W4 /permissive-)
endif()
