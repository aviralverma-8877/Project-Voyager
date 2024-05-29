#ifndef support_methods
    #define support_methods
    #include <Arduino.h>
    #include <Ticker.h>
    #include <string.h>
    #include <oled_display.h>
    #include <tasks.h>
#endif
#define DEBUGGING DEBUG

extern Ticker TickerForLedNotification;

void config_gpios();
void serial_print(String msg);
void setupTickers();
void stop_nortify_led();
void nortify_led();