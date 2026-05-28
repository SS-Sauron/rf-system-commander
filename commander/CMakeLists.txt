
cmake_minimum_required(VERSION 3.16)

# Shared components directory: health and output live here.
# Both scanner and commander point to the same physical directory.
# idf_component_register in each component's own CMakeLists.txt handles
# the rest — no manual include_directories() needed.
list(APPEND EXTRA_COMPONENT_DIRS "../common_components")
set(PARTITION_TABLE_CSV_FILENAME "partitions.csv")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(commander)


