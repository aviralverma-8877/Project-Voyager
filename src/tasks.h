#ifndef tasks
    #define tasks
    #include<Arduino.h>
    #include<support_method.h>
    #define ESP_INTR_FLAG_DEFAULT 0
    #define BUTTON1 BTN1
    #define BUTTON2 BTN2

    extern bool notify;
    extern bool btn_1_pressed;
    extern bool btn_2_pressed;

    void ping_mqtt_timer(void *param);
    void wifi_connection_check();
    void init_isr(int btn);
    void led_nortifier(void *param);
    void btn_intrupt(void *param);
    void transmit_beacon();
    void get_heap_info(void* params);
#endif
