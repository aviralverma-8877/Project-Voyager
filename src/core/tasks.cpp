#include "tasks.h"
#include "core_dispatch.h"
#include <web/web_sockets.h>

TaskHandle_t led_task_handle = NULL;

// ------------------------------------------------------------------- LED

// Blinks once per notification. Notifications that arrive during a blink are
// coalesced (ulTaskNotifyTake clears the count), so a burst gives one blink.
void async_led_notifier(void *param)
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int level = digitalRead(LED);
        digitalWrite(LED, !level);
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_LED_DELAY));
        digitalWrite(LED, level);
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_LED_DELAY));   // visible gap between blinks
    }
}

// Non-blocking: just signals the LED task. Safe from any task or ISR.
void led_nortifier()
{
    TaskHandle_t h = led_task_handle;
    if (h == NULL) return;
    if (xPortInIsrContext())
    {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(h, &woken);
        if (woken) portYIELD_FROM_ISR();
    }
    else
    {
        xTaskNotifyGive(h);
    }
}

// --------------------------------------------------------------- buttons

static const char* SOS_TITLE = "Hold for SOS";

static void send_sos()
{
    JsonDocument inner;
    inner["pack_type"] = "action";
    inner["data"] = "sos";
    String inner_str;
    serializeJson(inner, inner_str);

    JsonDocument doc;
    doc["name"] = get_username();
    doc["mac"] = WiFi.macAddress();
    doc["data"] = inner_str;
    String payload;
    serializeJson(doc, payload);

    QueueParam* p = new (std::nothrow) QueueParam();
    if (!p)
    {
        oled_alert("SOS", "failed: no memory", 3000);
        return;
    }
    p->type = LORA_MSG;
    p->message = payload;
    p->id = 0;   // device-originated: no tx_result
    if (send_packets == NULL || xQueueSend(send_packets, (void*)&p, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        delete p;
        serial_print("SOS: send queue full");
        oled_alert("SOS", "failed: queue full", 3000);
        return;
    }
    serial_print("SOS queued");
    oled_alert("SOS", "sent", 3000);
}

static inline bool btn_level(int pin)
{
    return digitalRead(pin) == STATE;
}

// Polls both buttons every 10 ms through the debounce/hold state machine in
// core_dispatch.h. BTN1: hold 2 s -> SOS (progress shown, early release cancels).
// BTN2: short -> oled_button(2,false), >= 1 s -> oled_button(2,true).
void btn_intrupt(void *param)
{
    CoreButton b1(CORE_SOS_HOLD_MS);
    CoreButton b2(CORE_BTN2_LONG_MS);
    uint8_t shown_step = 255;   // throttles oled_progress to 10% steps
    while (true)
    {
        uint32_t now = millis();

        BtnOutput o1 = b1.update(btn_level(BTN1), now);
        switch (o1.ev)
        {
        case BtnEvent::Pressed:
        case BtnEvent::Progress:
        {
            uint8_t step = o1.pct / 10;
            if (step != shown_step)
            {
                shown_step = step;
                oled_progress(SOS_TITLE, o1.pct);
            }
            break;
        }
        case BtnEvent::HoldFired:
            shown_step = 255;
            oled_progress(SOS_TITLE, 255);
            serial_print("BUTTON 1 held: SOS");
            send_sos();
            break;
        case BtnEvent::ShortRelease:
            shown_step = 255;
            oled_progress(SOS_TITLE, 255);
            serial_print("BUTTON 1 short press (SOS not sent)");
            oled_button(1, false);   // dismiss alert / step messages (Contract 7)
            break;
        case BtnEvent::LongRelease:
        case BtnEvent::None:
            break;
        }

        BtnOutput o2 = b2.update(btn_level(BTN2), now);
        if (o2.ev == BtnEvent::ShortRelease)
            oled_button(2, false);
        else if (o2.ev == BtnEvent::HoldFired)
            oled_button(2, true);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ------------------------------------------------------------- telemetry

// WIFI_DATA (Contract 4): STA connected -> {"mode":"STA","rssi":dBm};
// otherwise, if the AP is up -> {"mode":"AP","clients":n} (WiFi.RSSI() is
// STA-only and reads 0 in AP mode); else {"mode":"OFF"}.
void get_wifi_rssi(void* params)
{
    while (true)
    {
        bool sta = WiFi.status() == WL_CONNECTED;
        int rssi = sta ? WiFi.RSSI() : 0;
        if (sta)
            oled_set_wifi_rssi(rssi);
        if (events_client_count() > 0)
        {
            JsonDocument doc;
            if (sta)
            {
                doc["mode"] = "STA";
                doc["rssi"] = rssi;
            }
            else if (WiFi.getMode() & WIFI_MODE_AP)
            {
                doc["mode"] = "AP";
                doc["clients"] = WiFi.softAPgetStationNum();
            }
            else
            {
                doc["mode"] = "OFF";
            }
            String data;
            serializeJson(doc, data);
            send_to_events(data, "WIFI_DATA");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void get_heap_info(void* params)
{
    while (true)
    {
        if (events_client_count() > 0)
        {
            JsonDocument doc;
            doc["free_heap"] = ESP.getFreeHeap();
            doc["heap_size"] = ESP.getHeapSize();
            String data;
            serializeJson(doc, data);
            send_to_events(data, "RAM_DATA");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
