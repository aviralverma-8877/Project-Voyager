#include "oled_display.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

oled_buffer display_buffer[6];

void init_oled()
{
    for(int i=0; i<(sizeof(display_buffer)/sizeof(oled_buffer)); i++)
        display_buffer[i].msg = "";
    serial_print("initilizing oled display.");
    Wire.begin(SDA,SCL);
    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) 
    {
        serial_print("SSD1306 allocation failed");
        for(;;); // Don't proceed, loop forever
    }
    display.clearDisplay();
    show_splash();
}

void clear_oled_display()
{
    display.clearDisplay();
}

void display_text_oled()
{
    clear_oled_display();
    for(int i=0; i<6; i++)
    {
        int row = (i*10);
        String msg = display_buffer[i].msg;
        display.setTextSize(1);      // Normal 1:1 pixel scale
        display.setTextColor(SSD1306_WHITE); // Draw white text
        display.setCursor(0, row);     // Start at top-left corner
        display.cp437(false); 
        display.write(msg.c_str());
        display.display();
    }
}

void show_splash()
{
    display_buffer[0].msg = "ESP32 LORA";
    display_text_oled();
}