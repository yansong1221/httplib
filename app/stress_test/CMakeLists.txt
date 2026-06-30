set(MOUDLE stress_test)

file(GLOB_RECURSE MOUDLE_SOURCES *.h *.cpp *.hpp *.cxx)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${MOUDLE_SOURCES})

add_executable(${MOUDLE} ${MOUDLE_SOURCES})

find_package(Boost REQUIRED COMPONENTS program_options CONFIG)

target_link_libraries(${MOUDLE}
PRIVATE httplib
PRIVATE Boost::program_options
)

if (MSVC)
    target_compile_options(${MOUDLE} PRIVATE /bigobj)
endif()
