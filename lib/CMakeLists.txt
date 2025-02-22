set(MOUDLE httplib)

file(GLOB_RECURSE HTTPLIB_HEADER_FILES "${HTTPLIB_INCLUDE_DIR}/*.h" "${HTTPLIB_INCLUDE_DIR}/*.hpp" "${HTTPLIB_INCLUDE_DIR}/*.inl")
source_group(TREE ${HTTPLIB_INCLUDE_DIR} PREFIX "include" FILES ${HTTPLIB_HEADER_FILES})

file(GLOB_RECURSE HTTPLIB_SOURCE_FILES  ${HTTPLIB_LIB_DIR}/*.c ${HTTPLIB_LIB_DIR}/*.cxx ${HTTPLIB_LIB_DIR}/*.cpp ${HTTPLIB_LIB_DIR}/*.hpp)
source_group(TREE ${HTTPLIB_LIB_DIR} PREFIX "lib" FILES ${HTTPLIB_SOURCE_FILES})

find_package(Boost REQUIRED COMPONENTS beast url nowide json)
find_package(fmt CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(${MOUDLE} STATIC ${HTTPLIB_HEADER_FILES} ${HTTPLIB_SOURCE_FILES})

target_link_libraries(${MOUDLE} 
PUBLIC  Boost::beast Boost::url Boost::nowide Boost::json
PUBLIC  OpenSSL::SSL OpenSSL::Crypto
PUBLIC  spdlog::spdlog fmt::fmt
)

target_include_directories(${MOUDLE} PUBLIC ${HTTPLIB_INCLUDE_DIR})

if (MSVC)
    target_compile_options(${MOUDLE} PRIVATE /bigobj)
endif()

target_compile_definitions(${MOUDLE} PUBLIC HTTLIP_ENABLED_SSL)
