include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.16.0
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(aaf_tests
    cfb/test_builder.cpp
    cfb/test_container.cpp
    cfb/test_corruption.cpp
    cfb/test_fixtures.cpp
    cfb/test_types.cpp
    core/test_auid.cpp
    core/test_document.cpp
    core/test_metamodel.cpp
    core/test_value.cpp
    core/test_writer.cpp
    edit/test_session.cpp
    support/fixtures.cpp
)
target_include_directories(aaf_tests PRIVATE support)
target_link_libraries(aaf_tests PRIVATE aaf::edit aaf::project_options Catch2::Catch2WithMain)
target_compile_definitions(aaf_tests PRIVATE AAF_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
catch_discover_tests(aaf_tests)
