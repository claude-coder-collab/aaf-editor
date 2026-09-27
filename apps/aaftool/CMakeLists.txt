add_executable(aaftool main.cpp dump.cpp)
target_link_libraries(aaftool PRIVATE aaf::edit aaf::project_options nlohmann_json::nlohmann_json)
target_compile_definitions(aaftool PRIVATE AAF_VERSION="${PROJECT_VERSION}")
install(TARGETS aaftool)
