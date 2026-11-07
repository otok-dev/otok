cmake_minimum_required(VERSION 3.18...4.0)  # CMake 4 policies: its own MSVC runtime checks and debug format
project(tiny_d C)
add_library(tiny_d tiny_d.c)
# Windows exports through the import library; elsewhere a versioned name with links (libtiny_d.so.1)
set_target_properties(tiny_d PROPERTIES VERSION 1.0 SOVERSION 1 WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_compile_definitions(tiny_d PRIVATE $<IF:$<CONFIG:Debug>,TINY_D_VALUE=2,TINY_D_VALUE=1>)
install(TARGETS tiny_d ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES tiny_d.h DESTINATION include)
