add_library(aaf_core
    src/auid.cpp
    src/document.cpp
    src/generated/baseline_model.cpp
    src/metamodel.cpp
    src/validate.cpp
    src/value.cpp
    src/writer.cpp
)
add_library(aaf::core ALIAS aaf_core)
target_include_directories(aaf_core PUBLIC include)
target_link_libraries(aaf_core PUBLIC aaf::cfb PRIVATE aaf::project_options)
