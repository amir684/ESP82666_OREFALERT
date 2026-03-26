#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <Preferences.h>

// ===================== PIN CONFIG =====================
#define DATA_PIN        3     // GPIO3  -> WS2812 matrix data
#define NUM_LEDS        256   // 4 x 8x8 = 256 LEDs

#define BUTTON_PIN      10     // GPIO10  -> config button (connect to GND)
#define LONG_PRESS_MS   3000

// ===================== CONSTANTS =====================
#define ALERT_URL       "https://www.oref.org.il/warningMessages/alert/Alerts.json"
#define CHECK_INTERVAL  3000UL
#define CAT_PRE_ALARM   10
#define SAFE_TIMEOUT_MS (20UL * 60 * 1000)
#define API_FAIL_MAX    5     // consecutive failures before NO API shown
#define SCROLL_STEP_MS  40    // ms between scroll pixels

// ===================== OBJECTS =====================
CRGB leds[NUM_LEDS];
Preferences preferences;

// ===================== MATRIX MAPPING =====================
void setPixel(int x, int y, CRGB color) {
    if (x < 0 || x >= 32 || y < 0 || y >= 8) return;
    x = 31 - x;
    y = 7  - y;
    int panel = x / 8;
    int vcol  = x % 8;
    int vrow  = 7 - y;
    int pcol  = 7 - vrow;
    int prow  = vcol;
    leds[panel * 64 + prow * 8 + pcol] = color;
}

// ===================== FONT =====================
const char    FONT_KEYS[]    = "ABCDEFHIKLMNOPRSTUYW. ";
const uint8_t FONT_DATA[][5] = {
    {0x3F,0x48,0x48,0x48,0x3F}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x41,0x3E}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x48,0x48,0x48,0x40}, // F
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x41,0x41,0x7F,0x41,0x41}, // I
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x01,0x01,0x01,0x01}, // L
    {0x7F,0x60,0x10,0x60,0x7F}, // M
    {0x7F,0x20,0x10,0x08,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x48,0x48,0x48,0x30}, // P
    {0x7F,0x48,0x4C,0x4A,0x31}, // R
    {0x31,0x49,0x49,0x49,0x46}, // S
    {0x40,0x40,0x7F,0x40,0x40}, // T
    {0x7E,0x01,0x01,0x01,0x7E}, // U
    {0x60,0x18,0x07,0x18,0x60}, // Y
    {0x7F,0x04,0x18,0x04,0x7F}, // W
    {0x00,0x00,0x03,0x00,0x00}, // .
    {0x00,0x00,0x00,0x00,0x00}, // (space)
};

const uint8_t* getGlyph(char c) {
    const char* p = strchr(FONT_KEYS, c);
    if (!p) p = FONT_KEYS + sizeof(FONT_KEYS) - 2; // fallback: space
    return FONT_DATA[p - FONT_KEYS];
}

void drawText(const char* text, int xOffset, CRGB color) {
    FastLED.clear();
    for (int i = 0; text[i]; i++) {
        int cx = xOffset + i * 7;   // step=7 to fit the extra bold pixel
        const uint8_t* g = getGlyph(text[i]);
        for (int col = 0; col < 5; col++) {
            uint8_t data = g[col];
            for (int row = 0; row < 7; row++) {
                if (data & (1 << row)) {
                    setPixel(cx + col,     row, color);
                    setPixel(cx + col + 1, row, color); // bold: duplicate right
                }
            }
        }
    }
    FastLED.show();
}

// ===================== STATE =====================
enum AlertState { SAFE, PRE_ALARM, ALARM, UNSAFE, NO_API, BAD_CITY };

volatile AlertState currentState = SAFE;
SemaphoreHandle_t   stateMutex;

volatile int  apiFailCount = 0;
volatile bool cityValid    = true;

String cityName = "רמת השרון";

unsigned long lastAlertTime = 0;

// Flicker / blink state (loop task only)
unsigned long lastBlink = 0;
bool          blinkOn   = false;

// ===================== DISPLAY HELPERS =====================
CRGB currentColor = CRGB::Black;

// Scroll state
char scrollText[64] = "";
int  scrollOffset   = 32;
bool isScrolling    = false;
unsigned long lastScrollStep = 0;

void setColor(uint8_t r, uint8_t g, uint8_t b) {
    currentColor = CRGB(r, g, b);
}

// Draw text centered on the 32px-wide matrix
void showStatic(const char* text) {
    isScrolling = false;
    int len    = strlen(text);
    int totalW = len * 7 - 1;
    int x      = (32 - totalW) / 2;
    drawText(text, x, currentColor);
}

// Queue text for scrolling (driven by loop)
void showScroll(const char* text) {
    strncpy(scrollText, text, sizeof(scrollText) - 1);
    scrollText[sizeof(scrollText) - 1] = '\0';
    isScrolling    = true;
    scrollOffset   = 32;
    lastScrollStep = millis();
    drawText(scrollText, scrollOffset, currentColor);
}

// ===================== URL DECODE =====================
String urlDecode(const String& s) {
    String out;
    char hex[3] = {0};
    for (unsigned int i = 0; i < s.length(); i++) {
        if (s[i] == '%' && i + 2 < s.length()) {
            hex[0] = s[i + 1];
            hex[1] = s[i + 2];
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// ===================== APPLY STATE =====================
// Must be called from loop task only (FastLED not thread-safe)
void applyState(AlertState state) {
    blinkOn = false;
    switch (state) {
        case SAFE:
            setColor(0, 80, 0);         // Green solid
            showStatic("SAFE");
            break;

        case PRE_ALARM:
            setColor(150, 47, 0);       // Orange solid
            showStatic("UNSAFE");
            break;

        case ALARM:
            blinkOn = true;
            setColor(200, 0, 0);        // Red flicker (handled in loop)
            showStatic("ALERT");
            break;

        case UNSAFE:
            setColor(150, 47, 0);       // Orange solid
            showStatic("UNSAFE");
            break;

        case NO_API:
            setColor(0, 0, 80);         // Blue solid
            showScroll("NO API");
            break;

        case BAD_CITY:
            setColor(80, 0, 80);        // Magenta solid
            showScroll("BAD CITY");
            break;
    }
}

// ===================== ALERT TASK (core 0) =====================
void alertTask(void* param) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL));

        if (WiFi.status() != WL_CONNECTED) continue;

        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        if (!http.begin(client, ALERT_URL)) {
            apiFailCount++;
            continue;
        }

        http.addHeader("Referer",          "https://www.oref.org.il/");
        http.addHeader("X-Requested-With", "XMLHttpRequest");
        http.addHeader("Content-Type",     "application/json");
        http.setTimeout(8000);

        int code = http.GET();

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        AlertState cur = currentState;
        xSemaphoreGive(stateMutex);

        AlertState newState = cur;

        if (code == HTTP_CODE_OK) {
            apiFailCount = 0;   // reset failure counter on success

            String payload = http.getString();
            payload.trim();

            // Strip UTF-8 BOM
            if (payload.length() >= 3 &&
                (uint8_t)payload[0] == 0xEF &&
                (uint8_t)payload[1] == 0xBB &&
                (uint8_t)payload[2] == 0xBF) {
                payload = payload.substring(3);
            }

            if (payload.length() > 10) {
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, payload);

                if (!err) {
                    int cat = atoi(doc["cat"].as<const char*>() ?: "0");
                    String title = doc["title"].as<String>();
                    Serial.printf("[JSON] cat=%d title=%s\n", cat, title.c_str());

                    bool isEndOfEvent = title.indexOf("הסתיים") >= 0;
                    JsonArray data = doc["data"].as<JsonArray>();

                    bool cityFound = false;
                    for (JsonVariant v : data) {
                        String area = v.as<String>();
                        if (area.indexOf(cityName) >= 0 || cityName.indexOf(area) >= 0) {
                            cityFound = true;
                            break;
                        }
                    }

                    if (isEndOfEvent) {
                        if (cityFound) {
                            newState = SAFE;
                            Serial.println("[Alert] end-of-event -> SAFE");
                        }
                    } else {
                        if (cityFound) {
                            newState = (cat == CAT_PRE_ALARM) ? PRE_ALARM : ALARM;
                            lastAlertTime = millis();
                            Serial.printf("[Match] cat=%d -> %s\n", cat,
                                newState == PRE_ALARM ? "PRE_ALARM" : "ALARM");
                        } else if (cur == ALARM) {
                            newState = UNSAFE;
                            Serial.println("[Alert] ALARM ended -> UNSAFE");
                        }
                    }
                } else {
                    Serial.printf("[JSON] parse error: %s\n", err.c_str());
                }
            } else {
                if (cur == ALARM) {
                    newState = UNSAFE;
                    Serial.println("[HTTP] empty + was ALARM -> UNSAFE");
                }
            }
        } else {
            apiFailCount++;
            Serial.printf("[HTTP] error %d (fail count: %d)\n", code, (int)apiFailCount);
        }
        http.end();

        // Safety timeout
        if (cur != SAFE && millis() - lastAlertTime > SAFE_TIMEOUT_MS) {
            newState = SAFE;
            Serial.println("[Alert] safety timeout -> SAFE");
        }

        if (newState != cur) {
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            currentState = newState;
            xSemaphoreGive(stateMutex);
            Serial.printf("[Alert] State -> %s\n",
                newState == SAFE      ? "SAFE"      :
                newState == PRE_ALARM ? "PRE_ALARM" :
                newState == ALARM     ? "ALARM"     : "UNSAFE");
        }
    }
}

// ===================== SETUP =====================
void setup() {
    Serial.begin(115200);

    stateMutex = xSemaphoreCreateMutex();

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // --- FastLED init ---
    FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(30);
    setColor(0, 0, 80);
    showStatic("...");

    // --- Load saved city ---
    preferences.begin("redalert", true);
    String saved     = preferences.getString("city", "");
    bool forcePortal = preferences.getBool("portal", false);
    preferences.end();
    if (saved.length() > 0) cityName = saved;
    Serial.printf("[Setup] City: %s\n", cityName.c_str());

    // --- WiFiManager ---
    WiFiManager wm;
    wm.setTitle("Red Alert Config");

    WiFiManagerParameter cityParam("city", "City Name (Hebrew)", cityName.c_str(), 80);
    wm.addParameter(&cityParam);

    wm.setSaveParamsCallback([&]() {
        String val = urlDecode(String(cityParam.getValue()));
        val.trim();
        if (val.length() > 0) {
            cityName = val;
            preferences.begin("redalert", false);
            preferences.putString("city", cityName);
            preferences.end();
            Serial.printf("[Setup] City saved: %s\n", cityName.c_str());
        }
    });

    if (forcePortal) {
        preferences.begin("redalert", false);
        preferences.putBool("portal", false);
        preferences.end();
        Serial.println("[Setup] Portal requested by button");
    }

    setColor(0, 0, 80);
    showStatic(forcePortal ? "SETUP" : "WIFI");
    wm.setConfigPortalTimeout(180);

    bool connected = forcePortal
        ? wm.startConfigPortal("RedAlert-Setup")
        : wm.autoConnect("RedAlert-Setup");

    if (!connected) {
        Serial.println("[Setup] Portal timeout — restarting");
        ESP.restart();
    }

    Serial.printf("[Setup] WiFi OK. IP: %s\n", WiFi.localIP().toString().c_str());

    setColor(0, 80, 0);
    showStatic("OK");
    delay(1500);

    applyState(SAFE);

    // Start alert task on core 0
    xTaskCreatePinnedToCore(alertTask, "alertTask", 8192, nullptr, 1, nullptr, 0);
}

// ===================== LOOP (core 1) =====================
AlertState    lastDisplayState = SAFE;
unsigned long btnPressTime     = 0;
bool          btnWasPressed    = false;

void loop() {
    // --- Long press: trigger config portal ---
    bool btnPressed = (digitalRead(BUTTON_PIN) == LOW);
    if (btnPressed && !btnWasPressed) {
        btnPressTime  = millis();
        btnWasPressed = true;
    } else if (!btnPressed) {
        btnWasPressed = false;
    } else if (btnPressed && millis() - btnPressTime >= LONG_PRESS_MS) {
        Serial.println("[Button] Long press -> portal on next boot");
        preferences.begin("redalert", false);
        preferences.putBool("portal", true);
        preferences.end();
        ESP.restart();
    }

    // --- Compute display state ---
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    AlertState alertSt = currentState;
    xSemaphoreGive(stateMutex);

    AlertState displaySt;
    if (alertSt == SAFE) {
        if (apiFailCount >= API_FAIL_MAX) displaySt = NO_API;
        else if (!cityValid)             displaySt = BAD_CITY;
        else                             displaySt = SAFE;
    } else {
        displaySt = alertSt;   // alert states always take priority
    }

    // --- Apply when display changes ---
    if (displaySt != lastDisplayState) {
        lastDisplayState = displaySt;
        applyState(displaySt);
    }

    // --- Scroll animation ---
    if (isScrolling && millis() - lastScrollStep >= SCROLL_STEP_MS) {
        lastScrollStep = millis();
        int textW = strlen(scrollText) * 7;
        scrollOffset--;
        if (scrollOffset < -textW) scrollOffset = 32;
        drawText(scrollText, scrollOffset, currentColor);
    }

    // --- LED effects ---
    // Fast flicker: ALARM — redraw text in toggled color
    if (displaySt == ALARM && millis() - lastBlink >= 60) {
        lastBlink    = millis();
        blinkOn      = !blinkOn;
        currentColor = blinkOn ? CRGB(255, 0, 0) : CRGB::Black;
        drawText("ALERT", 1, currentColor);
    }
}
