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

if(TARGET beacon_update)
    add_executable(beacon_update_test update_engine_test.cpp update_lock_test.cpp update_download_test.cpp
        http_trust_test.cpp)
    target_link_libraries(beacon_update_test PRIVATE beacon_update beacon_http
        OpenSSL::Crypto LibArchive::LibArchive Catch2::Catch2WithMain)
    beacon_warnings(beacon_update_test)
    catch_discover_tests(beacon_update_test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME beacon_update_package COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/test/update_package_test.py)

    find_program(BEACON_OPENSSL_EXECUTABLE openssl REQUIRED)
    add_executable(beacon_https_probe https_probe.cpp)
    target_link_libraries(beacon_https_probe PRIVATE beacon_update)
    beacon_warnings(beacon_https_probe)
    add_test(NAME beacon_https_security COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/https_test.py $<TARGET_FILE:beacon_https_probe> ${BEACON_OPENSSL_EXECUTABLE})
    set_tests_properties(beacon_https_security PROPERTIES TIMEOUT 90)
endif()

if(TARGET beacon-updater)
    function(beacon_configure_test_update_config output)
        set(BEACON_UPDATE_PUBLIC_KEY_FILE
            "${PROJECT_SOURCE_DIR}/test/fixtures/update-signing-public.pem")
        file(READ "${BEACON_UPDATE_PUBLIC_KEY_FILE}" BEACON_UPDATE_PUBLIC_KEY)
        set(BEACON_UPDATE_PLATFORM "${BEACON_UPDATE_PLATFORM}")
        configure_file("${PROJECT_SOURCE_DIR}/cmake/update_config.hpp.in" "${output}" @ONLY)
    endfunction()

    set(BEACON_TEST_UPDATE_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/test-generated/beacon/update/config.hpp")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-generated/beacon/update")
    beacon_configure_test_update_config("${BEACON_TEST_UPDATE_CONFIG}")

    add_executable(beacon-updater-test ../src/update/main.cpp)
    target_include_directories(beacon-updater-test PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/test-generated")
    target_link_libraries(beacon-updater-test PRIVATE beacon_update beacon_io OpenSSL::Crypto)
    beacon_warnings(beacon-updater-test)

    add_executable(beacon_update_smoke_fixture update_smoke_fixture.cpp)
    beacon_warnings(beacon_update_smoke_fixture)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(BEACON_UPDATE_CLI_TEST_ARGS
        --updater $<TARGET_FILE:beacon-updater-test>
        --key "${PROJECT_SOURCE_DIR}/test/fixtures/update-signing-key.pem"
        --platform "${BEACON_UPDATE_PLATFORM}"
        --openssl "${BEACON_OPENSSL_EXECUTABLE}"
        --smoke-fixture $<TARGET_FILE:beacon_update_smoke_fixture>)
    if(NOT BEACON_UPDATE_PUBLIC_KEY_FILE)
        list(APPEND BEACON_UPDATE_CLI_TEST_ARGS --production-updater $<TARGET_FILE:beacon-updater>)
    endif()
    add_test(NAME beacon_update_cli COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/test/update_cli_test.py ${BEACON_UPDATE_CLI_TEST_ARGS})
    set_tests_properties(beacon_update_cli PROPERTIES TIMEOUT 90)
endif()

if(TARGET beacon)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(beacon_ui_test ui_assets_test.cpp window_context_test.cpp
        ../src/ui/widgets.cpp ../src/ui/window_context.cpp)
    target_link_libraries(beacon_ui_test PRIVATE beacon_imgui beacon_resources SDL3_image::SDL3_image Catch2::Catch2WithMain)
    target_compile_definitions(beacon_ui_test PRIVATE BEACON_TEST_ROOT="${PROJECT_SOURCE_DIR}")
    beacon_warnings(beacon_ui_test)
    catch_discover_tests(beacon_ui_test)

    add_test(NAME beacon_desktop_smoke COMMAND beacon --smoke-test)

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
