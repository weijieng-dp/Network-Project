cmake_minimum_required(VERSION 3.5)

# Find all .cpp, .h, .hpp files in the src subdirectory
file(GLOB_RECURSE Client_source_files CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/src/*.[ch]pp
    ${CMAKE_CURRENT_LIST_DIR}/src/*.h
    ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp
)

add_executable(Client
    ${Client_source_files}
)

target_link_libraries(Client PRIVATE
   ws2_32
)
