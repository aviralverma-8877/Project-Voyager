#include "oled_display.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void init_oled()
{
    serial_print("initilizing oled display.");
    Wire.begin(SDA,SCL);
    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) 
    {
        serial_print("SSD1306 allocation failed");
        for(;;); // Don't proceed, loop forever
    }
    display.clearDisplay();
    delay(1000);
    show_splash();
}

void show_splash()
{
    serial_print("Showing splash screen.");
    display.setTextSize(1);      // Normal 1:1 pixel scale
    display.setTextColor(SSD1306_WHITE); // Draw white text
    display.setCursor(0, 0);     // Start at top-left corner
    display.cp437(true); 
    display.write("Hello World.");
    display.display();
}