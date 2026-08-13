set(MODULE download_demo)

file(GLOB_RECURSE MODULE_SOURCES *.cxx *.cpp *.h *.hpp)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${MODULE_SOURCES})

add_executable(${MODULE} ${MODULE_SOURCES})

target_link_libraries(${MODULE}
PRIVATE httplib
)

if (MSVC)
    target_compile_options(${MODULE} PRIVATE /bigobj)
endif()
