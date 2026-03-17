idf_component_register(
                SRCS "main.c"
                    "../helpers/monitor.c"
                    "../helpers/workkernel.c"
                INCLUDE_DIRS "." "../helpers"
                REQUIRES driver esp_timer)
