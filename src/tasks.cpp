#include <tasks.h>

bool notify = true;
bool btn_1_pressed = false;
bool btn_2_pressed = false;

void led_nortifier(void* pvArgs)
{
    serial_print("LED Nortifier");
    while (true)
    {
        if(notify)
        {
            digitalWrite(LED, LOW);
            vTaskDelay(500/portTICK_PERIOD_MS);
            digitalWrite(LED, HIGH);
            vTaskDelay(500/portTICK_PERIOD_MS);
        }
        else{
            if(digitalRead(LED))
            {
                digitalWrite(LED, LOW);
            }
        }
    }
}

void btn_intrupt(void* pvArgs)
{
    serial_print("BTN intrupt started");
    while(true)
    {
        if(digitalRead(BTN1) == STATE)
        {
            if(!btn_1_pressed)
            {
                btn_1_pressed = true;
                serial_print("BUTTON 1 Pressed");
            }
        }
        else
        {
            if(btn_1_pressed)
            {
                btn_1_pressed = false;
                serial_print("BUTTON 1 Released");
                vTaskDelay(10/portTICK_PERIOD_MS);
            }
        }
        if(digitalRead(BTN2) == STATE)
        {
            if(!btn_2_pressed)
            {
                btn_2_pressed = true;
                serial_print("BUTTON 2 Pressed");
                notify = false;
            }
        }
        else
        {
            if(btn_2_pressed)
            {
                btn_2_pressed = false;
                serial_print("BUTTON 2 Released");
                vTaskDelay(10/portTICK_PERIOD_MS);
            }
        }
    }
}

