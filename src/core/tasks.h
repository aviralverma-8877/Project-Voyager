#ifndef TASKS_H
#define TASKS_H
    #include <Arduino.h>
    #include "support_method.h"

    extern TaskHandle_t led_task_handle;

    void async_led_notifier(void *param);
    void led_nortifier();          // non-blocking, any task (and ISR-safe)
    void btn_intrupt(void *param); // BTN1 hold-2s SOS, BTN2 short/long -> oled_button
    void get_heap_info(void* params);
    void get_wifi_rssi(void* params);
#endif
