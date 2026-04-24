project(vlua)
aux_source_directory(./ vlua_src)
add_library(vlua SHARED ${vlua_src})
TARGET_LINK_LIBRARIES(vlua dl)
