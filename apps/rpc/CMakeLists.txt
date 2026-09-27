add_library(aaf_rpc
    src/json_value.cpp
    src/server.cpp
)
add_library(aaf::rpc ALIAS aaf_rpc)
target_include_directories(aaf_rpc PUBLIC include)
target_link_libraries(aaf_rpc PUBLIC aaf::edit nlohmann_json::nlohmann_json PRIVATE aaf::project_options)
