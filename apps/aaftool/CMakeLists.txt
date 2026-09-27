include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(aaftool main.cpp dump.cpp)
target_link_libraries(aaftool PRIVATE aaf::core aaf::project_options nlohmann_json::nlohmann_json)
target_compile_definitions(aaftool PRIVATE AAF_VERSION="${PROJECT_VERSION}")
install(TARGETS aaftool)
