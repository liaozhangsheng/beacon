cmake_minimum_required(VERSION 3.21)

option(BUILD_TESTING "Build regression checks" OFF)
if(BUILD_TESTING)
    list(APPEND VCPKG_MANIFEST_FEATURES tests)
endif()

project(beacon
    VERSION 0.2.0
    DESCRIPTION "Minecraft speedrun progress tracker"
    LANGUAGES CXX
)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
    add_link_options("$<$<LINK_LANGUAGE:CXX>:-fexperimental-library>")
endif()

if(MSVC)
    set(BEACON_WARNING_OPTIONS /W4 /utf-8)
else()
    set(BEACON_WARNING_OPTIONS -Wall -Wextra -Wpedantic)
endif()

include(cmake/format.cmake)
include(GNUInstallDirs)
set(CMAKE_INSTALL_BINDIR "." CACHE PATH "Beacon executable and resource directory" FORCE)

function(beacon_warnings target)
    target_compile_options(${target} PRIVATE ${BEACON_WARNING_OPTIONS})
endfunction()

set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_CMAKE_PACKAGE OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_OBJECT_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/jsoncpp EXCLUDE_FROM_ALL)

add_library(beacon_core STATIC
    src/core/template.cpp
    src/core/evaluator.cpp
    src/core/json.cpp
)
target_include_directories(beacon_core
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(beacon_core PRIVATE jsoncpp_static)
# expected.hpp is header-only; the rule engine does not depend on networking or TLS.
target_include_directories(beacon_core SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/3rdparty/yalantinglibs/include)
beacon_warnings(beacon_core)

add_library(beacon_io STATIC src/io/file.cpp)
target_link_libraries(beacon_io PUBLIC beacon_core)
beacon_warnings(beacon_io)

add_library(beacon_resources STATIC
    src/ui/display.cpp
    src/ui/carousel.cpp
    src/ui/format.cpp
    src/ui/layout_metrics.cpp
    src/ui/presentation.cpp
    src/ui/progress_model.cpp
)
target_include_directories(beacon_resources PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(beacon_resources PUBLIC beacon_core PRIVATE beacon_io jsoncpp_static)
beacon_warnings(beacon_resources)

add_library(beacon_runtime STATIC
    src/minecraft/adapter.cpp
    src/minecraft/process.cpp
    src/app/persistence.cpp
    src/app/runtime.cpp
)
target_include_directories(beacon_runtime PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(beacon_runtime PUBLIC beacon_core PRIVATE beacon_io jsoncpp_static Threads::Threads)
if(WIN32)
    target_link_libraries(beacon_runtime PRIVATE shell32 user32)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(beacon_runtime PRIVATE src/minecraft/process_macos.mm)
    target_link_libraries(beacon_runtime PRIVATE "-framework AppKit")
elseif(UNIX)
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_link_libraries(beacon_runtime PRIVATE X11::X11)
        target_compile_definitions(beacon_runtime PRIVATE BEACON_HAVE_X11)
    endif()
endif()
beacon_warnings(beacon_runtime)

option(BEACON_BUILD_DESKTOP "Build the SDL desktop application" ON)
if(BEACON_BUILD_DESKTOP)
    set(YLT_ENABLE_SSL ON CACHE BOOL "" FORCE)
    add_subdirectory(3rdparty/yalantinglibs EXCLUDE_FROM_ALL)
    find_package(SDL3 3.2 REQUIRED CONFIG)
    find_package(OpenSSL REQUIRED)

    set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
    add_subdirectory(3rdparty/SDL_image EXCLUDE_FROM_ALL)

    # Patch build-tree copies; keep submodule checkouts untouched.
    function(beacon_patch source output patch)
        configure_file("${PROJECT_SOURCE_DIR}/${source}" "${CMAKE_CURRENT_BINARY_DIR}/thirdparty/${output}" COPYONLY)
        execute_process(
            COMMAND git apply --unidiff-zero "${PROJECT_SOURCE_DIR}/cmake/patches/${patch}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/thirdparty"
            RESULT_VARIABLE result
            ERROR_VARIABLE error
        )
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Cannot apply ${patch}: ${error}")
        endif()
    endfunction()
    beacon_patch(3rdparty/imgui/backends/imgui_impl_sdlrenderer3.cpp
        imgui_impl_sdlrenderer3.cpp imgui-sdlrenderer3.patch)
    beacon_patch(3rdparty/yalantinglibs/include/ylt/standalone/cinatra/coro_http_client.hpp
        cinatra/coro_http_client.hpp cinatra-client.patch)

    add_library(beacon_imgui STATIC
        3rdparty/imgui/imgui.cpp
        3rdparty/imgui/imgui_draw.cpp
        3rdparty/imgui/imgui_tables.cpp
        3rdparty/imgui/imgui_widgets.cpp
        3rdparty/imgui/backends/imgui_impl_sdl3.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/imgui_impl_sdlrenderer3.cpp
    )
    target_include_directories(beacon_imgui PUBLIC 3rdparty/imgui 3rdparty/imgui/backends)
    target_link_libraries(beacon_imgui PUBLIC SDL3::SDL3)
    beacon_warnings(beacon_imgui)

    add_library(beacon_profile STATIC src/ui/profile.cpp)
    target_include_directories(beacon_profile PUBLIC ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(beacon_profile PRIVATE beacon_runtime beacon_io jsoncpp_static yalantinglibs::yalantinglibs)
    get_target_property(BEACON_YLT_INCLUDES yalantinglibs INTERFACE_INCLUDE_DIRECTORIES)
    # Keep vendor headers in the same search class so the patched copy wins.
    target_include_directories(beacon_profile SYSTEM PRIVATE ${BEACON_YLT_INCLUDES})
    target_include_directories(beacon_profile PRIVATE ${PROJECT_SOURCE_DIR}/3rdparty/jsoncpp/include)
    target_include_directories(beacon_profile SYSTEM BEFORE PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/thirdparty
        ${PROJECT_SOURCE_DIR}/3rdparty/yalantinglibs/include/ylt/standalone/cinatra
    )
    target_include_directories(beacon_profile BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/cmake/compat)
    if(WIN32)
        target_link_libraries(beacon_profile PRIVATE ws2_32 dbghelp)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND YLT_ENABLE_IBV)
        find_path(BEACON_NUMA_INCLUDE_DIR numa.h REQUIRED)
        find_library(BEACON_NUMA_LIBRARY numa REQUIRED)
        target_include_directories(beacon_profile PRIVATE ${BEACON_NUMA_INCLUDE_DIR})
        target_link_libraries(beacon_profile PRIVATE ${BEACON_NUMA_LIBRARY})
    endif()
    beacon_warnings(beacon_profile)

    add_executable(beacon
        src/main.cpp
        src/ui/desktop.cpp
        src/ui/desktop_renderer.cpp
        src/ui/desktop_resources.cpp
        src/ui/desktop_layout.cpp
        src/ui/desktop_drawing.cpp
        src/ui/completion_animation.cpp
        src/ui/settings_panel.cpp
        src/ui/window_context.cpp
        src/ui/widgets.cpp
    )
    target_link_libraries(beacon PRIVATE
        beacon_profile
        beacon_resources
        beacon_imgui
        SDL3_image::SDL3_image
    )
    if(UNIX AND NOT APPLE)
        set_target_properties(beacon PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
    target_compile_definitions(beacon PRIVATE BEACON_VERSION="${PROJECT_VERSION}")
    beacon_warnings(beacon)

    if(WIN32)
        set_target_properties(beacon PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    if(MINGW)
        target_link_options(beacon PRIVATE -static-libgcc -static-libstdc++)
    endif()

    add_custom_command(TARGET beacon POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${PROJECT_SOURCE_DIR}/templates $<TARGET_FILE_DIR:beacon>/templates
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${PROJECT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:beacon>/assets
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:beacon>/.agents/skills/beacon-custom-template
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/.agents/skills/beacon-custom-template/SKILL.md
            $<TARGET_FILE_DIR:beacon>/.agents/skills/beacon-custom-template/SKILL.md
    )

    if(APPLE)
        install(TARGETS beacon
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        set(BEACON_RUNTIME_SEARCH_DIRS
            "$<TARGET_FILE_DIR:SDL3::SDL3>;$<TARGET_FILE_DIR:OpenSSL::SSL>;$<TARGET_FILE_DIR:OpenSSL::Crypto>"
        )
        configure_file(cmake/bundle_runtime.cmake.in bundle_runtime.cmake.gen @ONLY)
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle_runtime-$<CONFIG>.cmake"
            INPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle_runtime.cmake.gen")
        install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/bundle_runtime-$<CONFIG>.cmake")
    elseif(WIN32)
        file(GLOB BEACON_OPENSSL_RUNTIME_DLLS
            "${OPENSSL_ROOT_DIR}/bin/*ssl*.dll"
            "${OPENSSL_ROOT_DIR}/bin/*crypto*.dll"
            "${VCPKG_INSTALLED_DIR}/*/bin/*ssl*.dll"
            "${VCPKG_INSTALLED_DIR}/*/bin/*crypto*.dll"
        )
        install(TARGETS beacon
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        install(FILES
            "$<TARGET_RUNTIME_DLLS:beacon>"
            ${BEACON_OPENSSL_RUNTIME_DLLS}
            DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    else()
        set(BEACON_RUNTIME_POST_EXCLUDE_REGEXES)
        if(UNIX AND NOT APPLE)
            # CPack's ZIP generator turns SDL3's SONAME symlink into an empty
            # file. Install the real library below under the SONAME instead.
            list(APPEND BEACON_RUNTIME_POST_EXCLUDE_REGEXES "libSDL3\\.so\\.0")
        endif()
        install(TARGETS beacon
            RUNTIME_DEPENDENCY_SET beacon_runtime_dependencies
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        install(RUNTIME_DEPENDENCY_SET beacon_runtime_dependencies
            POST_EXCLUDE_REGEXES
                "^/lib/.*"
                "^/usr/lib/.*"
                ".*[/\\]Windows[/\\].*"
                ".*[/\\]System32[/\\].*"
                ${BEACON_RUNTIME_POST_EXCLUDE_REGEXES}
            DIRECTORIES
                "$<TARGET_FILE_DIR:SDL3::SDL3>"
                "$<TARGET_FILE_DIR:OpenSSL::SSL>"
                "$<TARGET_FILE_DIR:OpenSSL::Crypto>"
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        if(UNIX AND NOT APPLE)
            install(FILES "$<TARGET_FILE:SDL3::SDL3>"
                DESTINATION ${CMAKE_INSTALL_BINDIR}
                RENAME "libSDL3.so.0"
            )
        endif()
    endif()
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/assets"
    )
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/templates/"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/templates"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/.agents/skills/beacon-custom-template/SKILL.md"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/.agents/skills/beacon-custom-template"
    )
    install(FILES
        "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/3rdparty/SDL_image/LICENSE.txt"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME "SDL_image-LICENSE.txt"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/3rdparty/imgui/LICENSE.txt"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME "ImGui-LICENSE.txt"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/3rdparty/jsoncpp/LICENSE"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME "JsonCpp-LICENSE"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/3rdparty/yalantinglibs/LICENSE"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME "yalantinglibs-LICENSE"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/3rdparty/yalantinglibs/NOTICE"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME "yalantinglibs-NOTICE"
    )

    set(CPACK_PACKAGE_NAME "beacon")
    set(CPACK_PACKAGE_VENDOR "DHU-Minecraft")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Minecraft speedrun progress tracker")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    set(CPACK_GENERATOR "TGZ;ZIP")
    include(CPack)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
