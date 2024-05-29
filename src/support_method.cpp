#include <support_method.h>

Ticker TickerForBtnPresses;
Ticker TickerForLedNotification;

void config_gpios()
{
    pinMode(LED, OUTPUT);
    pinMode(BTN1, INPUT_PULLDOWN);
    pinMode(BTN2, INPUT_PULLDOWN);
    digitalWrite(LED, LOW);
    setupTickers();
}

void serial_print(String msg)
{
    if(DEBUGGING)
    {
        // display_text_oled(msg, 0, 10);
        Serial.println(msg);
    }
}

void setupTickers()
{
    TickerForBtnPresses.attach_ms(10, btn_intrupt);
}

void stop_nortify_led()
{
    TickerForLedNotification.detach();
    digitalWrite(LED, LOW);
}

void nortify_led()
{
    serial_print("Nortify");
    TickerForLedNotification.attach_ms(500, led_nortifier);
}