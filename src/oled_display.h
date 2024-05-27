#ifndef oled_display
    #define oled_display
    
    //OLED SSD1306 dependent libraries.
    #include <SPI.h>
    #include <Wire.h>
    #include <Adafruit_GFX.h>
    #include <Adafruit_SSD1306.h>

    #define SCREEN_WIDTH 128 // OLED display width, in pixels
    #define SCREEN_HEIGHT 64 // OLED display height, in pixels

    #define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
    #define SCREEN_ADDRESS 0x3D ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
#endif

#ifndef arduino_framework
    #include<Arduino.h>
#endif

extern Adafruit_SSD1306 display;
void init_oled();
