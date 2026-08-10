cmake_minimum_required(VERSION 3.20)
project(spatial_midi_graph_sequencer VERSION 1.0.0 LANGUAGES CXX)

option(SPATIAL_MIDI_BUILD_APP "Build the SDL2/ALSA desktop application" ON)
option(SPATIAL_MIDI_BUILD_TESTS "Build core unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(spatial_midi_core
    src/core/graph.cpp
    src/core/midi_note_input_worker.cpp
    src/core/sequencer_engine.cpp
    src/core/transport_worker.cpp
)
target_include_directories(spatial_midi_core PUBLIC include)
target_link_libraries(spatial_midi_core PUBLIC Threads::Threads)
target_compile_options(spatial_midi_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(SPATIAL_MIDI_BUILD_APP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2>=2.0.18)
    pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

    add_library(spatial_midi_alsa src/midi/alsa_sequencer.cpp)
    target_include_directories(spatial_midi_alsa PUBLIC include)
    target_link_libraries(spatial_midi_alsa PUBLIC spatial_midi_core PkgConfig::ALSA)
    target_compile_options(spatial_midi_alsa PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

    add_library(spatial_midi_ui
        src/ui/sdl_app.cpp
        src/ui/sdl_app_view.cpp
        src/ui/text_cache.cpp
    )
    target_include_directories(spatial_midi_ui PUBLIC include)
    target_link_libraries(spatial_midi_ui PUBLIC spatial_midi_core PkgConfig::SDL2 PkgConfig::SDL2_TTF)
    target_compile_options(spatial_midi_ui PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

    add_executable(spatial-midi src/main.cpp)
    target_link_libraries(spatial-midi PRIVATE spatial_midi_core spatial_midi_alsa spatial_midi_ui)
    target_compile_options(spatial-midi PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    install(TARGETS spatial-midi RUNTIME DESTINATION bin)
endif()

if(SPATIAL_MIDI_BUILD_TESTS)
    enable_testing()
    add_executable(spatial-midi-tests tests/core_tests.cpp)
    target_link_libraries(spatial-midi-tests PRIVATE spatial_midi_core)
    target_compile_options(spatial-midi-tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME spatial-midi-core-tests COMMAND spatial-midi-tests)
endif()
