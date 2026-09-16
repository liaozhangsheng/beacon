find_package(Catch2 3 REQUIRED CONFIG)

add_executable(beacon_test
    file_test.cpp
    core_test.cpp
    modern_test.cpp
    presentation_test.cpp
)
target_link_libraries(beacon_test
    PRIVATE
        beacon_runtime
        beacon_resources
        beacon_io
        Catch2::Catch2WithMain
)
target_compile_definitions(beacon_test PRIVATE BEACON_TEST_ROOT="${PROJECT_SOURCE_DIR}")
beacon_warnings(beacon_test)

include(Catch)
catch_discover_tests(beacon_test)

if(TARGET beacon)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(BEACON_OPENSSL_EXECUTABLE openssl REQUIRED)
    add_executable(beacon_https_probe https_probe.cpp)
    target_link_libraries(beacon_https_probe PRIVATE beacon_profile)
    beacon_warnings(beacon_https_probe)
    add_test(NAME beacon_https_security COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/https_test.py $<TARGET_FILE:beacon_https_probe> ${BEACON_OPENSSL_EXECUTABLE})
    set_tests_properties(beacon_https_security PROPERTIES TIMEOUT 90)

    add_executable(beacon_ui_test ui_assets_test.cpp window_context_test.cpp
        ../src/ui/widgets.cpp ../src/ui/window_context.cpp)
    target_link_libraries(beacon_ui_test PRIVATE beacon_imgui beacon_resources SDL3_image::SDL3_image Catch2::Catch2WithMain)
    target_compile_definitions(beacon_ui_test PRIVATE BEACON_TEST_ROOT="${PROJECT_SOURCE_DIR}")
    beacon_warnings(beacon_ui_test)
    catch_discover_tests(beacon_ui_test)

    add_test(NAME beacon_desktop_smoke COMMAND beacon --smoke-test)
    set_tests_properties(beacon_desktop_smoke PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_test(
        NAME beacon_installed_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DBEACON_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DBEACON_INSTALL_DIR=${CMAKE_BINARY_DIR}/installed-smoke"
            "-DBEACON_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
            "-DBEACON_BUILD_CONFIG=$<CONFIG>"
            -P "${PROJECT_SOURCE_DIR}/cmake/installed_smoke.cmake"
    )
    set_tests_properties(beacon_installed_smoke PROPERTIES TIMEOUT 60)
endif()
