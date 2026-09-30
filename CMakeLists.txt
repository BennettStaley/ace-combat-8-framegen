cmake_minimum_required(VERSION 3.22)
project(ac8tweaks C)

set(CMAKE_C_STANDARD 11)
# Static runtime: the launcher must run from the game folder with nothing else installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W4)

add_executable(ac8tweaks WIN32 launcher/main.c)

# The self-test includes main.c directly so it exercises the same code that ships.
add_executable(ac8tweaks_selftest launcher/selftest.c)
add_dependencies(ac8tweaks_selftest ac8tweaks)

enable_testing()
add_test(NAME launcher_selftest COMMAND ac8tweaks_selftest $<TARGET_FILE:ac8tweaks>)
add_test(NAME lua_core COMMAND python ${CMAKE_SOURCE_DIR}/tests/test_lua.py)

# cmake --install build --config Release --prefix dist
# produces the folder whose contents drop into the game root. The stock UE4SS settings and mod
# lists are excluded from the vendored copy; ours in mod/ue4ss take their place.
install(TARGETS ac8tweaks RUNTIME DESTINATION .)
install(DIRECTORY mod/AC8Tweaks DESTINATION .)
install(DIRECTORY third_party/ue4ss/ DESTINATION Game/Binaries/Win64
        PATTERN "SOURCE.txt" EXCLUDE PATTERN "UE4SS-settings.ini" EXCLUDE
        PATTERN "mods.txt" EXCLUDE PATTERN "mods.json" EXCLUDE)
install(DIRECTORY mod/ue4ss/ DESTINATION Game/Binaries/Win64/ue4ss)
