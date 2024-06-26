#ifndef tasks
    #define tasks
    #include<Arduino.h>
    #include<support_method.h>
    #include <Ticker.h>
    #define ESP_INTR_FLAG_DEFAULT 0
    #define BUTTON1 BTN1
    #define BUTTON2 BTN2

    void ping_mqtt_timer();
    void wifi_connection_check();
    void init_isr(int btn);
    void led_nortifier();
    void btn_intrupt();
    void transmit_beacon();
#endif
