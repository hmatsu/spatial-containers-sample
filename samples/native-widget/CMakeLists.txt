cmake_minimum_required(VERSION 3.16)
project(spatial_container_native_widget LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WIDGET_BUILD_TESTS "Build unit tests, and on Linux the lifecycle test through the emulation layer" ON)

# OpenXR headers and loader. Point OPENXR_SDK_SOURCE_DIR at a local checkout of
# KhronosGroup/OpenXR-SDK to avoid downloading it.
set(OPENXR_SDK_SOURCE_DIR "" CACHE PATH "Local OpenXR-SDK checkout (release 1.1.63 or later)")
if(OPENXR_SDK_SOURCE_DIR)
    set(openxr_sdk_SOURCE_DIR "${OPENXR_SDK_SOURCE_DIR}")
else()
    include(FetchContent)
    # Populate only: the SDK is added below with our own options.
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
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

# The panel and its helpers, shared with the unit tests.
add_library(widget_core STATIC src/panel.cpp src/graphics_common.cpp)
target_include_directories(widget_core PUBLIC "${openxr_sdk_SOURCE_DIR}/include")

add_executable(native_widget src/main.cpp)
target_link_libraries(native_widget PRIVATE widget_core openxr_loader)

# Vulkan is optional: Linux needs it to show anything; on Windows Direct3D 11 is the default.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    target_sources(native_widget PRIVATE src/graphics_vulkan.cpp)
    target_compile_definitions(native_widget PRIVATE WIDGET_HAS_VULKAN)
    target_link_libraries(native_widget PRIVATE Vulkan::Vulkan)
    message(STATUS "Native widget: Vulkan backend enabled")
else()
    message(STATUS "Native widget: Vulkan not found, Vulkan backend disabled")
endif()
if(WIN32)
    target_sources(native_widget PRIVATE src/graphics_d3d11.cpp)
    target_link_libraries(native_widget PRIVATE d3d11 dxgi)
    # Put the loader next to the executable.
    add_custom_command(TARGET native_widget POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:openxr_loader> $<TARGET_FILE_DIR:native_widget>)
endif()

if(WIDGET_BUILD_TESTS)
    enable_testing()
    # Also builds the layer's unit tests, and on Linux its mock runtime and end-to-end tests.
    set(SCEMU_BUILD_TESTS ON CACHE BOOL "" FORCE)
    add_subdirectory(../../tools/spatial-container-emulation-layer emulation_layer)

    add_executable(native_widget_tests tests/widget_tests.cpp)
    target_link_libraries(native_widget_tests PRIVATE widget_core)
    target_include_directories(native_widget_tests SYSTEM PRIVATE "${DOCTEST_INCLUDE_DIR}")
    add_test(NAME native_widget_unit COMMAND native_widget_tests)
endif()

if(WIDGET_BUILD_TESTS AND UNIX)
    add_test(NAME native_widget_headless COMMAND native_widget --headless --seconds 2.5 --start)
    set_tests_properties(native_widget_headless PROPERTIES
        ENVIRONMENT "XR_RUNTIME_JSON=$<TARGET_FILE_DIR:mock_runtime>/mock_runtime.json;XR_API_LAYER_PATH=$<TARGET_FILE_DIR:XrApiLayer_spatial_container_emulation>;XR_ENABLE_API_LAYERS=XR_APILAYER_spatial_container_emulation"
        # Shown, then hidden on request and shut down cleanly, having noticed the
        # countdown change at least twice in 2.5 seconds.
        PASS_REGULAR_EXPRESSION "Visible: 1.*Hide requested.*Visible: 0.*Shut down after [0-9]+ frames, ([2-9]|[1-9][0-9]+) panel changes"
        FAIL_REGULAR_EXPRESSION "failed:")
endif()
