# Minimum CMake version required, we'll just use the latest version.
cmake_minimum_required(VERSION 3.22)
# Project name, version and description
project(dppsimplify VERSION 1.0 DESCRIPTION "Simplify discord bot in c++")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Create an executable
add_executable(${PROJECT_NAME}
        src/main.cpp
        src/forwarder.cpp
        src/forwarder.h
        src/job_watcher.cpp
        src/job_watcher.h
        src/webhook_queue.cpp
        src/webhook_queue.h
)

# Find our pre-installed DPP package (using FindDPP.cmake).
find_package(DPP REQUIRED)

# Link the pre-installed DPP package.
target_link_libraries(${PROJECT_NAME}
        ${DPP_LIBRARIES}
)

# Include the DPP directories.
target_include_directories(${PROJECT_NAME} PRIVATE
        ${DPP_INCLUDE_DIR}
)

# Absolute path to config.properties, so it's found regardless of the working directory.
target_compile_definitions(${PROJECT_NAME} PRIVATE
        CONFIG_PATH="${CMAKE_SOURCE_DIR}/config.properties"
)

# Set C++ version
set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
)

# On Linux, also look for libdpp.so next to the executable, so a release bundle
# (binary + libdpp.so in one folder) runs without D++ installed system-wide.
if(UNIX AND NOT APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES BUILD_RPATH "\$ORIGIN")
endif()