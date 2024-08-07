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
    int size = 2;
    int dist = 16;
    int row = 0;
    for(int i=0; i<6; i++)
    {
        String msg = display_buffer[i].msg;
        if(i == 1)
        {
            size = 1;
            dist = 10;
        }
        display.setTextSize(size);
        display.setTextColor(SSD1306_WHITE); // Draw white text
        display.setCursor(0, row);     // Start at top-left corner
        display.cp437(false); 
        display.write(msg.c_str());
        row += dist;
    }
    display.display();
}

void show_splash()
{
    display_buffer[0].msg = PROJECT_NAME;
    display_text_oled();
}