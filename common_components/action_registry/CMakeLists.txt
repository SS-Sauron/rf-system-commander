# common_components/action_registry/CMakeLists.txt

idf_component_register(
    SRCS "action_registry.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        output    # output_write() for the C3 stub dispatch log
)

component_compile_options(-Wvla -Werror=vla)
