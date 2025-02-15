set(MOUDLE httplib)

file(GLOB_RECURSE MOUDLE_SOURCES *.h *.cpp *.hpp *.cxx)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${MOUDLE_SOURCES})

find_package(Boost REQUIRED COMPONENTS beast url)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(${MOUDLE} ${MOUDLE_SOURCES})

target_link_libraries(${MOUDLE} 
PUBLIC  Boost::beast Boost::url
PUBLIC  OpenSSL::SSL OpenSSL::Crypto
PUBLIC  spdlog::spdlog
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)