cmake_minimum_required(VERSION 3.15)

# ==========================================
# Setup vcpkg as an embedded dependency manager
# ==========================================
set(VCPKG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg")
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "Vcpkg toolchain file")

# If vcpkg is present, force its toolchain
if(EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file" FORCE)
    message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
else()
    message(WARNING "vcpkg not found in ${VCPKG_ROOT}. Did you clone with --recurse-submodules?")
endif()

# ==========================================
# Project definition
# ==========================================
project(trading-platform)

set(CMAKE_CXX_STANDARD 20)

add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP>)

# Global defines
add_definitions(-DNOMINMAX)
add_definitions(-D_CRT_SECURE_NO_WARNINGS)

if (MSVC)
    add_compile_options(/W4)
endif()

# ==========================================
# Auto-bootstrap vcpkg
# ==========================================
if(EXISTS "${VCPKG_ROOT}" AND NOT EXISTS "${VCPKG_ROOT}/vcpkg.exe")
    message(STATUS "Bootstrapping vcpkg...")
    if(WIN32)
        execute_process(COMMAND "${VCPKG_ROOT}/bootstrap-vcpkg.bat" "-disableMetrics")
    else()
        execute_process(COMMAND "${VCPKG_ROOT}/bootstrap-vcpkg.sh" "-disableMetrics")
    endif()
endif()

# ==========================================
# Find packages (vcpkg-managed)
# ==========================================
find_package(glew CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(rapidjson CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)

# ==========================================
# Subprojects
# ==========================================
add_subdirectory(src/Server)
add_subdirectory(src/Client)

# ==========================================
# Server link libraries
# ==========================================
target_link_libraries(Server
    PRIVATE
	GLEW::GLEW
	glfw
	imgui::imgui
	glm::glm
	implot::implot
	ws2_32
)

target_include_directories(Server PRIVATE
${RAPIDJSON_INCLUDE_DIRS}
)

# ==========================================
# Client link libraries
# ==========================================
target_link_libraries(Client
    PRIVATE
	GLEW::GLEW
	glfw
	imgui::imgui
	glm::glm
	implot::implot
	ftxui::screen
	ftxui::dom
	ftxui::component
	ws2_32
)

target_include_directories(Server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Shared
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Server/Header
)

target_include_directories(Client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Shared
)
target_include_directories(Client PRIVATE
${RAPIDJSON_INCLUDE_DIRS}
)

# After defining all executables/libraries
#set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT )

# ==========================================
# Organize IDE filters automatically
# ==========================================
function(set_source_groups target_name)
    get_target_property(target_sources ${target_name} SOURCES)
    foreach(source ${target_sources})
        get_filename_component(source_path "${source}" PATH)
        file(RELATIVE_PATH rel_source_path ${CMAKE_SOURCE_DIR} ${source_path})
        if(rel_source_path)
            string(REPLACE "\\" "/" source_group_path "${rel_source_path}")
            source_group("${source_group_path}" FILES "${source}")
        endif()
    endforeach()
endfunction()
