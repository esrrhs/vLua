cmake_minimum_required(VERSION 3.12)
project(vLua LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep frame pointers so SIGPROF PC attribution stays reliable.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -g3 -DNDEBUG -O2 -fno-omit-frame-pointer")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ---------------------------------------------------------------------------
# Vendored Lua 5.3.6 — vLua needs private VM headers (lobject.h / lstate.h).
# ---------------------------------------------------------------------------
set(LUA_VENDOR_VERSION 5.3.6)
set(LUA_VENDOR_DIR ${CMAKE_SOURCE_DIR}/dep/lua-${LUA_VENDOR_VERSION})
set(LUA_VENDOR_TGZ ${CMAKE_SOURCE_DIR}/dep/lua-${LUA_VENDOR_VERSION}.tar.gz)

if(NOT EXISTS "${LUA_VENDOR_DIR}/src/lua.h")
    if(NOT EXISTS "${LUA_VENDOR_TGZ}")
        message(FATAL_ERROR "Missing vendored Lua archive: ${LUA_VENDOR_TGZ}")
    endif()
    message(STATUS "Extracting vendored Lua ${LUA_VENDOR_VERSION}...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${LUA_VENDOR_TGZ}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dep"
        RESULT_VARIABLE _lua_extract_rc
    )
    if(NOT _lua_extract_rc EQUAL 0)
        message(FATAL_ERROR "Failed to extract ${LUA_VENDOR_TGZ}")
    endif()
endif()

set(LUA_INCLUDE_DIR "${LUA_VENDOR_DIR}/src")

add_subdirectory(src)

# Build the matching Lua 5.3.6 interpreter for tests (same ABI as headers).
set(LUA_LIB_SRCS
    ${LUA_VENDOR_DIR}/src/lapi.c
    ${LUA_VENDOR_DIR}/src/lauxlib.c
    ${LUA_VENDOR_DIR}/src/lbaselib.c
    ${LUA_VENDOR_DIR}/src/lbitlib.c
    ${LUA_VENDOR_DIR}/src/lcode.c
    ${LUA_VENDOR_DIR}/src/lcorolib.c
    ${LUA_VENDOR_DIR}/src/lctype.c
    ${LUA_VENDOR_DIR}/src/ldblib.c
    ${LUA_VENDOR_DIR}/src/ldebug.c
    ${LUA_VENDOR_DIR}/src/ldo.c
    ${LUA_VENDOR_DIR}/src/ldump.c
    ${LUA_VENDOR_DIR}/src/lfunc.c
    ${LUA_VENDOR_DIR}/src/lgc.c
    ${LUA_VENDOR_DIR}/src/linit.c
    ${LUA_VENDOR_DIR}/src/liolib.c
    ${LUA_VENDOR_DIR}/src/llex.c
    ${LUA_VENDOR_DIR}/src/lmathlib.c
    ${LUA_VENDOR_DIR}/src/lmem.c
    ${LUA_VENDOR_DIR}/src/loadlib.c
    ${LUA_VENDOR_DIR}/src/lobject.c
    ${LUA_VENDOR_DIR}/src/lopcodes.c
    ${LUA_VENDOR_DIR}/src/loslib.c
    ${LUA_VENDOR_DIR}/src/lparser.c
    ${LUA_VENDOR_DIR}/src/lstate.c
    ${LUA_VENDOR_DIR}/src/lstring.c
    ${LUA_VENDOR_DIR}/src/lstrlib.c
    ${LUA_VENDOR_DIR}/src/ltable.c
    ${LUA_VENDOR_DIR}/src/ltablib.c
    ${LUA_VENDOR_DIR}/src/ltm.c
    ${LUA_VENDOR_DIR}/src/lundump.c
    ${LUA_VENDOR_DIR}/src/lutf8lib.c
    ${LUA_VENDOR_DIR}/src/lvm.c
    ${LUA_VENDOR_DIR}/src/lzio.c
)

add_library(lua_vendor STATIC ${LUA_LIB_SRCS})
target_include_directories(lua_vendor PUBLIC ${LUA_INCLUDE_DIR})
# POSIX + dlopen, without readline (avoids an extra CI/runtime dependency).
target_compile_definitions(lua_vendor PUBLIC LUA_USE_POSIX LUA_USE_DLOPEN)
target_link_libraries(lua_vendor PUBLIC m ${CMAKE_DL_LIBS})
set_target_properties(lua_vendor PROPERTIES
    OUTPUT_NAME "lua53"
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lua
)

add_executable(lua53 ${LUA_VENDOR_DIR}/src/lua.c)
target_link_libraries(lua53 PRIVATE lua_vendor)
# Export Lua symbols so dlopen'd libvlua.so can resolve them at test time.
set_target_properties(lua53 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lua
    ENABLE_EXPORTS ON
)

# Optionally build Go profile converter tools.
find_program(GO_EXECUTABLE go)
if(GO_EXECUTABLE)
    add_custom_target(tools_vlua ALL
        COMMAND ${GO_EXECUTABLE} build -o ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/vlua vlua.go
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tools
        COMMENT "Building tools/vlua via Go"
    )
endif()

# Testing
option(BUILD_TESTING "Build and register tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
