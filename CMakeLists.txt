cmake_minimum_required(VERSION 3.22)
project(esp CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

set(IMGUI third_party/imgui)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.cpp)
  set(MAIN_SRC src/main.cpp)
else()
  set(MAIN_SRC main.cpp)
endif()

file(GLOB_RECURSE A64_SRC third_party/And64InlineHook/*And64InlineHook.c*)
list(GET A64_SRC 0 A64_MAIN)
get_filename_component(A64_DIR ${A64_MAIN} DIRECTORY)

add_library(esp SHARED
  ${MAIN_SRC} ${A64_MAIN}
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_opengl3.cpp)
target_include_directories(esp PRIVATE ${IMGUI} ${IMGUI}/backends ${A64_DIR})
target_compile_definitions(esp PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(esp PRIVATE log EGL GLESv3 android dl)
target_link_options(esp PRIVATE -Wl,--exclude-libs,ALL)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/injector.cpp)
  set(INJ_SRC src/injector.cpp)
else()
  set(INJ_SRC injector.cpp)
endif()
add_executable(injector ${INJ_SRC})
target_link_libraries(injector PRIVATE dl)
