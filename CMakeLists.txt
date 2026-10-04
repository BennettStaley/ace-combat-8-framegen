cmake_minimum_required(VERSION 3.22)
project(ac8tweaks C CXX)

set(CMAKE_C_STANDARD 11)
# Static runtime: the launcher must run from the game folder with nothing else installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W4)

add_executable(ac8tweaks WIN32 launcher/main.c)

# The self-test includes main.c directly so it exercises the same code that ships.
add_executable(ac8tweaks_selftest launcher/selftest.c)
add_dependencies(ac8tweaks_selftest ac8tweaks)

# In-game menu: a DLL the Lua core loads, drawing Dear ImGui on the frames the game presents.
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_options(imgui PRIVATE /W3)

add_library(ac8overlay SHARED overlay/overlay.cpp)
set_target_properties(ac8overlay PROPERTIES CXX_STANDARD 17)
target_link_libraries(ac8overlay PRIVATE imgui d3d12 dxgi d3dcompiler)

# Presents frames like the game does, loads the menu DLL and checks what it drew.
add_executable(ac8overlay_harness overlay/harness.cpp)
set_target_properties(ac8overlay_harness PROPERTIES CXX_STANDARD 17)
target_link_libraries(ac8overlay_harness PRIVATE d3d12 dxgi)
add_dependencies(ac8overlay_harness ac8overlay)

enable_testing()
add_test(NAME launcher_selftest COMMAND ac8tweaks_selftest $<TARGET_FILE:ac8tweaks>)
add_test(NAME lua_core COMMAND python ${CMAKE_SOURCE_DIR}/tests/test_lua.py)
add_test(NAME overlay COMMAND python ${CMAKE_SOURCE_DIR}/tests/test_overlay.py $<TARGET_FILE:ac8overlay_harness> $<TARGET_FILE:ac8overlay>)

# cmake --install build --config Release --prefix dist
# produces the folder whose contents drop into the game root. The stock UE4SS settings and mod
# lists are excluded from the vendored copy; ours in mod/ue4ss take their place.
install(TARGETS ac8tweaks RUNTIME DESTINATION .)
install(TARGETS ac8overlay RUNTIME DESTINATION AC8Tweaks)
install(DIRECTORY mod/AC8Tweaks DESTINATION .)
install(DIRECTORY third_party/ue4ss/ DESTINATION Game/Binaries/Win64
        PATTERN "SOURCE.txt" EXCLUDE PATTERN "UE4SS-settings.ini" EXCLUDE
        PATTERN "mods.txt" EXCLUDE PATTERN "mods.json" EXCLUDE)
# The mod's own folder is replaced whole, so a script removed from the source never ships from an old dist.
install(CODE "file(REMOVE_RECURSE \"\${CMAKE_INSTALL_PREFIX}/Game/Binaries/Win64/ue4ss/Mods/AC8Tweaks\")")
install(DIRECTORY mod/ue4ss/ DESTINATION Game/Binaries/Win64/ue4ss)
