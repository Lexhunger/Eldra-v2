idf_component_register(
    SRCS "console_imu.c"
    INCLUDE_DIRS "include"
    REQUIRES console imu_qmi8658 freertos
)
