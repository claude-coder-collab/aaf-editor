add_library(aaf_rpc
    src/json_value.cpp
    src/json_writer.cpp
    src/server.cpp
)
add_library(aaf::rpc ALIAS aaf_rpc)
target_include_directories(aaf_rpc PUBLIC include)
target_link_libraries(aaf_rpc PUBLIC aaf::edit aaf::timeline nlohmann_json::nlohmann_json PRIVATE aaf::project_options)
