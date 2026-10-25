cmake_minimum_required(VERSION 3.16)
project(spatial_container_emulation_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

option(SCEMU_BUILD_TESTS "Build the mock runtime and end-to-end tests (needs the OpenXR loader)" ON)

# OpenXR headers (and the loader for tests). Point OPENXR_SDK_SOURCE_DIR at a local
# checkout of KhronosGroup/OpenXR-SDK to avoid downloading it.
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
set(OPENXR_INCLUDE_DIR "${openxr_sdk_SOURCE_DIR}/include")

set(LAYER_NAME XR_APILAYER_spatial_container_emulation)
add_library(XrApiLayer_spatial_container_emulation SHARED src/layer.cpp)
target_include_directories(XrApiLayer_spatial_container_emulation PRIVATE "${OPENXR_INCLUDE_DIR}")
if(WIN32)
    set(LAYER_LIBRARY_PATH ".\\\\$<TARGET_FILE_NAME:XrApiLayer_spatial_container_emulation>")
else()
    set(LAYER_LIBRARY_PATH "./$<TARGET_FILE_NAME:XrApiLayer_spatial_container_emulation>")
endif()
configure_file(manifest/XrApiLayer_spatial_container_emulation.json.in
               "${CMAKE_CURRENT_BINARY_DIR}/manifest.json.in" @ONLY)
file(GENERATE
     OUTPUT "$<TARGET_FILE_DIR:XrApiLayer_spatial_container_emulation>/XrApiLayer_spatial_container_emulation.json"
     INPUT "${CMAKE_CURRENT_BINARY_DIR}/manifest.json.in")

install(TARGETS XrApiLayer_spatial_container_emulation
        LIBRARY DESTINATION . RUNTIME DESTINATION .)
install(FILES "$<TARGET_FILE_DIR:XrApiLayer_spatial_container_emulation>/XrApiLayer_spatial_container_emulation.json"
        DESTINATION .)

if(SCEMU_BUILD_TESTS)
    enable_testing()
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
    # A parent project (such as samples/cpp-minimal) may already provide the loader.
    if(NOT TARGET openxr_loader)
        add_subdirectory("${openxr_sdk_SOURCE_DIR}" openxr_sdk EXCLUDE_FROM_ALL)
    endif()
    add_subdirectory(tests)
endif()
