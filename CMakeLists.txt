cmake_minimum_required(VERSION 3.20)
# Counter-Strike (Xbox) static recompilation -- clean restart (CS2).
# Toolkit: private clone at tools/xboxrecomp pinned in dependencies.json.
project(cs2_xbox_recomp C)

if(MSVC)
    # The toolkit builds its libraries with /O2 even in Debug; /RTC1 conflicts.
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

# The player build (scripts/build_installer.py): the C runtime linked in, so a PC
# with no Visual C++ redistributable runs it; its data in %LOCALAPPDATA% and a
# log file per run (src/main.c player_setup, src/cs_diag.c); and symbols for
# every release, kept on the build PC and never shipped -- a player's freeze or
# crash report gives code offsets, scripts/symbolize_log.py names them.
# Set before the toolkit is added, so its libraries use the same runtime.
option(CS_RELEASE "Player build for the installer" OFF)
if(CS_RELEASE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(CS_RELEASE=1)
    if(MSVC)
        string(APPEND CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO " /OPT:REF /OPT:ICF")
    endif()
endif()

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/xboxrecomp" CACHE PATH "xboxrecomp root")
if(NOT EXISTS "${XBOXRECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "xboxrecomp missing at ${XBOXRECOMP_DIR}; run scripts/setup.ps1")
endif()
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# Pure pipeline output; never edited by hand.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR "src/recomp/gen is empty; run scripts/pipeline.py")
endif()

set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/cs_gpu.c
    src/cs_d3d11.c
    src/cs_prof.c
    src/cs_vsh_hlsl.c
    src/cs_rc_hlsl.c
    src/cs_kernel.c
    src/cs_launch.c
    src/cs_settings.c
    src/cs_overlay.c
    src/cs_diag.c
    resources/cs.rc
)

option(CS_GEN_INLINE "MSVC: /Ob2 /GS- for the generated code (measured: no gain)" OFF)
if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CS_GEN_INLINE)
    # RelWithDebInfo is /Ob1 (explicit inline only); translated functions are
    # wall-to-wall small helpers. /GS- drops the per-call stack cookie: the
    # generated code's own stack is the guest's, in guest memory.
    set_property(SOURCE ${RECOMP_GEN_SOURCES} APPEND PROPERTY COMPILE_OPTIONS /Ob2 /GS-)
endif()
# Register promotion in the generated code (pipeline --promote-regs emits the
# hooks; see RECOMP_REG_PROMOTE in recomp_types.h): translated functions keep
# the guest registers in locals between calls instead of thread-local memory.
# Generated sources only -- hand-written replacements use the register names
# against the globals.
option(CS_REG_PROMOTE "Keep guest registers in locals in the generated code" ON)
if(CS_REG_PROMOTE)
    set_property(SOURCE ${RECOMP_GEN_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS RECOMP_REG_PROMOTE)
endif()
# Diagnostic: every direct call in the generated code checks that the callee
# kept ebx/esi/edi/esp (RECOMP_ABI_CHECK in recomp_types.h); [ABI] lines name
# the first offenders. Costs a few compares per call.
option(CS_ABI_CHECK "Check callee-saved registers across generated calls" OFF)
if(CS_ABI_CHECK)
    set_property(SOURCE ${RECOMP_GEN_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS RECOMP_ABI_CHECK)
    set_property(SOURCE src/cs_kernel.c APPEND PROPERTY COMPILE_DEFINITIONS CS_ABI_CHECK)
endif()

add_executable(${PROJECT_NAME} WIN32 ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src/usb
    ${XBOXRECOMP_DIR}/src/kernel
    ${XBOXRECOMP_DIR}/src
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11 dxgi dxguid d3dcompiler
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>
    winmm dbghelp
)
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    # The player build names its .pdb without the build folder (symbolize_log.py finds it by id).
    if(CS_RELEASE)
        target_link_options(${PROJECT_NAME} PRIVATE /PDBALTPATH:cs2_xbox_recomp.pdb)
    endif()
    # /FS: parallel cl.exe instances share one PDB.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /FS)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # clang-cl (configure with -T ClangCL). The generated code keeps guest
    # registers and the x87 stack in thread-local globals and reaches guest
    # memory through volatile pointers; MSVC assumes every such store may
    # alias them and reloads the TLS block and memory base after each one.
    # Type-based aliasing lets clang keep those in host registers. Guest
    # memory is only ever touched through the volatile MEM* accessors, which
    # are never reordered against each other, so the generated code does no
    # type punning that strict aliasing could break. Generated sources only:
    # the hand-written runtime is left with clang-cl's MSVC-like default.
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/clang:-fstrict-aliasing;/clang:-fms-compatibility-version=19.20;-Wno-everything")
endif()

# config.exe: the player's settings program (Video and Controls), next to the
# game. Shares settings.ini with the game (src/cs_settings.c) and writes the
# keymap.ini the toolkit's keyboard reads, with the toolkit's own key names.
add_executable(config WIN32
    config/config.c
    resources/config.rc
    src/cs_settings.c
    ${XBOXRECOMP_DIR}/src/input/input_map.c
    ${XBOXRECOMP_DIR}/src/input/keyboard.c
)
target_include_directories(config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${XBOXRECOMP_DIR}/src/input
    ${XBOXRECOMP_DIR}/src
)
target_link_libraries(config PRIVATE comctl32 shell32 version advapi32 user32)

# The installer's import tool (not installed; Setup runs it from its temp folder) and
# RecompLauncher's prepare step: copies the game files from the player's own disc.
add_executable(cs_import installer/cs_import.c)
target_include_directories(cs_import PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/installer)
target_link_libraries(cs_import PRIVATE bcrypt shell32)
target_compile_definitions(cs_import PRIVATE UNICODE _UNICODE)

# RecompLauncher's start program: the game, kept as one session across its relaunches.
add_executable(cs_play WIN32 installer/cs_play.c resources/play.rc)
target_compile_definitions(cs_play PRIVATE UNICODE _UNICODE)
