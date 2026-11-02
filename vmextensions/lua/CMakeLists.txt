# luaproxy: liballocs-driven C interop for Lua (mirror of the CPython-side
# pycallocs extension in vmextensions/cpython). Standalone CMake project,
# deliberately not wired into the root pycallocs build.
#
#   cmake -S vmextensions/lua -B vmextensions/lua/build
#   cmake --build vmextensions/lua/build
#
# Two artifacts:
#   luaproxy.so - Lua C module for `require "luaproxy"` under the contrib/lua
#                 interpreter. Does NOT link liblua.a (non-PIC); Lua symbols
#                 resolve from the host interpreter, which contrib/lua's
#                 makefile links with -Wl,-E.
#   luahost     - embedded host (liblua.a + luaproxy statically), the primary
#                 dev/test driver:
#                 LD_PRELOAD=$LIBALLOCS/lib/liballocs_preload.so ./luahost x.lua
#
# Same location contract as setup.py: LIBALLOCS (cache var, with environment
# fallback), USE_ALASKA(+ALASKA*) for the Alaska configuration.

cmake_minimum_required(VERSION 3.16)
project(luaproxy C)

# The VM extensions are always built with the system compiler -- never with
# liballocs' allocscc nor alaska's clang (yukon); those apply to fixtures only.
if(CMAKE_C_COMPILER MATCHES "allocscc|alaska|yukon")
    message(FATAL_ERROR "luaproxy must be built with the system compiler, "
        "not '${CMAKE_C_COMPILER}' (allocscc/alaska/yukon are fixture-only toolchains)")
endif()

get_filename_component(PYCALLOCS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# --- location contract (cache var > environment > in-tree default) ----------
if(DEFINED ENV{LIBALLOCS})
    set(_liballocs_default "$ENV{LIBALLOCS}")
else()
    set(_liballocs_default "${PYCALLOCS_ROOT}/contrib/liballocs")
endif()
set(LIBALLOCS "${_liballocs_default}" CACHE PATH "liballocs checkout (built)")

set(LUA_SRC "${PYCALLOCS_ROOT}/contrib/lua" CACHE PATH "Lua source checkout (contrib/lua)")

option(USE_ALASKA "Build against the Alaska handle runtime (SS_HAVE_ALASKA)" OFF)

# The base-types provider meta-DSO (built by the main pycallocs build) supplies
# the __uniqtype__* scalar symbols. The module resolves them at load time from
# the preload; the executable needs them at link time.
set(BASETYPES_PROVIDER "${PYCALLOCS_ROOT}/build/basetypes/libbasetypes_provider.so"
    CACHE FILEPATH "basetypes provider meta-DSO from the main build")

# --- common compile settings -------------------------------------------------
add_library(luaproxy_common INTERFACE)
target_include_directories(luaproxy_common INTERFACE
    "${LUA_SRC}"
    "${PYCALLOCS_ROOT}/vmextensions/common"
    "${LIBALLOCS}/include"
    "${LIBALLOCS}/contrib/libsystrap/contrib/librunt/include"
    "${LIBALLOCS}/contrib/liballocstool/include"
)
target_compile_definitions(luaproxy_common INTERFACE LIFETIME_POLICIES)
target_compile_options(luaproxy_common INTERFACE
    -std=gnu11 -gdwarf-4 -Wall -Wextra -Wno-unused-parameter)

if(USE_ALASKA)
    set(ALASKA "${PYCALLOCS_ROOT}/contrib/alaska" CACHE PATH "Alaska checkout")
    set(ALASKA_OPT_TREE "alaska-anchorage" CACHE STRING "Alaska opt/ tree to link")
    target_compile_definitions(luaproxy_common INTERFACE SS_HAVE_ALASKA)
    target_link_directories(luaproxy_common INTERFACE
        "${ALASKA}/opt/${ALASKA_OPT_TREE}/lib")
endif()

set(LUAPROXY_SOURCES
    luaproxy.c
    foreign_type.c
    foreign_basetype.c
    composite_proxy.c
    address_proxy.c
    function_proxy.c
    library_loader.c
    proxy.c
    # Shared VM-agnostic code (also compiled into the CPython extension).
    "${PYCALLOCS_ROOT}/vmextensions/common/minicrunch.c"
    "${PYCALLOCS_ROOT}/vmextensions/common/dlsym_iter.c"
    "${PYCALLOCS_ROOT}/vmextensions/common/ffitypes.c"
)

# Compiled once, shared by the module and the host; must be PIC for the module.
add_library(luaproxy_objs OBJECT ${LUAPROXY_SOURCES})
set_target_properties(luaproxy_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(luaproxy_objs PRIVATE luaproxy_common)

# --- liblua.a (contrib/lua developer makefile; prebuilt in-tree) -------------
add_custom_command(OUTPUT "${LUA_SRC}/liblua.a"
    COMMAND make -C "${LUA_SRC}"
    COMMENT "Building contrib/lua (liblua.a)")
add_custom_target(liblua DEPENDS "${LUA_SRC}/liblua.a")

# --- luaproxy.so: the require-able C module ----------------------------------
# Lua API symbols and the liballocs runtime symbols stay undefined: they
# resolve at load time from the -Wl,-E host interpreter and the LD_PRELOADed
# liballocs_preload.so, exactly like the Python `allocs` extension.
add_library(luaproxy MODULE $<TARGET_OBJECTS:luaproxy_objs>)
set_target_properties(luaproxy PROPERTIES PREFIX "" OUTPUT_NAME "luaproxy")
target_link_libraries(luaproxy PRIVATE luaproxy_common ffi dl m)
target_link_directories(luaproxy PRIVATE "${LIBALLOCS}/lib")
target_link_libraries(luaproxy PRIVATE allocs)
set_target_properties(luaproxy PROPERTIES
    BUILD_RPATH "${LIBALLOCS}/lib"
    SKIP_BUILD_RPATH OFF)
if(USE_ALASKA)
    target_link_libraries(luaproxy PRIVATE alaska)
endif()

# --- luahost: embedded dev/test driver ---------------------------------------
# An executable must resolve everything at link time. The lifetime-/GC-policy
# API is exported by liballocs_preload.so only (not liballocs.so), and the
# instrumentation counters by liballocs_dummyweaks.so; at run time the
# LD_PRELOADed preload copy is the same DSO, so interposition still works.
# -Wl,-E re-exports the static Lua API for C modules loaded via require.
add_executable(luahost luahost.c $<TARGET_OBJECTS:luaproxy_objs>)
add_dependencies(luahost liblua)
target_link_libraries(luahost PRIVATE luaproxy_common
    "${LUA_SRC}/liblua.a"
    "${LIBALLOCS}/lib/liballocs_preload.so"
    "${LIBALLOCS}/lib/liballocs_dummyweaks.so"
    "${BASETYPES_PROVIDER}"
    ffi dl m)
target_link_options(luahost PRIVATE -Wl,-E)
set_target_properties(luahost PROPERTIES
    BUILD_RPATH "${LIBALLOCS}/lib")
if(USE_ALASKA)
    target_link_libraries(luahost PRIVATE alaska)
endif()

# --- sanity check: the module must keep Lua core symbols undefined -----------
add_custom_target(check-symbols
    COMMAND sh -c "nm -u $<TARGET_FILE:luaproxy> | grep -q ' lua_' && echo 'OK: Lua symbols undefined (resolve from host)' || { echo 'FAIL: no undefined lua_* symbols?'; exit 1; }"
    COMMAND sh -c "! nm --defined-only $<TARGET_FILE:luaproxy> | grep -q ' T lua_' && echo 'OK: no Lua core symbols defined in luaproxy.so' || { echo 'FAIL: luaproxy.so defines Lua core symbols'; exit 1; }"
    DEPENDS luaproxy
    VERBATIM)
