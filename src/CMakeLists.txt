add_library(vlua SHARED vlua.cpp)
target_include_directories(vlua PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(vlua PRIVATE ${CMAKE_DL_LIBS} pthread)
set_target_properties(vlua PROPERTIES PREFIX "lib" OUTPUT_NAME "vlua")

# Lua symbols are resolved from the host process at load time (require / hookso).
if(APPLE)
    set_target_properties(vlua PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
endif()
