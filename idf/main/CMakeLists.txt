idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES core modem notify forwarder web sched
)
