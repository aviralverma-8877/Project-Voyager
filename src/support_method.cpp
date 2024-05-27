#include <support_method.h>

void serial_print(String msg)
{
    if(DEBUGGING)
        Serial.println(msg);
}