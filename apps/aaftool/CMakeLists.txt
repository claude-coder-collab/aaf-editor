add_executable(aaftool main.cpp)
target_link_libraries(aaftool PRIVATE aaf::cfb aaf::project_options)
target_compile_definitions(aaftool PRIVATE AAF_VERSION="${PROJECT_VERSION}")
install(TARGETS aaftool)
