// Sensor broadcast mode: reads the sensors wired to the spare GPIOs and
// broadcasts the readings over LoRa at a fixed interval.
//
// While the mode is enabled the LoRa-to-serial bridge and file transfer are
// off (core and web_server check sensor_mode_enabled()); chat messages still
// work because sensor packets go through the same send_packets queue.
#ifndef SENSOR_SUPPORT_H
#define SENSOR_SUPPORT_H
    #include <Arduino.h>

    #define SENSOR_CONFIG_PATH "/config/sensor_config.json"

    // Loads sensor_config.json (missing / invalid -> disabled) and starts the
    // sensor task. Call once from setup(), after the queues exist.
    void sensors_init();

    // True while sensor broadcast mode is on. Cheap, safe from any task.
    bool sensor_mode_enabled();

    // Validates (ranges, pins free on this board), saves atomically and applies
    // at once. Returns false and sends an `alert` when the input is invalid;
    // the file is not changed in that case.
    bool sensor_save_config(const char* json);

    // Sends WS `sensor_config`: the active config plus this board's usable pins.
    void sensor_send_config_to_ws();
#endif
