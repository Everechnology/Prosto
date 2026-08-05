cmake_minimum_required(VERSION 3.16)
project(ProstoPlus VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MINGW)
    # Enable DLL export/import handling for MinGW builds.
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ---------------------------------------------------------------------------
# Dependencies (adjust paths for your environment)
# ---------------------------------------------------------------------------
# The project vendors sqlite3 for portability and uses native Windows APIs
# for bundled functionality, avoiding external system dependencies for this build.

# ---------------------------------------------------------------------------
# prosto_core.dll — shared library
# ---------------------------------------------------------------------------
add_library(prosto_core SHARED
    src/utils.cpp
    src/interpreter.cpp
    src/builtins.cpp
    src/sqlite3.c
)

target_compile_definitions(prosto_core PRIVATE PROSTO_BUILD_DLL)
target_include_directories(prosto_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include/miniz
)

target_link_libraries(prosto_core
    PRIVATE ws2_32 ole32 oleaut32 uuid
)

if(WIN32)
    target_link_libraries(prosto_core PRIVATE winhttp bcrypt shell32)
    target_compile_definitions(prosto_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# prosto_cli.exe — standalone REPL / script runner
# ---------------------------------------------------------------------------
add_executable(prosto_cli src/main.cpp)
target_link_libraries(prosto_cli PRIVATE prosto_core)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS prosto_core prosto_cli
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(DIRECTORY include/prosto DESTINATION include)
