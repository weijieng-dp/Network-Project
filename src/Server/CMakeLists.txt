cmake_minimum_required(VERSION 3.5)

# Find all .cpp, .h, .hpp files in the src subdirectory
file(GLOB_RECURSE Server_source_files CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/src/*.[ch]pp
    ${CMAKE_CURRENT_LIST_DIR}/src/*.h
    ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp
)

add_executable(Server
    ${Server_source_files}
)

target_link_libraries(Server PRIVATE
   ws2_32
)
