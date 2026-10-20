set(MOUDLE demo)

file(GLOB_RECURSE MOUDLE_SOURCES *.h *.cpp *.hpp *.cxx)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${MOUDLE_SOURCES})

add_executable(${MOUDLE} ${MOUDLE_SOURCES})

target_link_libraries(${MOUDLE}
PRIVATE httplib
)

# 与库使用同一组能力特征位（演示程序里按能力分支）。
target_compile_definitions(${MOUDLE} PRIVATE ${HTTPLIB_FEATURE_DEFINITIONS})

if (MSVC)
    target_compile_options(${MOUDLE} PRIVATE /bigobj)
endif()
