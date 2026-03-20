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
