#include <tasks.h>

bool notify = false;

void async_led_notifier(void *param)
{
    while(true)
    {
        if(notify)
        {
            led_nortifier();
        }
        notify = false;
        vTaskDelay(1/portTICK_PERIOD_MS);
    }
}

void led_nortifier()
{
    if(digitalRead(LED))
    {
        digitalWrite(LED, LOW);
        vTaskDelay(NOTIFY_LED_DELAY/portTICK_PERIOD_MS);
        digitalWrite(LED, HIGH);
    }
    else
    {
        digitalWrite(LED, HIGH);
        vTaskDelay(NOTIFY_LED_DELAY/portTICK_PERIOD_MS);
        digitalWrite(LED, LOW);
    }
}
