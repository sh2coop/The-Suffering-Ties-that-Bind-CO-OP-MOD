# Game Title (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(Suffering2 default.xbe game.toml ${GAME_PARTITIONS} game)

# test builds: the runtime's Debug/Info/Warn log lines (file opens, kernel calls) with the optimized game code.
# cmake -DGAME_VERBOSE_LOG=ON out/build/release, then build. log.h keys its level on NDEBUG.
option(GAME_VERBOSE_LOG "Write the runtime's verbose log" OFF)
if(GAME_VERBOSE_LOG)
    # with function names for every runtime function in Suffering2.pdb, so tools/sample.py and a crash dump name them
    target_compile_options(Suffering2 PRIVATE -UNDEBUG -g -gcodeview)
endif()

# the executable's icon (tools/launcher/suffering2.rc), compiled with llvm-rc from the toolchain
if(WIN32)
    get_filename_component(LLVM_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
    if(NOT EXISTS ${LLVM_BIN}/llvm-rc.exe)
        find_program(LLVM_RC_FOUND llvm-rc)
        if(LLVM_RC_FOUND)
            get_filename_component(LLVM_BIN ${LLVM_RC_FOUND} DIRECTORY)
        endif()
    endif()
    if(EXISTS ${LLVM_BIN}/llvm-rc.exe)
        set(CMAKE_RC_COMPILER ${LLVM_BIN}/llvm-rc.exe CACHE FILEPATH "" FORCE)
        set(CMAKE_RC_FLAGS "" CACHE STRING "" FORCE)
        enable_language(RC)
        set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> <INCLUDES> <FLAGS> /fo <OBJECT> <SOURCE>")
        # no dependency-file flags: llvm-rc takes none
        set(CMAKE_DEPFILE_FLAGS_RC "")
        set(CMAKE_RC_DEPENDS_USE_COMPILER FALSE)
        set(CMAKE_RC_DEPFILE_FORMAT "")
        target_sources(Suffering2 PRIVATE tools/launcher/suffering2.rc)
    else()
        message(WARNING "llvm-rc was not found: the executable gets no icon")
    endif()
endif()
