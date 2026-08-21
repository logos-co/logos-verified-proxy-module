cmake_minimum_required(VERSION 3.14)
project(VerifiedProxyModuleTests LANGUAGES CXX)

include(LogosTest)

# Mirror the module build: inject the version from metadata.json.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../metadata.json" _vp_metadata_json)
string(JSON VERIFIED_PROXY_MODULE_VERSION GET "${_vp_metadata_json}" version)

# Unit tests against a MOCKED libverifproxy.
#
# flake.nix sets tests.mockCLibs = [ "verifproxy" ], which stops the test
# derivation from even resolving the real external library — without it every
# CI test run would pay for the whole nimbus/Nim toolchain build.
logos_test(
    NAME verified_proxy_module_tests
    MODULE_SOURCES
        ../src/proxy_config.cpp
        ../src/proxy_runtime.cpp
        ../src/verified_proxy_impl.cpp
    TEST_SOURCES
        main.cpp
        test_config_validation.cpp
        test_proxy_runtime.cpp
        verified_proxy_events_test.cpp
    MOCK_C_SOURCES
        mocks/mock_libverifproxy.cpp
    EXTRA_INCLUDES
        stubs
        .
)
target_compile_definitions(verified_proxy_module_tests PRIVATE
    VERIFIED_PROXY_MODULE_VERSION="${VERIFIED_PROXY_MODULE_VERSION}")
