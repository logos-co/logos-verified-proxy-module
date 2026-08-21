cmake_minimum_required(VERSION 3.14)
project(VerifiedProxyModulePlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# Universal module — generated_code/ is picked up automatically.
logos_module(
    NAME verified_proxy_module
    SOURCES
        src/verified_proxy_impl.h
        src/verified_proxy_impl.cpp
        src/proxy_config.h
        src/proxy_config.cpp
        src/proxy_runtime.h
        src/proxy_runtime.cpp
    EXTERNAL_LIBS
        verifproxy
    INCLUDE_DIRS
        lib
)

# Inject the module version from metadata.json so moduleVersion() reports it
# without parsing JSON at runtime.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" _vp_metadata_json)
string(JSON VERIFIED_PROXY_MODULE_VERSION GET "${_vp_metadata_json}" version)
target_compile_definitions(verified_proxy_module_module_plugin
    PRIVATE VERIFIED_PROXY_MODULE_VERSION="${VERIFIED_PROXY_MODULE_VERSION}")

# Native libraries libverifproxy.a leaves undefined. Copied from nimbus-eth1's
# own consumer recipe (Makefile VERIFPROXY_LDFLAGS), whose `libverifproxy_test`
# target links `-lverifproxy` PLAIN — no --whole-archive / -force_load — and
# then RUNS the result. That is the upstream proof that ordinary archive
# linking suffices here.
#
# These go here and NOT in metadata.json's nix.cmake.extra_link_libraries:
# that key is parsed (parseMetadata.nix:97) and read by no builder code path,
# so putting flags there links nothing at all.
if(APPLE)
    # -framework Security is load-bearing AND cannot be inferred: LogosModule
    # puts `-undefined dynamic_lookup` on every macOS plugin, so omitting it
    # yields a plugin that LINKS CLEANLY and then fails at dlopen.
    # No explicit -lc++: CMake already links the C++ runtime for a CXX target,
    # and naming it again only produces a "duplicate libraries" warning.
    target_link_libraries(verified_proxy_module_module_plugin PRIVATE
        "-framework Security")
elseif(WIN32)
    # Upstream says -lc++ because their dist build uses llvm-mingw; logos-nix's
    # cross is gcc-mingw, so it is libstdc++ here. A static Nim archive records
    # no imports, so the plugin must name the Win32 set itself.
    target_link_libraries(verified_proxy_module_module_plugin PRIVATE
        stdc++ ws2_32 bcrypt iphlpapi userenv ntdll dbghelp winpthread)
else()
    target_link_libraries(verified_proxy_module_module_plugin PRIVATE stdc++ m)
    # Do not re-export anything the archive carries (bearssl, secp256k1, blst,
    # a vendored zlib/sqlite) into the host's global namespace.
    target_link_options(verified_proxy_module_module_plugin PRIVATE
        -Wl,--exclude-libs,ALL)
endif()
