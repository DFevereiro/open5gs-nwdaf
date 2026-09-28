find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.4)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    message(STATUS "Catch2 fetched via FetchContent")
endif()

add_library(nwdaf_mock
    mock_open5gs.cpp
)
target_include_directories(nwdaf_mock PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(nwdaf_mock PUBLIC nwdaf_lib)

# Unit + ARCH tests — no shared network resources, safe to run in parallel.
add_executable(nwdaf_tests
    test_collector.cpp
    test_analytics.cpp
    test_arch_improvements.cpp   # TIER-4 ARCH-01…ARCH-05 tests
    test_h1_analytics.cpp        # H1.4 catalogue + H1.5 E-model MOS
    test_analytics_catalogue.cpp # H1.7 known/operator/implemented/advertised IDs
    test_schema_validator.cpp    # H1.7 OpenAPI validator against the official artifacts
    test_supported_features.cpp  # H1.7 TS 29.571 SupportedFeatures + negotiation
    test_oauth.cpp               # H1.10 access-token validation (TLS builds)
)
target_include_directories(nwdaf_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(nwdaf_tests PRIVATE
    nwdaf_mock
    nwdaf_lib
    Catch2::Catch2WithMain
)
target_compile_definitions(nwdaf_tests PRIVATE
    NWDAF_3GPP_OPENAPI_DIR="${NWDAF_3GPP_OPENAPI_DIR}"
)

# Integration tests bind to port 17779 and must not run in parallel with each
# other.  RESOURCE_LOCK ensures CTest serialises every test in this target.
add_executable(nwdaf_integration_tests
    test_server_integration.cpp
    test_openapi_conformance.cpp   # H1.6 OpenAPI contract test
    test_sbi_security.cpp          # H1.10 mTLS (compiled to no tests without NWDAF_USE_TLS)
    test_3gpp_sbi.cpp              # H1.7 3GPP Nnwdaf_AnalyticsInfo / EventsSubscription
    test_nrf_client.cpp            # H1.9 NRF NFManagement client
)
target_include_directories(nwdaf_integration_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(nwdaf_integration_tests PRIVATE
    nwdaf_mock
    nwdaf_lib
    Catch2::Catch2WithMain
)
# H1.6: the contract test reads the published OpenAPI document from the source
# tree, so the spec and the code are checked against each other in CI.
target_compile_definitions(nwdaf_integration_tests PRIVATE
    NWDAF_OPENAPI_SPEC="${CMAKE_SOURCE_DIR}/docs/openapi/nwdaf-analytics-v1.yaml"
    NWDAF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    NWDAF_3GPP_OPENAPI_DIR="${NWDAF_3GPP_OPENAPI_DIR}"
)

include(CTest)
include(Catch)
catch_discover_tests(nwdaf_tests)
catch_discover_tests(nwdaf_integration_tests
    PROPERTIES RESOURCE_LOCK "nwdaf_port_17779"
)
