#include <tasks.h>

bool notify = true;
bool btn_1_pressed = false;
bool btn_2_pressed = false;

void wifi_connection_check()
{
    if(WiFi.status() != WL_CONNECTED)
    {
        config_wifi();
    }
}

void ping_mqtt_timer()
{
    String mac = WiFi.macAddress();
    JsonDocument doc;
    doc["mac"] = mac;
    doc["uname"] = username;
    String mqtt_ping;
    serializeJson(doc, mqtt_ping);
    ping_mqtt(mqtt_ping);
}

void led_nortifier()
{
    if(digitalRead(LED))
    {
        digitalWrite(LED, LOW);
    }
    else
    {
        digitalWrite(LED, HIGH);
    }
}

void btn_intrupt()
{
    if(digitalRead(BTN1) == STATE)
    {
        if(!btn_1_pressed)
        {
            btn_1_pressed = true;
            serial_print("BUTTON 1 Pressed");
            stop_nortify_led();
        }
    }
    else
    {
        if(btn_1_pressed)
        {
            serial_print("BUTTON 1 Released");
            btn_1_pressed = false;
        }
    }
    if(digitalRead(BTN2) == STATE)
    {
        if(!btn_2_pressed)
        {
            btn_2_pressed = true;
            serial_print("BUTTON 2 Pressed");
        }
    }
    else
    {
        if(btn_2_pressed)
        {
            serial_print("BUTTON 2 Released");
            btn_2_pressed = false;
        }
    }
}

