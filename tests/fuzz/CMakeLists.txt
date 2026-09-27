if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AAF_BUILD_FUZZERS requires Clang")
endif()

function(aaf_add_fuzzer name library)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${library} aaf::project_options)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

aaf_add_fuzzer(fuzz_cfb aaf::cfb)
aaf_add_fuzzer(fuzz_document aaf::core)
