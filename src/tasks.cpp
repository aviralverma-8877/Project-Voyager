#include <tasks.h>

bool notify = true;
bool btn_1_pressed = false;
bool btn_2_pressed = false;

void config_gpios()
{
    pinMode(LED, OUTPUT);
    pinMode(BTN1, INPUT_PULLDOWN);
    pinMode(BTN2, INPUT_PULLDOWN);
    digitalWrite(LED, HIGH);

    xTaskCreatePinnedToCore(btn_intrupt, "button_input", 2048, NULL, 1, NULL, ARDUINO_EVENT_RUNNING_CORE);
}

void led_nortifier(void* pvArgs)
{
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

void btn_intrupt(void* arg)
{
    while(true)
    {
        if(digitalRead(BTN1) == STATE)
        {
            if(!btn_1_pressed)
            {
                btn_1_pressed = true;
                serial_print("BUTTON 1 Pressed");
                notify = false;
            }
        }
        else
        {
            if(btn_1_pressed)
            {
                serial_print("BUTTON 1 Released");
                btn_1_pressed = false;
                vTaskDelay(100/portTICK_PERIOD_MS);
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
                vTaskDelay(100/portTICK_PERIOD_MS);
            }
        }

    }
}

