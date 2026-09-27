add_library(aaf_common INTERFACE)
add_library(aaf::common ALIAS aaf_common)
target_include_directories(aaf_common INTERFACE include)
