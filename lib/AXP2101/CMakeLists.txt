aux_source_directory(. DIR_AXP2101_SRCS)

include_directories(../Config)

add_library(AXP2101 ${DIR_AXP2101_SRCS})
target_link_libraries(AXP2101 PUBLIC Config)
