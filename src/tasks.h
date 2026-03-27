#ifndef tasks
    #define tasks
    #include<Arduino.h>
    #include<support_method.h>
    #define ESP_INTR_FLAG_DEFAULT 0

    extern bool notify;

    void async_led_notifier(void *param);
    void led_nortifier();
#endif
