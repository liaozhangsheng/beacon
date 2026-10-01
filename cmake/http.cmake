# Shared HTTPS support for the desktop profile loader and the updater.
set(YLT_ENABLE_SSL ON CACHE BOOL "" FORCE)
find_package(OpenSSL REQUIRED)
add_subdirectory("${PROJECT_SOURCE_DIR}/3rdparty/yalantinglibs" EXCLUDE_FROM_ALL)

beacon_patch(3rdparty/yalantinglibs/include/ylt/standalone/cinatra/coro_http_client.hpp
    cinatra/coro_http_client.hpp cinatra-client.patch)

add_library(beacon_http STATIC src/http/client.cpp src/http/trust.cpp)
target_compile_definitions(beacon_http PRIVATE CINATRA_LOG_TRACE=cinatra::NULL_LOGGER)
target_include_directories(beacon_http PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_include_directories(beacon_http BEFORE PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/thirdparty"
    "${PROJECT_SOURCE_DIR}/3rdparty/yalantinglibs/include/ylt/standalone/cinatra"
    "${PROJECT_SOURCE_DIR}/cmake/compat"
)
target_link_libraries(beacon_http
    PUBLIC beacon_core OpenSSL::SSL OpenSSL::Crypto
    PRIVATE yalantinglibs::yalantinglibs
)
if(WIN32)
    target_link_libraries(beacon_http PRIVATE crypt32 ws2_32 dbghelp)
elseif(APPLE)
    target_link_libraries(beacon_http PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND YLT_ENABLE_IBV)
    find_path(BEACON_NUMA_INCLUDE_DIR numa.h REQUIRED)
    find_library(BEACON_NUMA_LIBRARY numa REQUIRED)
    target_include_directories(beacon_http PRIVATE ${BEACON_NUMA_INCLUDE_DIR})
    target_link_libraries(beacon_http PRIVATE ${BEACON_NUMA_LIBRARY})
endif()
beacon_warnings(beacon_http)
