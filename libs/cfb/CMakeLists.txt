add_library(aaf_cfb
    src/builder.cpp
    src/byte_io.cpp
    src/container.cpp
    src/types.cpp
)
add_library(aaf::cfb ALIAS aaf_cfb)
target_include_directories(aaf_cfb PUBLIC include)
target_link_libraries(aaf_cfb PUBLIC aaf::common PRIVATE aaf::project_options)
