#ifndef support_methods
    #define support_methods
    #include <Arduino.h>
    #include <Ticker.h>
    #include <string.h>
    #include <oled_display.h>
    #include <tasks.h>
    #include <ESPmDNS.h>
    #include <DNSServer.h>
    #include <WiFi.h>
    extern DNSServer dnsServer;
    extern Ticker TickerForLedNotification;

    void dns_request_process();
    void setup_dns();
    void config_gpios();
    void serial_print(String msg);
    void setupTickers();
    void stop_nortify_led();
    void nortify_led();
    #define DEBUGGING DEBUG
#endif



