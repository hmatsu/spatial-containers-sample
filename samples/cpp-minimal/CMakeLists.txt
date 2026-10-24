cmake_minimum_required(VERSION 3.16)
project(spatial_container_cpp_minimal LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CPPMIN_BUILD_TESTS "Run the sample headless through the emulation layer and its mock runtime (Linux)" ON)

# OpenXR headers and loader. Point OPENXR_SDK_SOURCE_DIR at a local checkout of
# KhronosGroup/OpenXR-SDK to avoid downloading it.
set(OPENXR_SDK_SOURCE_DIR "" CACHE PATH "Local OpenXR-SDK checkout (release 1.1.63 or later)")
if(OPENXR_SDK_SOURCE_DIR)
    set(openxr_sdk_SOURCE_DIR "${OPENXR_SDK_SOURCE_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.65
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(openxr_sdk)
    if(NOT openxr_sdk_POPULATED)
        FetchContent_Populate(openxr_sdk)
    endif()
endif()
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
add_subdirectory("${openxr_sdk_SOURCE_DIR}" openxr_sdk EXCLUDE_FROM_ALL)

add_executable(cpp_minimal src/main.cpp src/graphics_headless.cpp)
target_include_directories(cpp_minimal PRIVATE "${openxr_sdk_SOURCE_DIR}/include")
target_link_libraries(cpp_minimal PRIVATE openxr_loader)
if(WIN32)
    target_sources(cpp_minimal PRIVATE src/graphics_d3d11.cpp)
    target_link_libraries(cpp_minimal PRIVATE d3d11 dxgi d3dcompiler)
    # Put the loader next to the executable.
    add_custom_command(TARGET cpp_minimal POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:openxr_loader> $<TARGET_FILE_DIR:cpp_minimal>)
endif()

if(CPPMIN_BUILD_TESTS AND UNIX)
    enable_testing()
    set(SCEMU_BUILD_TESTS ON CACHE BOOL "" FORCE)
    add_subdirectory(../../tools/spatial-container-emulation-layer emulation_layer)

    add_test(NAME cpp_minimal_headless COMMAND cpp_minimal --headless --seconds 1 --toggle-seconds 0.3)
    set_tests_properties(cpp_minimal_headless PROPERTIES
        ENVIRONMENT "XR_RUNTIME_JSON=$<TARGET_FILE_DIR:mock_runtime>/mock_runtime.json;XR_API_LAYER_PATH=$<TARGET_FILE_DIR:XrApiLayer_spatial_container_emulation>;XR_ENABLE_API_LAYERS=XR_APILAYER_spatial_container_emulation"
        # The full lifecycle, in order: shown, both mode switches, hidden on request, clean shutdown.
        PASS_REGULAR_EXPRESSION "Visible: 1.*Bounds changed: immersive.*Bounds changed: bounded.*Hide requested.*Visible: 0.*Shut down"
        FAIL_REGULAR_EXPRESSION "failed:")
endif()
