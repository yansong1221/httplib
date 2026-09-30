find_package(Catch2 CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)

# Shared test support: common helper headers, Catch2 main, internal lib headers.
add_library(httplib_test_support INTERFACE)
set_target_properties(httplib_test_support PROPERTIES FOLDER "tests")
target_link_libraries(httplib_test_support
    INTERFACE
        httplib
        Catch2::Catch2WithMain
)
target_include_directories(httplib_test_support
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${HTTPLIB_LIB_DIR}
)

if(HTTPLIB_ENABLED_SSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(httplib_test_support INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(MSVC)
    target_compile_options(httplib_test_support INTERFACE /bigobj)
endif()

function(add_httplib_test name label)
    add_executable(${name} ${ARGN})
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ARGN})
    target_link_libraries(${name} PRIVATE httplib_test_support)
    set_target_properties(${name} PROPERTIES FOLDER "tests")

    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES LABELS "${label}" TIMEOUT 300)

    # 测试必须保持 TU 隔离，不参与 unity build：
    #   1. 各测试文件在匿名 namespace 里定义了同名辅助函数（set_text / read_file /
    #      arm_watchdog / as_string），凑批后匿名 namespace 合并，直接撞名；
    #   2. http_methods_test.cpp 给 httplib::headers 补了构造函数，跨 TU 给库类加
    #      成员只在"每个测试文件自成一个 TU"的前提下才成立。
    set_target_properties(${name} PROPERTIES UNITY_BUILD OFF)
endfunction()

add_httplib_test(httplib_test_core core
    action_queue_test.cpp
    aspect_test.cpp
    async_event_test.cpp
    async_mutex_test.cpp
    body_pipeline_test.cpp
    body_reader_test.cpp
    body_utils_test.cpp
    chunked_test.cpp
    client_ip_test.cpp
    lazy_request_test.cpp
    middleware_test.cpp
    router_test.cpp
    server_lifecycle_test.cpp
    session_cookie_test.cpp
    ticker_test.cpp
)

add_httplib_test(httplib_test_http http
    compressor_test.cpp
    enum_conv_test.cpp
    headers_test.cpp
    http_methods_test.cpp
    ndjson_test.cpp
    response_test.cpp
    sse_test.cpp
    websocket_test.cpp
)

add_httplib_test(httplib_test_client client
    client_pool_test.cpp
    client_test.cpp
    downloader_test.cpp
    download_scheduler_test.cpp
)

add_httplib_test(httplib_test_proxy proxy
    proxy_strategy_test.cpp
    proxy_test.cpp
)

if(HTTPLIB_ENABLED_SSL)
    add_httplib_test(httplib_test_jwt jwt
        jwt_test.cpp
    )
    target_link_libraries(httplib_test_jwt PRIVATE jwt-cpp::jwt-cpp)
else()
    message(STATUS "HTTPLIB: skipping httplib_test_jwt (HTTPLIB_ENABLED_SSL=OFF)")
endif()

if(HTTPLIB_ENABLED_DATABASE)
    add_httplib_test(httplib_test_db db
        db_bug_test.cpp
        db_regression_test.cpp
        mysql_test.cpp
        odbc_test.cpp
        sqlite_test.cpp
    )
else()
    message(STATUS "HTTPLIB: skipping httplib_test_db (HTTPLIB_ENABLED_DATABASE=OFF)")
endif()
