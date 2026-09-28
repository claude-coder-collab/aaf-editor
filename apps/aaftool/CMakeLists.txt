add_executable(aaftool main.cpp dump.cpp)
target_link_libraries(aaftool PRIVATE aaf::rpc aaf::project_options nlohmann_json::nlohmann_json)
target_compile_definitions(aaftool PRIVATE AAF_VERSION="${AAF_VERSION}")
install(TARGETS aaftool)
