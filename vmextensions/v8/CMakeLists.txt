# v8proxy: liballocs-driven C interop for JavaScript, as a Node.js native
# addon (raw V8 API). Third sibling of vmextensions/cpython and
# vmextensions/lua. Standalone CMake project, deliberately not wired into the
# root pycallocs build:
#
#   cmake -S vmextensions/v8 -B vmextensions/v8/build
#   cmake --build vmextensions/v8/build
#
# Product: v8proxy.node -- loaded with require() into the system `node`
# (which IS the host; V8/node symbols stay undefined in the addon and resolve
# from the node executable at load time, the same discipline as luaproxy.so
# against the -Wl,-E Lua interpreter). liballocs enters via LD_PRELOAD.
#
# Same location contract as the lua extension: LIBALLOCS cache var with
# environment fallback; NODE_HEADERS overrides the node header dir.

cmake_minimum_required(VERSION 3.16)
project(v8proxy CXX C)

# The VM extensions are always built with the system compiler -- never with
# liballocs' allocscc nor alaska's clang (yukon); those apply to fixtures only.
if(CMAKE_C_COMPILER MATCHES "allocscc|alaska|yukon"
        OR CMAKE_CXX_COMPILER MATCHES "allocscc|alaska|yukon")
    message(FATAL_ERROR "v8proxy must be built with the system compiler, "
        "not '${CMAKE_C_COMPILER}'/'${CMAKE_CXX_COMPILER}' "
        "(allocscc/alaska/yukon are fixture-only toolchains)")
endif()

get_filename_component(PYCALLOCS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(DEFINED ENV{LIBALLOCS})
    set(_liballocs_default "$ENV{LIBALLOCS}")
else()
    set(_liballocs_default "${PYCALLOCS_ROOT}/contrib/liballocs")
endif()
set(LIBALLOCS "${_liballocs_default}" CACHE PATH "liballocs checkout (built)")

# --- node headers ------------------------------------------------------------
# Preference order: the node install's own include/node (nvm/tarball installs
# ship full headers incl. config.gypi), then the node-gyp download cache.
find_program(NODE_EXECUTABLE node REQUIRED)
execute_process(COMMAND "${NODE_EXECUTABLE}" -p "process.version.slice(1)"
    OUTPUT_VARIABLE _node_version OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${NODE_EXECUTABLE}" -p
    "require('path').resolve(process.execPath, '..', '..', 'include', 'node')"
    OUTPUT_VARIABLE _node_prefix_headers OUTPUT_STRIP_TRAILING_WHITESPACE)
if(EXISTS "${_node_prefix_headers}/node.h")
    set(_node_headers_default "${_node_prefix_headers}")
else()
    set(_node_headers_default "$ENV{HOME}/.cache/node-gyp/${_node_version}/include/node")
endif()
set(NODE_HEADERS "${_node_headers_default}"
    CACHE PATH "node development headers")
if(NOT EXISTS "${NODE_HEADERS}/node.h")
    message(FATAL_ERROR "node headers not found at ${NODE_HEADERS} "
        "(install headers, run `npx node-gyp install`, or set NODE_HEADERS)")
endif()

# --- ABI guard ---------------------------------------------------------------
# This addon is compiled with ZERO V8 layout defines, which is only correct
# while node's own V8 build has all of these flags off (verified for node
# 24.2.0). A node built with any of them on would silently corrupt object
# layouts, so fail the configure instead.
foreach(_flag v8_enable_pointer_compression v8_enable_sandbox
        v8_enable_31bit_smis_on_64bit_arch v8_enable_external_code_space)
    file(STRINGS "${NODE_HEADERS}/config.gypi" _line REGEX "'${_flag}':")
    if(NOT _line MATCHES "'${_flag}': *0")
        message(FATAL_ERROR "node's ${_flag} is not 0 (got: ${_line}); "
            "v8proxy must then be compiled with the matching V8 layout "
            "defines -- see the ABI guard section of vmextensions/v8/CMakeLists.txt")
    endif()
endforeach()

set(V8PROXY_SOURCES
    v8proxy.cc
    proxy.cc
    foreign_type.cc
    foreign_basetype.cc
    composite_proxy.cc
    address_proxy.cc
    function_proxy.cc
    library_loader.cc
)

set(VMEXT_COMMON_SOURCES
    "${PYCALLOCS_ROOT}/vmextensions/common/minicrunch.c"
    "${PYCALLOCS_ROOT}/vmextensions/common/dlsym_iter.c"
    "${PYCALLOCS_ROOT}/vmextensions/common/ffitypes.c"
)

add_library(v8proxy MODULE ${V8PROXY_SOURCES} ${VMEXT_COMMON_SOURCES})
set_target_properties(v8proxy PROPERTIES
    PREFIX "" SUFFIX ".node" OUTPUT_NAME "v8proxy"
    POSITION_INDEPENDENT_CODE ON
    BUILD_RPATH "${LIBALLOCS}/lib")
target_compile_features(v8proxy PRIVATE cxx_std_20)
# -fpermissive: liballocs' malloc-meta.h defines structs inside an anonymous
# union, which is ill-formed C++ (fine C); gcc downgrades it to a warning
# under -fpermissive. Scoped to C++ only (the C sources stay strict).
target_compile_options(v8proxy PRIVATE
    -gdwarf-4 -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fpermissive>)
target_compile_definitions(v8proxy PRIVATE
    BUILDING_NODE_EXTENSION NODE_GYP_MODULE_NAME=v8proxy LIFETIME_POLICIES)
target_include_directories(v8proxy PRIVATE
    "${NODE_HEADERS}"
    "${PYCALLOCS_ROOT}/vmextensions/common"
    "${LIBALLOCS}/include"
    "${LIBALLOCS}/contrib/libsystrap/contrib/librunt/include"
    "${LIBALLOCS}/contrib/liballocstool/include")
# The __uniqtype__* scalar symbols referenced by minicrunch and the base-type
# tables are DATA symbols, so the dynamic loader binds them eagerly at
# require() time even without the liballocs preload -- link the provider
# directly (same DSO as the preloaded copy at test time; luahost discipline).
set(BASETYPES_PROVIDER "${PYCALLOCS_ROOT}/build/basetypes/libbasetypes_provider.so"
    CACHE FILEPATH "basetypes provider meta-DSO from the main build")
target_link_directories(v8proxy PRIVATE "${LIBALLOCS}/lib")
target_link_libraries(v8proxy PRIVATE allocs "${BASETYPES_PROVIDER}" ffi dl m)

# The addon must keep V8/node symbols undefined (resolved from the node
# executable at require() time).
add_custom_target(check-symbols
    COMMAND sh -c "nm -u $<TARGET_FILE:v8proxy> | grep -q '_ZN2v8' && echo 'OK: V8 symbols undefined (resolve from node)' || { echo 'FAIL: no undefined v8 symbols?'; exit 1; }"
    COMMAND sh -c "! nm --defined-only $<TARGET_FILE:v8proxy> | grep -q ' T _ZN2v8' && echo 'OK: no V8 symbols defined in v8proxy.node' || { echo 'FAIL: v8proxy.node defines V8 symbols'; exit 1; }"
    DEPENDS v8proxy
    VERBATIM)
