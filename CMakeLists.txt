cmake_minimum_required(VERSION 3.24)
project(BlackHole LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Dependencies: downloaded automatically the first time you configure.
# (No vcpkg or Git needed - CMake fetches the release zips directly.)
# ---------------------------------------------------------------------------
include(FetchContent)
# Lets older dependency CMake files configure under CMake 4.x
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND  OFF CACHE BOOL "" FORCE)  # X11 only: needs fewer packages, runs under XWayland
set(GLM_BUILD_LIBRARY   OFF CACHE BOOL "" FORCE)  # GLM is used header-only

# Link the C++ runtime statically, so the release .exe runs on PCs without the VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
  URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(glm
  URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(glfw glm)

# GLAD (OpenGL 4.3 core loader) is generated and kept in external/glad
add_library(glad STATIC external/glad/src/gl.c)
target_include_directories(glad PUBLIC external/glad/include)

# ---------------------------------------------------------------------------
add_executable(black_hole
  src/main.cpp
  src/shader.cpp)
target_include_directories(black_hole PRIVATE src)
target_link_libraries(black_hole PRIVATE glfw glm::glm glad)

if(MSVC)
  target_compile_options(black_hole PRIVATE /W3 /utf-8)
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT black_hole)
else()
  target_compile_options(black_hole PRIVATE -Wall -Wextra)
endif()

# Copy the shaders next to the executable on every build, so the program
# finds them at runtime. You can also edit them there and press R to reload.
add_custom_target(copy_shaders ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:black_hole>/shaders
  COMMENT "Copying shaders")
add_dependencies(black_hole copy_shaders)
