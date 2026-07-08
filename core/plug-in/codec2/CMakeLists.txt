set (codec2_SRCS
 codec2.cpp
)

INCLUDE_DIRECTORIES(${CODEC2_INCLUDE_DIRS})

SET(sems_module_name codec2)
SET(sems_module_libs ${CODEC2_LIBRARIES})
INCLUDE(${CMAKE_SOURCE_DIR}/cmake/module.rules.txt)
