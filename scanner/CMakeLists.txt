# scanner/CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

# Pull in all shared components from the common pool.
# Must be set BEFORE include(project).
list(APPEND EXTRA_COMPONENT_DIRS ../common_components)

# Exclude Commander-only components that need Bluetooth
set(EXCLUDE_COMPONENTS
    bt_media_action
    dummy_action
    action_registry
    rule_engine
    command_parser
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(scanner)