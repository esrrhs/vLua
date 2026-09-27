# Smoke tests use the vendored Lua 5.3.6 interpreter so VM internals match
# the headers used to compile libvlua.so.

set(VLUA_TEST_ENV
    "LUA_CPATH=${CMAKE_SOURCE_DIR}/bin/?.so\;${CMAKE_SOURCE_DIR}/bin/lib?.so"
)

add_test(
    NAME test_getstr
    COMMAND $<TARGET_FILE:lua53> ${CMAKE_CURRENT_SOURCE_DIR}/test_getstr.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(test_getstr PROPERTIES ENVIRONMENT "${VLUA_TEST_ENV}")

add_test(
    NAME test_funcname
    COMMAND $<TARGET_FILE:lua53> ${CMAKE_CURRENT_SOURCE_DIR}/test_funcname.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(test_funcname PROPERTIES ENVIRONMENT "${VLUA_TEST_ENV}")

add_test(
    NAME test_hotreload
    COMMAND $<TARGET_FILE:lua53> ${CMAKE_CURRENT_SOURCE_DIR}/test_hotreload.lua
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(test_hotreload PROPERTIES ENVIRONMENT "${VLUA_TEST_ENV}")
