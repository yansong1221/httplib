set(MOUDLE examples)

file(GLOB_RECURSE MOUDLE_SOURCES *.h *.cpp *.hpp *.cxx)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${MOUDLE_SOURCES})

add_executable(${MOUDLE} ${MOUDLE_SOURCES})

target_link_libraries(${MOUDLE} 
PRIVATE httplib
)

target_include_directories(${MOUDLE} PRIVATE ${PROJECT_SOURCE_DIR}/lib)

if (MSVC)
    target_compile_options(${MOUDLE} PRIVATE /bigobj)
endif()

#target_compile_definitions(${MOUDLE} PRIVATE HTTLIP_ENABLED_SSL)