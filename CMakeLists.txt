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
    src/cs_kernel.c
    src/cs_launch.c
)

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
    # /FS: parallel cl.exe instances share one PDB.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /FS)
endif()
