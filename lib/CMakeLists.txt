set(MODULE httplib)

set(GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
    ${HTTPLIB_INCLUDE_DIR}/httplib/version.hpp.in
    ${GENERATED_INCLUDE_DIR}/httplib/version.hpp
    @ONLY
)

file(GLOB_RECURSE HTTPLIB_HEADER_FILES "${HTTPLIB_INCLUDE_DIR}/*.h" "${HTTPLIB_INCLUDE_DIR}/*.hpp" "${HTTPLIB_INCLUDE_DIR}/*.inl")
source_group(TREE ${HTTPLIB_INCLUDE_DIR} PREFIX "include" FILES ${HTTPLIB_HEADER_FILES})

file(GLOB_RECURSE HTTPLIB_SOURCE_FILES ${HTTPLIB_LIB_DIR}/*.h ${HTTPLIB_LIB_DIR}/*.c ${HTTPLIB_LIB_DIR}/*.cxx ${HTTPLIB_LIB_DIR}/*.cpp ${HTTPLIB_LIB_DIR}/*.hpp)
source_group(TREE ${HTTPLIB_LIB_DIR} PREFIX "lib" FILES ${HTTPLIB_SOURCE_FILES})

find_package(Boost REQUIRED COMPONENTS asio beast json url CONFIG)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

if(HTTPLIB_SHARED_LIBRARY)
    add_library(${MODULE} SHARED ${HTTPLIB_HEADER_FILES} ${HTTPLIB_SOURCE_FILES})
    target_compile_definitions(${MODULE} PUBLIC HTTPLIB_SHARED_LIBRARY PRIVATE HTTPLIB_EXPORTS)
else()
    add_library(${MODULE} STATIC ${HTTPLIB_HEADER_FILES} ${HTTPLIB_SOURCE_FILES})
endif()

if(WIN32 AND HTTPLIB_SHARED_LIBRARY)
    set(RC_FILE ${GENERATED_INCLUDE_DIR}/win32_version.rc)
    configure_file(
        ${HTTPLIB_LIB_DIR}/win32_version.rc.in
        ${RC_FILE}
        @ONLY
    )
    target_sources(${MODULE} PRIVATE ${RC_FILE})
endif()

target_link_libraries(${MODULE}
PUBLIC  Boost::json Boost::asio
PUBLIC  spdlog::spdlog fmt::fmt
PRIVATE Boost::beast Boost::url
)

target_include_directories(${MODULE} 
    PUBLIC 
        $<BUILD_INTERFACE:${HTTPLIB_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${HTTPLIB_LIB_DIR}
)

if (MSVC)
    target_compile_options(${MODULE} PRIVATE /bigobj)
endif()

# 能力特征位只注入本库编译单元：公共头零宏，未启用能力的运行期收口见
# server_impl.cpp / http_stream.hpp 的 #else 分支与 lib/jwt/*.cpp 的失效桩段。
target_compile_definitions(${MODULE} PRIVATE ${HTTPLIB_FEATURE_DEFINITIONS})

# jwt 模块条件编译：门控由 HTTPLIB_FEATURE_DEFINITIONS（PRIVATE 定义，见
# 根 CMakeLists）驱动，jwt 头文件树与 lib 编译单元自行 #ifdef 感知——CMake 只
# 负责依赖查找与链接，宏不进任何导出接口（DATABASE/COMPRESS/SSL 同规）。
# HTTPS/WSS 通道（HTTPLIB_ENABLED_SSL 走 boost::asio::ssl）与 jwt-cpp 都要
# OpenSSL：历史上通道的链路是被 jwt 的无条件 OpenSSL 链接顺带 carry 的，jwt
# 抽门控后两处显式声明，SSL=ON + JWT=OFF 与 SSL=OFF + JWT=ON 都自洽。
if(HTTPLIB_ENABLED_SSL OR HTTPLIB_ENABLED_JWT)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(${MODULE} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(HTTPLIB_ENABLED_JWT)
    find_package(jwt-cpp CONFIG REQUIRED)
    target_link_libraries(${MODULE} PRIVATE jwt-cpp::jwt-cpp)
endif()

if(HTTPLIB_ENABLED_COMPRESS)
    find_package(Boost REQUIRED COMPONENTS iostreams CONFIG)
    find_package(unofficial-brotli CONFIG REQUIRED)

    target_link_libraries(${MODULE} PRIVATE Boost::iostreams unofficial::brotli::brotlidec unofficial::brotli::brotlienc)
endif()
if(HTTPLIB_ENABLED_DATABASE)
    find_package(Boost REQUIRED COMPONENTS mysql charconv CONFIG)

    target_link_libraries(${MODULE} PRIVATE Boost::mysql Boost::charconv)

    find_library(SQLITE3_LIB sqlite3 REQUIRED)
    target_link_libraries(${MODULE} PRIVATE ${SQLITE3_LIB})

    if(WIN32)
        target_link_libraries(${MODULE} PRIVATE odbc32)
    else()
        find_library(ODBC_LIB odbc REQUIRED)
        target_link_libraries(${MODULE} PRIVATE ${ODBC_LIB} ${CMAKE_DL_LIBS})
    endif()
endif()


if (WIN32)
    include (win32_version.cmake)

    get_WIN32_WINNT(winnt_version)

    target_compile_definitions(
        ${MODULE}
        PUBLIC
            _WIN32_WINNT=${winnt_version}   # Keep this public so user's can't accidentally use conflicting versions
    )
endif()

include(GNUInstallDirs)

install(TARGETS ${MODULE}
    EXPORT httplib-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY ${HTTPLIB_INCLUDE_DIR}/httplib
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PATTERN "*.hpp.in" EXCLUDE
)
install(FILES ${GENERATED_INCLUDE_DIR}/httplib/version.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/httplib
)

install(EXPORT httplib-targets
    NAMESPACE httplib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/httplib
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/httplib-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/httplib-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/httplib
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/httplib-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/httplib
)
