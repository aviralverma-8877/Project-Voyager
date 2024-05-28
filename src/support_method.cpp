#include <support_method.h>

void serial_print(String msg)
{
    if(DEBUGGING)
    {
        // display_text_oled(msg, 0, 10);
        Serial.println(msg);
    }
}