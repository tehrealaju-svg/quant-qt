cmake_minimum_required(VERSION 3.16)
project(quant_qt VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# quant-core: git submodule in external/quant-core, or a sibling checkout for development.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/quant-core/CMakeLists.txt)
  set(QUANT_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/quant-core)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../quant-core/CMakeLists.txt)
  set(QUANT_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../quant-core)
else()
  message(FATAL_ERROR "quant-core not found: run 'git submodule update --init'")
endif()
set(QUANT_BUILD_DAEMON ON CACHE BOOL "" FORCE)
set(QUANT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${QUANT_CORE_DIR} quant-core)

# GLFW
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)

# Dear ImGui
set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_glfw.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
target_link_libraries(imgui PUBLIC glfw)
find_package(OpenGL REQUIRED)
target_link_libraries(imgui PUBLIC OpenGL::GL)

add_library(qrcodegen STATIC third_party/qrcodegen/qrcodegen.cpp)
target_include_directories(qrcodegen PUBLIC third_party/qrcodegen)

file(GLOB QT_SRC CONFIGURE_DEPENDS src/*.cpp)
set(QT_EXTRA "")
if(WIN32)
  set(QT_EXTRA src/quant.rc)
endif()
add_executable(quant-qt WIN32 ${QT_SRC} ${QT_EXTRA})
target_link_libraries(quant-qt PRIVATE quant imgui qrcodegen)
if(WIN32)
  target_link_options(quant-qt PRIVATE -static)
endif()
