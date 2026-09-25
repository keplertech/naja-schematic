cmake_minimum_required(VERSION 3.20)
project(naja-schematic)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------
# Git commit hash (baked in at configure time)
# ------------------------------------------------------------
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE NAJA_SCHEMATIC_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
else()
  set(NAJA_SCHEMATIC_GIT_HASH "unknown")
endif()
message(STATUS "naja-schematic commit: ${NAJA_SCHEMATIC_GIT_HASH}")

# ------------------------------------------------------------
# naja version (read from the submodule's literal, not built — needed on
# both targets since both interface with naja: native links it directly,
# WASM talks to najaeda_server.py, which wraps the same naja/najaeda code)
# ------------------------------------------------------------
file(STRINGS ${CMAKE_SOURCE_DIR}/thirdparty/naja/src/core/NajaVersion.h.in
  NAJA_VERSION_LINE REGEX "NAJA_VERSION \\{ \"[^\"]*\" \\}")
string(REGEX REPLACE ".*\"([^\"]*)\".*" "\\1" NAJA_VERSION "${NAJA_VERSION_LINE}")
message(STATUS "naja version: ${NAJA_VERSION}")

# ------------------------------------------------------------
# Detect Emscripten
# ------------------------------------------------------------
if("${CMAKE_CXX_COMPILER}" MATCHES "em\\+\\+")
  message(STATUS "Configuring for Emscripten (WASM) build")
  set(EMSCRIPTEN_BUILD TRUE)
endif()

# ------------------------------------------------------------
# ImGui sources (shared by both targets)
# ------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/thirdparty/imgui)

set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

include_directories(
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
  ${CMAKE_SOURCE_DIR}/thirdparty
  ${CMAKE_SOURCE_DIR}/src
)

# 32-bit ImDrawIdx: see src/imgui_user_config.h for why (16-bit indices cap
# a single ImDrawList at 64K vertices, which the WASM/WebGL backend -- unlike
# desktop GL 3.2+ -- has no way to work around).
add_compile_definitions(IMGUI_USER_CONFIG="imgui_user_config.h")

# ------------------------------------------------------------
# Core application sources (no platform-specific I/O)
# ------------------------------------------------------------
set(CORE_SOURCES
  src/Console.cpp
  src/Types.cpp
  src/GUIData.cpp
  src/NetlistTree.cpp
  src/EquipotentialView.cpp
  src/SchematicView.cpp
  src/DiagnosisStore.cpp
  src/DiagnosisView.cpp
  src/SourceStore.cpp
  src/SourceView.cpp
  src/PropertiesStore.cpp
  src/PropertiesView.cpp
  src/AppLogic.cpp
  ${IMGUI_SOURCES}
)

# ============================================================
# Target 1: naja-schematic  — Browser / VSCode webview (WASM)
# ============================================================
if(EMSCRIPTEN_BUILD)
  message(STATUS "Building WASM target: naja-schematic")

  add_executable(naja-schematic
    ${CORE_SOURCES}
    src/WebSocketClient.cpp
    src/WebSocketProvider.cpp
    src/JsBridgeProvider.cpp
    src/main_wasm.cpp
  )

  set(EM_COMPILE_FLAGS
    "-sUSE_SDL=2"
    "-fexceptions"
    "-sEXCEPTION_CATCHING_ALLOWED=['main','naja-schematic']"
  )

  set(EM_LINK_FLAGS
    "-sUSE_SDL=2"
    "-sUSE_WEBGL2=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sFULL_ES3=1"
    "-sASSERTIONS=1"
    "-sEXIT_RUNTIME=0"
    "-sEXPORTED_RUNTIME_METHODS=['requestFullscreen']"
    "-sWASM=1"
    "-lwebsocket.js"
    "-lembind"
    "-fexceptions"
    "-sEXCEPTION_STACK_TRACES=1"
    "-sEXCEPTION_CATCHING_ALLOWED=['main','naja-schematic']"
  )

  # Default: a full page (naja-schematic.html + .js + .wasm) for emrun.
  # NAJA_SCHEMATIC_WASM_MODULE: one self-contained naja-schematic.js (wasm
  # inlined) defining a createNajaSchematic({canvas, ...}) factory, so a host
  # page can embed one viewer per canvas -- what the naja_schematic Python
  # package ships (its index.html and Jupyter widget both load it).
  option(NAJA_SCHEMATIC_WASM_MODULE
    "Build the WASM viewer as an embeddable single-file JS module" OFF)
  if(NAJA_SCHEMATIC_WASM_MODULE)
    list(APPEND EM_LINK_FLAGS
      "-sMODULARIZE=1"
      "-sEXPORT_NAME=createNajaSchematic"
      "-sSINGLE_FILE=1"
      "-sENVIRONMENT=web"
    )
    set(NAJA_SCHEMATIC_WASM_SUFFIX ".js")
  else()
    list(APPEND EM_LINK_FLAGS "--shell-file=${CMAKE_SOURCE_DIR}/src/shell_minimal.html")
    set(NAJA_SCHEMATIC_WASM_SUFFIX ".html")
  endif()

  target_compile_options(naja-schematic PRIVATE ${EM_COMPILE_FLAGS})
  target_link_options(naja-schematic    PRIVATE ${EM_LINK_FLAGS})
  set_target_properties(naja-schematic  PROPERTIES SUFFIX ${NAJA_SCHEMATIC_WASM_SUFFIX})
  target_compile_definitions(naja-schematic PRIVATE
    NAJA_SCHEMATIC_GIT_HASH="${NAJA_SCHEMATIC_GIT_HASH}"
    NAJA_VERSION_STRING="${NAJA_VERSION}"
  )

# ============================================================
# Target 2: naja-schematic-standalone — Native desktop app
# ============================================================
else()
  message(STATUS "Building native standalone target: naja-schematic-standalone")

  # ---- naja SNL ----
  # Disable naja sub-features we don't need
  set(BUILD_NAJA_PYTHON  OFF CACHE BOOL "" FORCE)
  set(BUILD_BENCHMARKS   OFF CACHE BOOL "" FORCE)
  set(ENABLE_SANITIZERS  OFF CACHE BOOL "" FORCE)
  set(CODE_COVERAGE      OFF CACHE BOOL "" FORCE)
  add_subdirectory(thirdparty/naja)

  find_package(SDL2 REQUIRED)
  find_package(OpenGL REQUIRED)

  if(APPLE)
    set(PLATFORM_FILE_DIALOG_SOURCE src/NativeFileDialog.mm)
    set(PLATFORM_FILE_DIALOG_LIBS "-framework Cocoa")
  else()
    set(PLATFORM_FILE_DIALOG_SOURCE src/NativeFileDialogLinux.cpp)
    set(PLATFORM_FILE_DIALOG_LIBS "")
  endif()

  add_executable(naja-schematic-standalone
    ${CORE_SOURCES}
    src/LocalSNLProvider.cpp
    ${PLATFORM_FILE_DIALOG_SOURCE}
    src/main_native.cpp
  )

  target_include_directories(naja-schematic-standalone PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(naja-schematic-standalone PRIVATE
    ${SDL2_LIBRARIES}
    OpenGL::GL
    naja_nl
    naja_snl_verilog
    naja_snl_liberty
    naja_snl_systemverilog
    naja_nl_dump
    ${PLATFORM_FILE_DIALOG_LIBS}
  )

  target_compile_definitions(naja-schematic-standalone PRIVATE
    NAJA_SCHEMATIC_GIT_HASH="${NAJA_SCHEMATIC_GIT_HASH}"
    NAJA_VERSION_STRING="${NAJA_VERSION}"
  )

  # Enable compile_commands.json for clangd / VS Code IntelliSense
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# ============================================================
# Target 3: naja-schematic-tests — GUI-independent unit tests
# ============================================================
# Native only: exercises the logic layer (JSON parsing, DiagnosisStore,
# SourceStore, NetlistTree structure) without SDL/OpenGL/naja. Links only
# ImGui's core (imgui.cpp/imgui_draw.cpp/imgui_tables.cpp/imgui_widgets.cpp),
# not the SDL2/OpenGL3 backends, since none of the tested code needs a real
# render backend -- render()/draw calls are never invoked from tests.
if(NOT EMSCRIPTEN_BUILD)
  option(NAJA_SCHEMATIC_BUILD_TESTS "Build naja-schematic-tests" ON)
  if(NAJA_SCHEMATIC_BUILD_TESTS)
    # naja (added above) already vendors its own googletest copy
    # (thirdparty/naja/thirdparty/googletest, EXCLUDE_FROM_ALL) and defines
    # gtest/gtest_main targets from it. Only add ours if those aren't
    # already present, since CMake target names must be unique build-wide.
    if(NOT TARGET gtest_main)
      set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
      set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
      add_subdirectory(thirdparty/googletest)
    endif()

    file(GLOB NAJA_SCHEMATIC_TEST_SOURCES tests/*.cpp)

    add_executable(naja-schematic-tests
      src/Console.cpp
      src/Types.cpp
      src/GUIData.cpp
      src/NetlistTree.cpp
      src/DiagnosisStore.cpp
      src/SourceStore.cpp
      src/PropertiesStore.cpp
      ${IMGUI_DIR}/imgui.cpp
      ${IMGUI_DIR}/imgui_draw.cpp
      ${IMGUI_DIR}/imgui_tables.cpp
      ${IMGUI_DIR}/imgui_widgets.cpp
      ${NAJA_SCHEMATIC_TEST_SOURCES}
    )
    target_link_libraries(naja-schematic-tests PRIVATE gtest_main)

    enable_testing()
    include(GoogleTest)
    gtest_discover_tests(naja-schematic-tests)
  endif()
endif()
