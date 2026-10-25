if(NOT UNIX)
    message(STATUS "Emulation layer tests are only set up for Linux; skipping.")
    return()
endif()

add_library(mock_runtime SHARED mock_runtime/mock_runtime.cpp)
target_include_directories(mock_runtime PRIVATE "${OPENXR_INCLUDE_DIR}")
file(GENERATE
     OUTPUT "$<TARGET_FILE_DIR:mock_runtime>/mock_runtime.json"
     CONTENT "{\n  \"file_format_version\": \"1.0.0\",\n  \"runtime\": {\n    \"name\": \"Mock runtime\",\n    \"library_path\": \"$<TARGET_FILE:mock_runtime>\"\n  }\n}\n")

add_executable(layer_test layer_test.cpp)
target_include_directories(layer_test PRIVATE "${OPENXR_INCLUDE_DIR}")
target_link_libraries(layer_test PRIVATE openxr_loader ${CMAKE_DL_LIBS})
add_dependencies(layer_test mock_runtime XrApiLayer_spatial_container_emulation)

set(TEST_ENV
    "XR_RUNTIME_JSON=$<TARGET_FILE_DIR:mock_runtime>/mock_runtime.json"
    "XR_API_LAYER_PATH=$<TARGET_FILE_DIR:XrApiLayer_spatial_container_emulation>"
    "XR_ENABLE_API_LAYERS=${LAYER_NAME}")

add_test(NAME layer_basic COMMAND layer_test "$<TARGET_FILE:mock_runtime>" basic)
set_tests_properties(layer_basic PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_test(NAME layer_deny COMMAND layer_test "$<TARGET_FILE:mock_runtime>" deny)
set_tests_properties(layer_deny PROPERTIES ENVIRONMENT
    "${TEST_ENV};XR_SCEMU_DENY_VISIBLE_REQUESTS=1;XR_SCEMU_DENY_BOUNDS_MODE_REQUESTS=1")
