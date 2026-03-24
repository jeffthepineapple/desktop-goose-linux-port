cmake_minimum_required(VERSION 3.17)
project(CppGoose)

set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(LS REQUIRED gtk4-layer-shell-0)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2MIXER REQUIRED SDL2_mixer)
pkg_check_modules(GDKPIXBUF REQUIRED gdk-pixbuf-2.0)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)

find_program(WAYLAND_SCANNER NAMES wayland-scanner)

# Wayland code generation
function(wayland_generate protocol_file out_header out_code)
    get_filename_component(protocol_name ${protocol_file} NAME_WE)
    add_custom_command(
        OUTPUT ${out_header}
        COMMAND ${WAYLAND_SCANNER} client-header ${protocol_file} ${out_header}
        DEPENDS ${protocol_file}
    )
    add_custom_command(
        OUTPUT ${out_code}
        COMMAND ${WAYLAND_SCANNER} private-code ${protocol_file} ${out_code}
        DEPENDS ${protocol_file}
    )
endfunction()

set(VIRTUAL_POINTER_PROTOCOL "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-virtual-pointer-unstable-v1.xml")
wayland_generate(${VIRTUAL_POINTER_PROTOCOL}
    "${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-client-protocol.h"
    "${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-protocol.c")

set(GENERATED_SOURCES
    "${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-protocol.c"
    "${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-client-protocol.h"
)

file(GLOB_RECURSE SRC_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB MAIN_SOURCES "main.cpp")
set(SOURCES ${SRC_SOURCES} ${MAIN_SOURCES} ${GENERATED_SOURCES})



find_package(X11 REQUIRED)
# Xtst is often separate, sometimes part of X11 variables depending on cmake version,
# but usually we check for it explicitly if FindX11 doesn't automatically give it targets.
# FindX11 usually sets X11_XTest_FOUND if requested components? No, simpler to just check lib.
if(X11_FOUND)
    message(STATUS "X11 Found")
    if (NOT X11_XTest_FOUND)
        # Try finding it manually if CMake's module didn't (older cmake)
        find_library(X11_XTest_LIB Xtst ${X11_LIBRARY_DIR})
        if (X11_XTest_LIB)
            set(X11_XTest_FOUND TRUE)
            message(STATUS "Xtst Found at ${X11_XTest_LIB}")
        endif()
    endif()
endif()

add_executable(CppGoose ${SOURCES})

target_link_libraries(CppGoose
    ${GTK4_LIBRARIES}
    ${LS_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${SDL2MIXER_LIBRARIES}
    ${GDKPIXBUF_LIBRARIES}
    ${X11_LIBRARIES}
    ${X11_XTest_LIB}
    ${WAYLAND_CLIENT_LIBRARIES}
    m
    stdc++fs
)

target_include_directories(CppGoose PUBLIC
    ${GTK4_INCLUDE_DIRS}
    ${LS_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${SDL2MIXER_INCLUDE_DIRS}
    ${GDKPIXBUF_INCLUDE_DIRS}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_options(CppGoose PUBLIC
    ${GTK4_CFLAGS_OTHER}
    ${LS_CFLAGS_OTHER}
    ${SDL2_CFLAGS_OTHER}
    ${SDL2MIXER_CFLAGS_OTHER}
    ${GDKPIXBUF_CFLAGS_OTHER}
    ${WAYLAND_CLIENT_CFLAGS_OTHER}
)
