set(MOUDLE httplib)


option(HTTPLIB_ENABLED_SSL "HTTLIB ENABLED SSL" OFF)
option(HTTPLIB_ENABLED_COMPRESS "HTTLIB ENABLED COMPRESS" OFF)
option(HTTPLIB_ENABLED_WEBSOCKET "HTTLIB ENABLED WEBSOCKET" OFF)

file(GLOB_RECURSE HTTPLIB_HEADER_FILES "${HTTPLIB_INCLUDE_DIR}/*.h" "${HTTPLIB_INCLUDE_DIR}/*.hpp" "${HTTPLIB_INCLUDE_DIR}/*.inl")
source_group(TREE ${HTTPLIB_INCLUDE_DIR} PREFIX "include" FILES ${HTTPLIB_HEADER_FILES})

file(GLOB_RECURSE HTTPLIB_SOURCE_FILES  ${HTTPLIB_LIB_DIR}/*.c ${HTTPLIB_LIB_DIR}/*.cxx ${HTTPLIB_LIB_DIR}/*.cpp ${HTTPLIB_LIB_DIR}/*.hpp)
source_group(TREE ${HTTPLIB_LIB_DIR} PREFIX "lib" FILES ${HTTPLIB_SOURCE_FILES})

find_package(Boost REQUIRED COMPONENTS beast url nowide json)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(${MOUDLE} STATIC ${HTTPLIB_HEADER_FILES} ${HTTPLIB_SOURCE_FILES})

target_link_libraries(${MOUDLE} 
PUBLIC  Boost::beast Boost::url Boost::nowide Boost::json
PUBLIC  spdlog::spdlog fmt::fmt
)

target_include_directories(${MOUDLE} PUBLIC ${HTTPLIB_INCLUDE_DIR})

if (MSVC)
    target_compile_options(${MOUDLE} PRIVATE /bigobj)
endif()

if(HTTPLIB_ENABLED_SSL)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(${MOUDLE} PUBLIC HTTPLIB_ENABLED_SSL)
    target_link_libraries(${MOUDLE} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
if(HTTPLIB_ENABLED_COMPRESS)
    find_package(Boost REQUIRED COMPONENTS iostreams)
    target_compile_definitions(${MOUDLE} PUBLIC HTTPLIB_ENABLED_COMPRESS)
    target_link_libraries(${MOUDLE} PRIVATE Boost::iostreams)
endif()

IF(HTTPLIB_ENABLED_WEBSOCKET)
    target_compile_definitions(${MOUDLE} PUBLIC HTTPLIB_ENABLED_WEBSOCKET)
endif()

if (WIN32)
    include (win32_version.cmake)

    get_WIN32_WINNT(winnt_version)

    target_compile_definitions(
        ${MOUDLE}
        PUBLIC
            _WIN32_WINNT=${winnt_version}   # Keep this public so user's can't accidentally use conflicting versions
    )
endif()
