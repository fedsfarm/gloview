cmake_minimum_required(VERSION 3.19)
project(gloview VERSION "0.3.0" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(WLR_USE_UNSTABLE)
# -fno-gnu-unique: C++ static-init guard vars (_ZGV...) otherwise emit as STB_GNU_UNIQUE,
# which makes glibc mark the .so NODELETE so dlclose never unmaps it — a same-path
# `hyprctl plugin load` after unload then returns the CACHED old image (same dlopen handle)
# and the hot-reload silently runs stale code. Plain symbols let dlclose truly unmap.
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fPIC -fno-gnu-unique)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPR_DEPS REQUIRED hyprland)
pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)
# Lua's pkg-config module name varies by distro: `lua` (Arch), `lua5.4`/`lua-5.4`
# (Debian/Nix), or `luajit` (what Hyprland itself links). Take the first that exists.
pkg_search_module(LUA REQUIRED lua lua5.4 lua-5.4 lua54 luajit)

add_library(gloview SHARED
  src/main.cpp
  src/overview.cpp
  src/preview_filter.cpp
  src/layout.cpp
)
set_target_properties(gloview PROPERTIES PREFIX "")  # produce gloview.so, not libgloview.so

target_include_directories(gloview PRIVATE ${HYPR_DEPS_INCLUDE_DIRS} ${LUA_INCLUDE_DIRS})
foreach(inc ${HYPR_DEPS_INCLUDE_DIRS})
  if(EXISTS "${inc}/hyprland/src")
    target_include_directories(gloview PRIVATE "${inc}/hyprland/src")
  endif()
endforeach()

target_link_directories(gloview PRIVATE ${HYPR_DEPS_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS})
target_link_libraries(gloview PRIVATE ${HYPR_DEPS_LIBRARIES} ${LUA_LIBRARIES} PkgConfig::GLESV2)

install(TARGETS gloview LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Dev hot-reload (official same-path unload;load):  cmake --build build --target reload
# This is the upstream-documented dev reload (Hyprland wiki, Plugins/Development):
#   hyprctl plugin unload /abs/plugin.so ; hyprctl plugin load /abs/plugin.so
# It ONLY picks up new code because of -fno-gnu-unique above: C++ static-init guard vars
# (_ZGV...) otherwise emit as STB_GNU_UNIQUE, which makes glibc mark the .so NODELETE so
# dlclose never unmaps it and the same-path load returns the CACHED old image (same dlopen
# handle) -> a silent stale reload. With the flag, dlclose truly unmaps and the same path
# re-mmaps the fresh build. (If you ever load a pre-flag binary, that image sticks NODELETE
# for the life of the Hyprland process and poisons its path until you restart Hyprland once.)
# `hyprctl gloviewunload` (Overview::hardClose) first does a synchronous, animation-free
# teardown + cancels the recapture timer so the unload can't free plugin-owned render/timer
# state while Hyprland still references it (-> SEGV / 150% IPC-dead spin); the 0.15s settle
# lets one plugin-free frame flush the render pass. A plugin crash kills the whole session —
# prefer a nested Hyprland for risky changes. (For installed plugins the user path is
# `hyprpm enable/disable <name>` + `hyprpm reload`.)
add_custom_target(reload
  COMMAND bash -c "hyprctl gloviewunload || true"
  COMMAND ${CMAKE_COMMAND} -E sleep 0.15
  COMMAND bash -c "hyprctl plugin unload $<TARGET_FILE:gloview> || true"
  # Also drop a hyprpm-installed copy: it autoloads at session start and already holds the
  # shouldRenderWindow hooks, so loading the build path next to it fails to hook (two
  # instances can't trampoline the same prologue). No-op when not installed/loaded.
  # ($ENV{USER} resolves at configure time — a literal $USER gets mangled by the Makefile.)
  COMMAND bash -c "hyprctl plugin unload /var/cache/hyprpm/$ENV{USER}/gloview/gloview.so || true"
  COMMAND hyprctl plugin load $<TARGET_FILE:gloview>
  DEPENDS gloview
  USES_TERMINAL
  COMMENT "Hot-reloading gloview into the running Hyprland"
)
