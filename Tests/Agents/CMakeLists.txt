project(AgentsTests)

enable_language(C CXX ASM)

set(CMAKE_CXX_COMPILER g++)

file(GLOB_RECURSE TEST_SOURCES ${PROJECT_SOURCE_DIR}/Source/*.cpp)
add_executable(AgentsTests EXCLUDE_FROM_ALL ${TEST_SOURCES})

target_include_directories(AgentsTests PRIVATE ${DOCTESTINC})

add_test(NAME AgentsTests COMMAND AgentsTests)

target_link_libraries(AgentsTests PRIVATE
    agents
    Tactility
    TactilityCore
    TactilityKernel
    platform-posix
    hal-device-module
    lvgl-module
    lvgl
    SDL2::SDL2-static SDL2-static
)
