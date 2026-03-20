idf_component_register(
    SRCS
        "main.c"
        "ble_log.c"
        "mr60bha1.c"
        "mr60bha2.c"
        "matter_endpoint.cpp"
        "vital_cluster.cpp"
    INCLUDE_DIRS "."
)

# --wrap=nimble_port_init: unser __wrap_nimble_port_init() läuft nach
# dem echten nimble_port_init() und registriert den NUS-GATT-Service
# im einzig sicheren Fenster vor ble_gatts_start().
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=nimble_port_init"
)
