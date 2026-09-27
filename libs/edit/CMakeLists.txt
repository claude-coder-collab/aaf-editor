add_library(aaf_edit
    src/operations.cpp
    src/session.cpp
    src/transaction.cpp
)
add_library(aaf::edit ALIAS aaf_edit)
target_include_directories(aaf_edit PUBLIC include)
target_link_libraries(aaf_edit PUBLIC aaf::core PRIVATE aaf::project_options)
