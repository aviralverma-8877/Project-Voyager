#include "oled_display.h"
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
void init_oled()
{
    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) 
    {
        for(;;); // Don't proceed, loop forever
    }
    display.display();
    delay(2000);
    display.clearDisplay();
}