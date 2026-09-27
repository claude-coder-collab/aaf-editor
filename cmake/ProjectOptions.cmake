option(AAF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(AAF_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(AAF_BUILD_TESTS "Build unit tests" ON)
option(AAF_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)

add_library(aaf_project_options INTERFACE)
add_library(aaf::project_options ALIAS aaf_project_options)
target_compile_features(aaf_project_options INTERFACE cxx_std_26)

if(MSVC)
    target_compile_options(aaf_project_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    if(AAF_WARNINGS_AS_ERRORS)
        target_compile_options(aaf_project_options INTERFACE /WX)
    endif()
else()
    target_compile_options(aaf_project_options INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
        -Wcast-align -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wundef)
    if(AAF_WARNINGS_AS_ERRORS)
        target_compile_options(aaf_project_options INTERFACE -Werror)
    endif()
    if(AAF_ENABLE_SANITIZERS)
        target_compile_options(aaf_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(aaf_project_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

if(AAF_BUILD_FUZZERS)
    target_compile_options(aaf_project_options INTERFACE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    target_link_options(aaf_project_options INTERFACE -fsanitize=address,undefined)
endif()
