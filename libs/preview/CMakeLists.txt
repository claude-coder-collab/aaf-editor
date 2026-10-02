add_library(aaf_preview src/preview.cpp)
add_library(aaf::preview ALIAS aaf_preview)
target_include_directories(aaf_preview PUBLIC include)
target_link_libraries(aaf_preview PUBLIC aaf::core PRIVATE aaf::timeline aaf::project_options)
