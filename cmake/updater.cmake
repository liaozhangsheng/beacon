if(APPLE)
    find_path(LibArchive_INCLUDE_DIR archive.h
        HINTS /opt/homebrew/opt/libarchive/include /usr/local/opt/libarchive/include)
    find_library(LibArchive_LIBRARY archive
        HINTS /opt/homebrew/opt/libarchive/lib /usr/local/opt/libarchive/lib)
endif()
find_package(LibArchive REQUIRED)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" beacon_processor)
if(beacon_processor MATCHES "^(aarch64|arm64)$")
    set(beacon_arch arm64)
elseif(beacon_processor MATCHES "^(x86_64|amd64|x64)$")
    set(beacon_arch x64)
else()
    message(FATAL_ERROR "Unsupported updater architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(WIN32)
    set(beacon_os windows)
elseif(APPLE)
    set(beacon_os macos)
else()
    set(beacon_os linux)
endif()
set(BEACON_UPDATE_PLATFORM "${beacon_os}-${beacon_arch}")
set(BEACON_UPDATE_PUBLIC_KEY_FILE "" CACHE FILEPATH "Ed25519 release verification public key (PEM)")
set(BEACON_UPDATE_PUBLIC_KEY "")
if(BEACON_UPDATE_PUBLIC_KEY_FILE)
    file(READ "${BEACON_UPDATE_PUBLIC_KEY_FILE}" BEACON_UPDATE_PUBLIC_KEY)
else()
    message(STATUS "Updater has no release public key: network updates will be disabled")
endif()
configure_file(cmake/update_config.hpp.in generated/beacon/update/config.hpp @ONLY)

add_library(beacon_update STATIC src/update/engine.cpp src/update/download.cpp)
target_link_libraries(beacon_update
    PUBLIC beacon_core beacon_install_lock beacon_http
    PRIVATE beacon_io jsoncpp_static OpenSSL::Crypto LibArchive::LibArchive)
beacon_warnings(beacon_update)
# main.cpp is compiled per executable: the embedded key and platform come from config.hpp.
add_executable(beacon-updater src/update/main.cpp)
target_include_directories(beacon-updater PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(beacon-updater PRIVATE beacon_update beacon_io OpenSSL::Crypto)
beacon_warnings(beacon-updater)
if(UNIX AND NOT APPLE)
    set_target_properties(beacon-updater PROPERTIES INSTALL_RPATH "$ORIGIN")
    # DT_RPATH also covers dependencies of the bundled HTTP/archive libraries.
    target_link_options(beacon-updater PRIVATE "LINKER:--disable-new-dtags")
endif()
install(TARGETS beacon-updater
    RUNTIME_DEPENDENCY_SET beacon_updater_dependencies
    RUNTIME DESTINATION updater)
if(APPLE)
    configure_file(cmake/bundle_updater.cmake.in bundle_updater.cmake.gen @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle_updater-$<CONFIG>.cmake"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle_updater.cmake.gen")
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/bundle_updater-$<CONFIG>.cmake")
elseif(WIN32)
    install(RUNTIME_DEPENDENCY_SET beacon_updater_dependencies
        PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
        POST_EXCLUDE_REGEXES ".*[/\\]Windows[/\\].*" ".*[/\\]System32[/\\].*"
        DIRECTORIES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
            "${OPENSSL_ROOT_DIR}/bin"
            "$<TARGET_FILE_DIR:LibArchive::LibArchive>"
        RUNTIME DESTINATION updater)
else()
    configure_file(cmake/bundle_updater_linux.cmake.in bundle_updater_linux.cmake.gen @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle_updater-$<CONFIG>.cmake"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle_updater_linux.cmake.gen")
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/bundle_updater-$<CONFIG>.cmake")
endif()
