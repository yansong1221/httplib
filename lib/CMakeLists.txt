set(MODULE httplib)

file(GLOB_RECURSE HTTPLIB_HEADER_FILES "${HTTPLIB_INCLUDE_DIR}/*.h" "${HTTPLIB_INCLUDE_DIR}/*.hpp" "${HTTPLIB_INCLUDE_DIR}/*.inl")
source_group(TREE ${HTTPLIB_INCLUDE_DIR} PREFIX "include" FILES ${HTTPLIB_HEADER_FILES})

file(GLOB_RECURSE HTTPLIB_SOURCE_FILES  ${HTTPLIB_LIB_DIR}/*.h ${HTTPLIB_LIB_DIR}/*.c ${HTTPLIB_LIB_DIR}/*.cxx ${HTTPLIB_LIB_DIR}/*.cpp ${HTTPLIB_LIB_DIR}/*.hpp)
source_group(TREE ${HTTPLIB_LIB_DIR} PREFIX "lib" FILES ${HTTPLIB_SOURCE_FILES})

find_package(Boost REQUIRED COMPONENTS beast json CONFIG)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

if(HTTPLIB_SHARED_LIBRARY)
    add_library(${MODULE} SHARED ${HTTPLIB_HEADER_FILES} ${HTTPLIB_SOURCE_FILES})
    target_compile_definitions(${MODULE} PUBLIC HTTPLIB_SHARED_LIBRARY PRIVATE HTTPLIB_EXPORTS)
else()
    add_library(${MODULE} STATIC ${HTTPLIB_HEADER_FILES} ${HTTPLIB_SOURCE_FILES})
endif()

target_link_libraries(${MODULE} 
PUBLIC  Boost::beast Boost::json
PUBLIC  spdlog::spdlog fmt::fmt
)

target_include_directories(${MODULE} 
    PUBLIC 
        $<BUILD_INTERFACE:${HTTPLIB_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${HTTPLIB_LIB_DIR}
)

if (MSVC)
    target_compile_options(${MODULE} PRIVATE /bigobj)
endif()

if(HTTPLIB_ENABLED_SSL)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(${MODULE} PUBLIC HTTPLIB_ENABLED_SSL)
    target_link_libraries(${MODULE} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
if(HTTPLIB_ENABLED_COMPRESS)
    find_package(Boost REQUIRED COMPONENTS iostreams CONFIG)
    find_package(unofficial-brotli CONFIG REQUIRED)

    target_compile_definitions(${MODULE} PUBLIC HTTPLIB_ENABLED_COMPRESS)
    target_link_libraries(${MODULE} PRIVATE Boost::iostreams unofficial::brotli::brotlidec unofficial::brotli::brotlienc)
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
