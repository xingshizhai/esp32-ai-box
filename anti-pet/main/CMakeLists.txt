idf_component_register(
    SRCS
        "app_role.c"
        "modes/anti_modes.c"
        "modes/mode_benchmark.c"
        "modes/mode_casual.c"
        "modes/mode_defect_hunter.c"
        "modes/mode_pressure.c"
    INCLUDE_DIRS "." "modes"
    REQUIRES app_core role_iface
)
