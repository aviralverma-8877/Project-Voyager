#include "oled_display.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DisplayState display_state;

// Legacy compatibility buffer
oled_buffer display_buffer[6];

// ============================================================================
// INITIALIZATION
// ============================================================================

void init_oled() {
    // Initialize legacy buffer
    for(int i = 0; i < 6; i++) {
        display_buffer[i].msg = "";
    }

    // Initialize display state
    display_state.mode = MODE_SPLASH;
    display_state.wifi_status = WIFI_DISCONNECTED;
    display_state.lora_status = LORA_IDLE;
    display_state.mqtt_connected = false;
    display_state.signal_strength = 0;
    display_state.last_update = millis();

    serial_print("Initializing OLED display");
    Wire.begin(SDA, SCL);

    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        serial_print("ERROR: SSD1306 allocation failed");
        for(;;) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    display.clearDisplay();
    display.setTextWrap(false);
    show_boot_animation();
}

void clear_oled_display() {
    display.clearDisplay();
}

// ============================================================================
// HIGH-LEVEL DISPLAY FUNCTIONS
// ============================================================================

void show_splash_screen() {
    display_state.mode = MODE_SPLASH;
    clear_oled_display();

    // Draw project name large and centered
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);

    const char* project_name = PROJECT_NAME;
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(project_name, 0, 0, &x1, &y1, &w, &h);
    int16_t x = (SCREEN_WIDTH - w) / 2;
    int16_t y = 20;
    display.setCursor(x, y);
    display.print(project_name);

    // Draw version or subtitle
    display.setTextSize(1);
    draw_centered_text("LoRa File Transfer", 42);

    // Draw decorative line
    display.drawLine(10, 55, SCREEN_WIDTH - 10, 55, SSD1306_WHITE);

    display.display();
}

void show_status_screen() {
    display_state.mode = MODE_STATUS;
    clear_oled_display();

    draw_header();
    draw_status_bar();

    // Content area starts at y=14
    int16_t y = CONTENT_Y_START;
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // Line 1: WiFi status
    if(display_state.wifi_status == WIFI_CONNECTED_AP) {
        display.setCursor(MARGIN_LEFT, y);
        display.print("Mode: AP");
    } else if(display_state.wifi_status == WIFI_CONNECTED_STA) {
        display.setCursor(MARGIN_LEFT, y);
        display.print("Mode: STA");
    } else if(display_state.wifi_status == WIFI_CONNECTING) {
        display.setCursor(MARGIN_LEFT, y);
        display.print("Connecting...");
    } else {
        display.setCursor(MARGIN_LEFT, y);
        display.print("WiFi: Off");
    }
    y += LINE_HEIGHT;

    // Line 2: SSID or IP
    if(!display_state.line1.isEmpty()) {
        display.setCursor(MARGIN_LEFT, y);
        display.print(display_state.line1.substring(0, 20)); // Limit length
    }
    y += LINE_HEIGHT;

    // Line 3: IP Address or additional info
    if(!display_state.line2.isEmpty()) {
        display.setCursor(MARGIN_LEFT, y);
        display.print(display_state.line2.substring(0, 20));
    }
    y += LINE_HEIGHT;

    // Line 4: Status message
    if(!display_state.status_message.isEmpty()) {
        display.setCursor(MARGIN_LEFT, y);
        display.print(display_state.status_message.substring(0, 20));
    }

    display.display();
}

void show_connecting_screen(const char* message, uint8_t progress) {
    display_state.mode = MODE_CONNECTING;
    clear_oled_display();

    draw_header();

    // Message
    display.setTextSize(1);
    draw_centered_text(message, 20);

    // Progress bar
    draw_progress_bar(10, 35, SCREEN_WIDTH - 20, 8, progress);

    // Percentage
    char percent_text[8];
    snprintf(percent_text, sizeof(percent_text), "%d%%", progress);
    draw_centered_text(percent_text, 48);

    display.display();
}

void show_error_screen(const char* error_message) {
    display_state.mode = MODE_ERROR;
    clear_oled_display();

    // Draw error icon (X in circle)
    display.drawCircle(SCREEN_WIDTH / 2, 18, 12, SSD1306_WHITE);
    display.drawLine(SCREEN_WIDTH / 2 - 6, 18 - 6, SCREEN_WIDTH / 2 + 6, 18 + 6, SSD1306_WHITE);
    display.drawLine(SCREEN_WIDTH / 2 - 6, 18 + 6, SCREEN_WIDTH / 2 + 6, 18 - 6, SSD1306_WHITE);

    // Error text
    display.setTextSize(1);
    draw_centered_text("ERROR", 36);
    draw_wrapped_text(error_message, MARGIN_LEFT, 48, SCREEN_WIDTH - MARGIN_LEFT - MARGIN_RIGHT);

    display.display();
}

void show_message_screen(const char* title, const char* message) {
    display_state.mode = MODE_MESSAGE;
    clear_oled_display();

    // Title
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    draw_centered_text(title, 5);

    // Divider line
    display.drawLine(5, 16, SCREEN_WIDTH - 5, 16, SSD1306_WHITE);

    // Message
    draw_wrapped_text(message, MARGIN_LEFT, 20, SCREEN_WIDTH - MARGIN_LEFT - MARGIN_RIGHT);

    display.display();
}

// ============================================================================
// STATUS UPDATE FUNCTIONS
// ============================================================================

void set_wifi_status(WiFiStatus status, const char* ssid, const char* ip) {
    display_state.wifi_status = status;
    if(ssid != nullptr) display_state.line1 = ssid;
    if(ip != nullptr) display_state.line2 = ip;
    display_state.last_update = millis();
}

void set_lora_status(LoRaStatus status) {
    display_state.lora_status = status;
    display_state.last_update = millis();
}

void set_mqtt_status(bool connected) {
    display_state.mqtt_connected = connected;
    display_state.last_update = millis();
}

void set_status_message(const char* message) {
    display_state.status_message = message;
    display_state.last_update = millis();
}

void set_signal_strength(uint8_t strength) {
    display_state.signal_strength = (strength > 4) ? 4 : strength;
    display_state.last_update = millis();
}

void update_display() {
    // Refresh current screen mode
    switch(display_state.mode) {
        case MODE_STATUS:
            show_status_screen();
            break;
        case MODE_SPLASH:
            show_splash_screen();
            break;
        default:
            break;
    }
}

// ============================================================================
// DRAWING HELPER FUNCTIONS
// ============================================================================

void draw_header() {
    // Draw project name in header
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(MARGIN_LEFT, 2);
    display.print(PROJECT_NAME);

    // Draw header separator line
    display.drawLine(0, HEADER_HEIGHT, SCREEN_WIDTH, HEADER_HEIGHT, SSD1306_WHITE);
}

void draw_status_bar() {
    int16_t icon_y = SCREEN_HEIGHT - STATUS_BAR_HEIGHT + 1;
    int16_t icon_spacing = 20;
    int16_t x_pos = MARGIN_LEFT;

    // Draw status bar background line
    display.drawLine(0, SCREEN_HEIGHT - STATUS_BAR_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - STATUS_BAR_HEIGHT, SSD1306_WHITE);

    // WiFi icon
    draw_wifi_icon(x_pos, icon_y, display_state.wifi_status);
    x_pos += icon_spacing;

    // LoRa icon
    draw_lora_icon(x_pos, icon_y, display_state.lora_status);
    x_pos += icon_spacing;

    // MQTT icon
    if(display_state.mqtt_connected) {
        draw_mqtt_icon(x_pos, icon_y, display_state.mqtt_connected);
        x_pos += icon_spacing;
    }

    // Signal strength on the right
    draw_signal_bars(SCREEN_WIDTH - 18, icon_y, display_state.signal_strength);
}

void draw_wifi_icon(int16_t x, int16_t y, WiFiStatus status) {
    // WiFi signal icon (simplified)
    if(status == WIFI_DISCONNECTED) {
        // Draw X
        display.drawLine(x, y, x + 6, y + 6, SSD1306_WHITE);
        display.drawLine(x, y + 6, x + 6, y, SSD1306_WHITE);
    } else if(status == WIFI_CONNECTING) {
        // Draw blinking dot
        if((millis() / 500) % 2 == 0) {
            display.fillCircle(x + 3, y + 3, 2, SSD1306_WHITE);
        }
    } else {
        // Draw WiFi waves (AP or STA)
        display.drawCircle(x + 3, y + 6, 2, SSD1306_WHITE);
        display.drawLine(x + 1, y + 4, x + 5, y + 4, SSD1306_WHITE);
        display.drawPixel(x, y + 2, SSD1306_WHITE);
        display.drawPixel(x + 6, y + 2, SSD1306_WHITE);

        // Add "A" for AP mode
        if(status == WIFI_CONNECTED_AP) {
            display.setTextSize(1);
            display.setCursor(x + 8, y);
            display.print("A");
        }
    }
}

void draw_lora_icon(int16_t x, int16_t y, LoRaStatus status) {
    // LoRa antenna icon
    if(status == LORA_ERROR) {
        // Draw X
        display.drawLine(x, y, x + 6, y + 6, SSD1306_WHITE);
        display.drawLine(x, y + 6, x + 6, y, SSD1306_WHITE);
    } else if(status == LORA_TRANSMITTING) {
        // Antenna with arrows going out
        display.drawLine(x + 3, y, x + 3, y + 6, SSD1306_WHITE);
        display.drawLine(x, y + 2, x + 3, y, SSD1306_WHITE);
        display.drawLine(x + 6, y + 2, x + 3, y, SSD1306_WHITE);
        display.drawPixel(x + 1, y + 5, SSD1306_WHITE);
        display.drawPixel(x + 5, y + 5, SSD1306_WHITE);
    } else if(status == LORA_RECEIVING) {
        // Antenna with arrows coming in
        display.drawLine(x + 3, y, x + 3, y + 6, SSD1306_WHITE);
        display.drawLine(x, y, x + 3, y + 2, SSD1306_WHITE);
        display.drawLine(x + 6, y, x + 3, y + 2, SSD1306_WHITE);
    } else {
        // Simple antenna
        display.drawLine(x + 3, y, x + 3, y + 6, SSD1306_WHITE);
        display.drawLine(x, y + 2, x + 3, y, SSD1306_WHITE);
        display.drawLine(x + 6, y + 2, x + 3, y, SSD1306_WHITE);
    }
}

void draw_mqtt_icon(int16_t x, int16_t y, bool connected) {
    if(connected) {
        // Draw "M" for MQTT
        display.setTextSize(1);
        display.setCursor(x, y);
        display.print("M");
    }
}

void draw_signal_bars(int16_t x, int16_t y, uint8_t strength) {
    // Draw signal bars (0-4)
    int16_t bar_width = 2;
    int16_t bar_spacing = 3;

    for(uint8_t i = 0; i < 4; i++) {
        int16_t bar_height = (i + 1) * 2;
        int16_t bar_x = x + (i * bar_spacing);
        int16_t bar_y = y + 6 - bar_height;

        if(i < strength) {
            display.fillRect(bar_x, bar_y, bar_width, bar_height, SSD1306_WHITE);
        } else {
            display.drawRect(bar_x, bar_y, bar_width, bar_height, SSD1306_WHITE);
        }
    }
}

void draw_progress_bar(int16_t x, int16_t y, int16_t width, int16_t height, uint8_t progress) {
    // Draw border
    display.drawRect(x, y, width, height, SSD1306_WHITE);

    // Draw fill
    if(progress > 100) progress = 100;
    int16_t fill_width = ((width - 2) * progress) / 100;
    if(fill_width > 0) {
        display.fillRect(x + 1, y + 1, fill_width, height - 2, SSD1306_WHITE);
    }
}

void draw_centered_text(const char* text, int16_t y, uint8_t size) {
    display.setTextSize(size);
    display.setTextColor(SSD1306_WHITE);

    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    int16_t x = (SCREEN_WIDTH - w) / 2;

    display.setCursor(x, y);
    display.print(text);
}

void draw_wrapped_text(const char* text, int16_t x, int16_t y, int16_t max_width, uint8_t size) {
    display.setTextSize(size);
    display.setTextColor(SSD1306_WHITE);

    String str = text;
    int16_t cursor_x = x;
    int16_t cursor_y = y;
    int16_t char_width = 6 * size;
    int16_t line_height = 8 * size;

    for(size_t i = 0; i < str.length(); i++) {
        if(cursor_x + char_width > x + max_width) {
            cursor_x = x;
            cursor_y += line_height;
        }

        if(cursor_y + line_height > SCREEN_HEIGHT) break; // Stop if out of bounds

        display.setCursor(cursor_x, cursor_y);
        display.print(str.charAt(i));
        cursor_x += char_width;
    }
}

// ============================================================================
// ANIMATION FUNCTIONS
// ============================================================================

void show_boot_animation() {
    clear_oled_display();

    // Show project name with fade-in effect (simplified)
    for(int frame = 0; frame < 3; frame++) {
        clear_oled_display();

        // Draw project name
        display.setTextSize(2);
        display.setTextColor(SSD1306_WHITE);
        const char* project_name = PROJECT_NAME;
        int16_t x1, y1;
        uint16_t w, h;
        display.getTextBounds(project_name, 0, 0, &x1, &y1, &w, &h);
        int16_t x = (SCREEN_WIDTH - w) / 2;
        display.setCursor(x, 20);
        display.print(project_name);

        // Draw expanding circle
        int16_t radius = 5 + (frame * 8);
        display.drawCircle(SCREEN_WIDTH / 2, 45, radius, SSD1306_WHITE);

        display.display();
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    vTaskDelay(800 / portTICK_PERIOD_MS);
}

void show_connecting_animation() {
    static uint8_t frame = 0;
    const char* dots[] = {"   ", ".  ", ".. ", "..."};

    char message[32];
    snprintf(message, sizeof(message), "Connecting%s", dots[frame % 4]);

    show_connecting_screen(message, (frame * 10) % 100);
    frame++;
}

// ============================================================================
// LEGACY COMPATIBILITY FUNCTION
// ============================================================================

void display_text_oled() {
    serial_print("Displaying text on OLED (legacy mode)");
    clear_oled_display();

    int size = 2;
    int dist = 20;
    int row = 0;

    for(int i = 0; i < 6; i++) {
        String msg = display_buffer[i].msg;

        if(i == 1) {
            size = 1;
            dist = 12;
        }

        display.setTextSize(size);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, row);
        display.cp437(false);
        display.write(msg.c_str());
        row += dist;
    }

    display.display();
}
