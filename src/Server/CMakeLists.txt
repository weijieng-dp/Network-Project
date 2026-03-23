cmake_minimum_required(VERSION 3.5)

#find files with extension of .cpp and .h
file(GLOB_RECURSE Engine_source_files CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.[ch]pp
    ${CMAKE_CURRENT_LIST_DIR}/*.h
)


#adding project
add_executable(Server
    ${Engine_source_files}
    #${imgui_source_files}
)



target_link_libraries(Server PRIVATE
   opengl32.lib
)
# Debug & Release DLLs

