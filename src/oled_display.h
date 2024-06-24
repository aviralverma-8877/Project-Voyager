#ifndef oled_display
    #define oled_display
    #include <Arduino.h>
    #include <support_method.h>
    //OLED SSD1306 dependent libraries.
    #include <SPI.h>
    #include <Wire.h>
    #include <Adafruit_GFX.h>
    #include <Adafruit_SSD1306.h>
    #define SCREEN_WIDTH 128 // OLED display width, in pixels
    #define SCREEN_HEIGHT 64 // OLED display height, in pixels

    #define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
    #define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
    #define SCL OSCL
    #define SDA OSDA

    struct oled_buffer{
        String msg = "";
    };
    extern oled_buffer display_buffer[6];
    void init_oled();
    void show_splash();
    void clear_oled_display();
    void display_text_oled();
#endif