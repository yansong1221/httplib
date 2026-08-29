set(MOUDLE httplib_tests)

find_package(Catch2 CONFIG REQUIRED)

file(GLOB_RECURSE MOUDLE_SOURCES *.h *.cpp *.hpp *.cxx)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${MOUDLE_SOURCES})

add_executable(${MOUDLE} ${MOUDLE_SOURCES})

target_link_libraries(${MOUDLE}
PRIVATE httplib
PRIVATE Catch2::Catch2WithMain
)

find_package(jwt-cpp CONFIG REQUIRED)
target_link_libraries(${MOUDLE} PRIVATE jwt-cpp::jwt-cpp)

if(HTTPLIB_ENABLED_SSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(${MOUDLE} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

target_include_directories(${MOUDLE} PRIVATE ${HTTPLIB_LIB_DIR})

if (MSVC)
    target_compile_options(${MOUDLE} PRIVATE /bigobj)
endif()

add_test(NAME ${MOUDLE} COMMAND ${MOUDLE})
