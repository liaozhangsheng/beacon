find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)

file(GLOB_RECURSE PROJECT_FORMAT_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/test/*.cpp"
)

if(CLANG_FORMAT_EXECUTABLE AND PROJECT_FORMAT_SOURCES)
    add_custom_target(format
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${PROJECT_FORMAT_SOURCES}
        COMMENT "Formatting C/C++ source files"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${PROJECT_FORMAT_SOURCES}
        COMMENT "Checking C/C++ formatting"
        VERBATIM
    )
else()
    add_custom_target(format
        COMMAND "${CMAKE_COMMAND}" -E echo "clang-format was not found or no source files were available"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND "${CMAKE_COMMAND}" -E echo "clang-format was not found or no source files were available"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM
    )
endif()
