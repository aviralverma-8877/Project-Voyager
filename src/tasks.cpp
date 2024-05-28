#include <tasks.h>

bool btn_1_pressed = false;
bool btn_2_pressed = false;

void led_nortifier(void* pvArgs)
{
    while (true)
    {
        digitalWrite(LED, LOW);
        vTaskDelay(500/portTICK_PERIOD_MS);
        digitalWrite(LED, HIGH);
        vTaskDelay(500/portTICK_PERIOD_MS);
    }
}

void btn_1_intrupt(void* pvArgs)
{
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
                vTaskDelay(10/portTICK_PERIOD_MS);
            }
        }
    }
}

void btn_2_intrupt(void* pvArgs)
{
    while(true)
    {
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
                btn_2_pressed = false;
                vTaskDelay(10/portTICK_PERIOD_MS);
            }
        }
    }
}