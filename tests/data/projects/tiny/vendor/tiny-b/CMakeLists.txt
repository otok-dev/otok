cmake_minimum_required(VERSION 3.18)
project(tiny_b C)
# tiny-a is found through CMAKE_PREFIX_PATH, which otok.cmake sets to the prefixes of dependencies
find_path(TINY_A_INCLUDE tiny_a.h REQUIRED)
add_library(tiny_b STATIC tiny_b.c)
target_include_directories(tiny_b PRIVATE ${TINY_A_INCLUDE})
install(TARGETS tiny_b ARCHIVE DESTINATION lib)
install(FILES tiny_b.h DESTINATION include)
