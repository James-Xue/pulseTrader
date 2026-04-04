# compiler_flags.cmake — strict C++20 warnings for all targets
#
# Applied globally via add_compile_options so every target in the project
# inherits these flags without needing to opt in explicitly.

if(MSVC)
    add_compile_options(
        /W4         # High warning level
        /WX         # Treat warnings as errors
        /permissive- # Strict standard conformance
        /Zc:__cplusplus  # Report correct __cplusplus value
    )
else()
    # GCC and Clang
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wno-unused-parameter   # Allow unused params in virtual overrides
        -Wno-missing-field-initializers
    )

    # Extra hardening in release builds
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O2 -DNDEBUG)
    endif()
endif()
