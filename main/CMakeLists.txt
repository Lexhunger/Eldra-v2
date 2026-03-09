idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        eldra_display_round
        eldra_sensors
        eldra_eyes
        eldra_emotion
        eldra_comms
        eldra_logging
        eldra_sd
        eldra_cloud
        eldra_sleep
        wifi_driver
        sd_driver
        config_store
        rtc_driver
        imu_qmi8658
        exio
        runtime_monitor
        console_jobs
        console_wifi_bridge
        console_sd_bridge
        console_rtc_bridge
        console_imu_bridge
        console_emotion_bridge
)
