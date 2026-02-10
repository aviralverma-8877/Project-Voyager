#ifndef oled_display
    #define oled_display
    #include <Arduino.h>
    #include <support_method.h>
    #include <SPI.h>
    #include <Wire.h>
    #include <Adafruit_GFX.h>
    #include <Adafruit_SSD1306.h>

    // Display dimensions
    #define SCREEN_WIDTH 128
    #define SCREEN_HEIGHT 64
    #define OLED_RESET -1
    #define SCREEN_ADDRESS 0x3C
    #define SCL OSCL
    #define SDA OSDA

    // Layout constants
    #define HEADER_HEIGHT 12
    #define STATUS_BAR_HEIGHT 10
    #define CONTENT_Y_START (HEADER_HEIGHT + 2)
    #define LINE_HEIGHT 12
    #define MARGIN_LEFT 2
    #define MARGIN_RIGHT 2

    // Display modes
    enum DisplayMode {
        MODE_SPLASH,
        MODE_STATUS,
        MODE_CONNECTING,
        MODE_ERROR,
        MODE_MESSAGE
    };

    // Status indicators
    enum WiFiStatus {
        WIFI_DISCONNECTED,
        WIFI_CONNECTING,
        WIFI_CONNECTED_AP,
        WIFI_CONNECTED_STA
    };

    enum LoRaStatus {
        LORA_IDLE,
        LORA_TRANSMITTING,
        LORA_RECEIVING,
        LORA_ERROR
    };

    struct DisplayState {
        DisplayMode mode;
        WiFiStatus wifi_status;
        LoRaStatus lora_status;
        bool mqtt_connected;
        String title;
        String line1;
        String line2;
        String line3;
        String line4;
        String status_message;
        uint8_t signal_strength; // 0-4
        unsigned long last_update;
    };

    // Global display state
    extern DisplayState display_state;
    extern Adafruit_SSD1306 display;

    // Core functions
    void init_oled();
    void update_display();
    void clear_oled_display();

    // High-level display functions
    void show_splash_screen();
    void show_status_screen();
    void show_connecting_screen(const char* message, uint8_t progress);
    void show_error_screen(const char* error_message);
    void show_message_screen(const char* title, const char* message);

    // Status update functions
    void set_wifi_status(WiFiStatus status, const char* ssid = nullptr, const char* ip = nullptr);
    void set_lora_status(LoRaStatus status);
    void set_mqtt_status(bool connected);
    void set_status_message(const char* message);
    void set_signal_strength(uint8_t strength); // 0-4 bars

    // Drawing helper functions
    void draw_header();
    void draw_status_bar();
    void draw_wifi_icon(int16_t x, int16_t y, WiFiStatus status);
    void draw_lora_icon(int16_t x, int16_t y, LoRaStatus status);
    void draw_mqtt_icon(int16_t x, int16_t y, bool connected);
    void draw_signal_bars(int16_t x, int16_t y, uint8_t strength);
    void draw_progress_bar(int16_t x, int16_t y, int16_t width, int16_t height, uint8_t progress);
    void draw_centered_text(const char* text, int16_t y, uint8_t size = 1);
    void draw_wrapped_text(const char* text, int16_t x, int16_t y, int16_t max_width, uint8_t size = 1);

    // Animation functions
    void show_boot_animation();
    void show_connecting_animation();

    // Legacy compatibility (for gradual migration)
    struct oled_buffer {
        String msg = "";
    };
    extern oled_buffer display_buffer[6];
    void display_text_oled(); // Legacy function

#endif
