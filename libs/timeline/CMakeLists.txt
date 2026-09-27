add_library(aaf_timeline
    src/projection.cpp
    src/rational.cpp
)
add_library(aaf::timeline ALIAS aaf_timeline)
target_include_directories(aaf_timeline PUBLIC include)
target_link_libraries(aaf_timeline PUBLIC aaf::core PRIVATE aaf::project_options)
