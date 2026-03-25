#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ─── Display ──────────────────────────────────────────────────────────────────
#define DATA_PIN    3
#define NUM_LEDS    256

uint8_t matrixBrightness = 20;  // 1–100, loaded from Preferences

// ─── Button ───────────────────────────────────────────────────────────────────
#define BUTTON_PIN    10     // GPIO10 → connect to GND, internal pull-up
#define LONG_PRESS_MS 3000

CRGB leds[NUM_LEDS];

void setPixel(int x, int y, CRGB color) {
    if (x < 0 || x >= 32 || y < 0 || y >= 8) return;
    x = 31 - x;  // 180° rotation
    y = 7  - y;  // 180° rotation
    int panel = x / 8;
    int vcol  = x % 8;
    int vrow  = 7 - y;
    int pcol  = 7 - vrow;
    int prow  = vcol;
    leds[panel * 64 + prow * 8 + pcol] = color;
}

// ─── Font: 5 cols, bit0=bottom row, bit6=top row ──────────────────────────────
const char    FONT_KEYS[]    = "ABCDEFIKLMNOPRSTUYW. ";
const uint8_t FONT_DATA[][5] = {
    {0x3F,0x48,0x48,0x48,0x3F}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x41,0x3E}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x48,0x48,0x48,0x40}, // F
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
    {0x00,0x00,0x03,0x00,0x00}, // . (dot — 2 bottom pixels)
    {0x00,0x00,0x00,0x00,0x00}, // (space)
};

const uint8_t* getGlyph(char c) {
    const char* p = strchr(FONT_KEYS, c);
    if (!p) p = FONT_KEYS + sizeof(FONT_KEYS) - 2;
    return FONT_DATA[p - FONT_KEYS];
}

// bold=true: each lit pixel is doubled one step to the right, spacing 7px
int textWidth(const char* text, bool bold = true) {
    int len = strlen(text);
    return len > 0 ? len * (bold ? 7 : 6) - 1 : 0;
}

void drawText(const char* text, int xOffset, CRGB color, bool bold = true) {
    int step = bold ? 7 : 6;
    FastLED.clear();
    for (int i = 0; text[i]; i++) {
        int cx = xOffset + i * step;
        const uint8_t* g = getGlyph(text[i]);
        for (int col = 0; col < 5; col++) {
            uint8_t data = g[col];
            for (int row = 0; row < 7; row++) {
                if (data & (1 << row)) {
                    setPixel(cx + col,     row, color);
                    if (bold) setPixel(cx + col + 1, row, color);
                }
            }
        }
    }
    FastLED.show();
}

void drawCentered(const char* text, CRGB color, bool bold = true) {
    drawText(text, (32 - textWidth(text, bold)) / 2, color, bold);
}

// ─── Preferences (replaces EEPROM) ────────────────────────────────────────────
char cityName[65] = "";

void saveCityName(const char* name) {
    Preferences prefs;
    prefs.begin("alert", false);
    prefs.putString("city", name);
    prefs.end();
}

void loadCityName() {
    Preferences prefs;
    prefs.begin("alert", false);  // false = create namespace if not exists
    String s = prefs.getString("city", "");
    prefs.end();
    s.toCharArray(cityName, 65);
}

void saveForcePortal(bool val) {
    Preferences prefs;
    prefs.begin("alert", false);
    prefs.putBool("portal", val);
    prefs.end();
}

bool loadForcePortal() {
    Preferences prefs;
    prefs.begin("alert", false);
    bool val = prefs.getBool("portal", false);
    prefs.end();
    return val;
}

void saveBrightness(uint8_t val) {
    Preferences prefs;
    prefs.begin("alert", false);
    prefs.putUChar("bright", val);
    prefs.end();
}

void loadBrightness() {
    Preferences prefs;
    prefs.begin("alert", false);
    uint8_t val = prefs.getUChar("bright", 20);
    prefs.end();
    if (val >= 1 && val <= 100) matrixBrightness = val;
}

// ─── URL decode ───────────────────────────────────────────────────────────────
String urlDecode(const String& s) {
    String out;
    for (unsigned int i = 0; i < s.length(); i++) {
        if (s[i] == '%' && i + 2 < s.length()) {
            char hex[3] = {s[i+1], s[i+2], '\0'};
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

// ─── Alert state ──────────────────────────────────────────────────────────────
enum AlertState { SAFE, PRE_ALARM, ALARM, UNSAFE, NO_API, BAD_CITY };

// Shared between Core 0 (network) and Core 1 (display).
// All are 32-bit aligned — reads/writes are atomic on ESP32-C3 RISC-V.
volatile AlertState alertState    = SAFE;
volatile int        apiFailCount  = 0;
volatile uint32_t   lastAlertMs   = 0;
volatile uint32_t   unsafeStartMs = 0;

#define CHECK_INTERVAL   3000UL
#define SAFE_TIMEOUT_MS  (20UL * 60 * 1000)
#define API_FAIL_MAX     5

// ─── Network task (Core 0) ────────────────────────────────────────────────────
void checkAlerts() {
    if (WiFi.status() != WL_CONNECTED || strlen(cityName) == 0) return;

    // Safety timeouts — checked every poll cycle
    uint32_t now = millis();
    if (alertState != SAFE && now - lastAlertMs > SAFE_TIMEOUT_MS) {
        alertState = SAFE;
        Serial.println("[State] safety timeout → SAFE");
        return;
    }
    if (alertState == UNSAFE && now - unsafeStartMs >= SAFE_TIMEOUT_MS) {
        alertState = SAFE;
        Serial.println("[State] unsafe timeout → SAFE");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, "https://www.oref.org.il/warningMessages/alert/Alerts.json")) {
        apiFailCount++;
        Serial.println("[HTTP] begin failed");
        return;
    }
    http.addHeader("Referer",          "https://www.oref.org.il/");
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("Content-Type",     "application/json");
    http.setTimeout(5000);

    int code = http.GET();
    Serial.printf("[HTTP] code=%d  heap=%d\n", code, ESP.getFreeHeap());

    if (code != HTTP_CODE_OK) {
        apiFailCount++;
        http.end();
        return;
    }

    apiFailCount = 0;
    String payload = http.getString();
    http.end();

    payload.trim();
    if (payload.length() >= 3 &&
        (uint8_t)payload[0] == 0xEF &&
        (uint8_t)payload[1] == 0xBB &&
        (uint8_t)payload[2] == 0xBF) {
        payload = payload.substring(3);
    }

    AlertState cur  = alertState;
    AlertState next = cur;

    if (payload.length() <= 10) {
        // API: no active alerts
        if (cur == ALARM || cur == PRE_ALARM) {
            next = UNSAFE;                // was alarming → enter safety window
            unsafeStartMs = millis();
        }
        // UNSAFE stays UNSAFE — waits for explicit "הסתיים" from Oref or safety timeout
    } else {
        JsonDocument doc;
        if (deserializeJson(doc, payload) != DeserializationError::Ok) return;

        int    cat   = doc["cat"].as<int>();  // as<int>() handles both string and number from Oref
        String title = doc["title"] | "";
        bool   ended = title.indexOf("הסתיים") >= 0;

        String city = String(cityName);
        bool cityFound = false;
        for (JsonVariant v : doc["data"].as<JsonArray>()) {
            String area = v.as<String>();
            if (area.indexOf(city) >= 0 || city.indexOf(area) >= 0) {
                cityFound = true;
                break;
            }
        }

        Serial.printf("[JSON] cat=%d ended=%d cityFound=%d title=%s\n",
                      cat, ended, cityFound, title.c_str());

        if (ended) {
            if (cityFound) next = SAFE;   // Oref confirmed all-clear for our city
        } else if (cityFound) {
            next = (cat == 10) ? PRE_ALARM : ALARM;
            lastAlertMs = millis();
        } else {
            // City not in any active alert
            if (cur == ALARM || cur == PRE_ALARM) {
                next = UNSAFE;            // alert ended for our city → safety window
                unsafeStartMs = millis();
            }
            // UNSAFE stays UNSAFE — waits for explicit "הסתיים" or safety timeout
        }
    }

    if (next != cur) {
        alertState = next;
        Serial.printf("[State] -> %d\n", (int)next);
    }
}

void networkTask(void* pvParameters) {
    for (;;) {
        checkAlerts();
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL));
    }
}

// ─── Display update (Core 1 — loop) ───────────────────────────────────────────
AlertState    lastDisplayed = (AlertState)99;
int           scrollX       = 32;
bool          blinkOn       = false;
unsigned long lastScrollMs  = 0;
unsigned long lastBlinkMs   = 0;

#define SCROLL_MS   40
#define ALARM_BLINK 60
#define NOAPI_BLINK 600

AlertState computeDisplay() {
    AlertState as = alertState;
    if (as == SAFE) {
        if (apiFailCount >= API_FAIL_MAX) return NO_API;
        if (strlen(cityName) == 0)        return BAD_CITY;
    }
    return as;
}

void updateDisplay() {
    unsigned long now = millis();
    AlertState ds = computeDisplay();

    if (ds != lastDisplayed) {
        lastDisplayed = ds;
        scrollX  = 32;
        blinkOn  = false;
        lastScrollMs = 0;  // force immediate first draw
        switch (ds) {
            case SAFE:      drawCentered("SAFE",      CRGB(0, 80, 0));        break;
            case ALARM:     drawCentered("ALARM",     CRGB(80, 0, 0));        break;
            case PRE_ALARM: drawText("PRE ALARM", scrollX, CRGB(255, 80, 0)); break;
            case UNSAFE:    drawText("UNSAFE",    scrollX, CRGB(80, 0, 0));   break;
            case NO_API:    drawText("NO API",    scrollX, CRGB(0, 0, 80));   break;
            case BAD_CITY:  drawText("BAD CITY",  scrollX, CRGB(80, 0, 80));  break;
            default: break;
        }
        return;
    }

    switch (ds) {
        case ALARM:
            if (now - lastBlinkMs >= ALARM_BLINK) {
                lastBlinkMs = now;
                blinkOn = !blinkOn;
                if (blinkOn) drawCentered("ALARM", CRGB(80, 0, 0));
                else        { FastLED.clear(); FastLED.show(); }
            }
            break;
        case PRE_ALARM:
            if (now - lastScrollMs >= SCROLL_MS) {
                lastScrollMs = now;
                drawText("PRE ALARM", scrollX, CRGB(255, 80, 0));
                if (--scrollX < -textWidth("PRE ALARM")) scrollX = 32;
            }
            break;
        case UNSAFE:
            if (now - lastScrollMs >= SCROLL_MS) {
                lastScrollMs = now;
                drawText("UNSAFE", scrollX, CRGB(80, 0, 0));
                if (--scrollX < -textWidth("UNSAFE")) scrollX = 32;
            }
            break;
        case NO_API:
            if (now - lastBlinkMs >= NOAPI_BLINK) {
                lastBlinkMs = now;
                blinkOn = !blinkOn;
            }
            if (now - lastScrollMs >= SCROLL_MS) {
                lastScrollMs = now;
                if (blinkOn) drawText("NO API", scrollX, CRGB(0, 0, 80));
                else        { FastLED.clear(); FastLED.show(); }
                if (--scrollX < -textWidth("NO API")) scrollX = 32;
            }
            break;
        case BAD_CITY:
            if (now - lastScrollMs >= SCROLL_MS) {
                lastScrollMs = now;
                drawText("BAD CITY", scrollX, CRGB(80, 0, 80));
                if (--scrollX < -textWidth("BAD CITY")) scrollX = 32;
            }
            break;
        default: break;
    }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);  // USB-CDC needs time to enumerate on cold boot

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(matrixBrightness);

    // ── Boot sequence ──────────────────────────────────────────────────────────
    fill_solid(leds, NUM_LEDS, CRGB(0, 0, 60));  // Blue = booting
    FastLED.show();
    drawCentered("...", CRGB(0, 0, 60));

    loadCityName();
    loadBrightness();
    bool forcePortal = loadForcePortal();
    if (forcePortal) {
        saveForcePortal(false);
        Serial.println("[Setup] Portal requested by button");
    }
    Serial.printf("[Setup] City: '%s'\n", cityName);

    // ── WiFiManager in its own scope so memory is freed when done ──────────────
    {
        WiFiManager wm;
        wm.setConfigPortalTimeout(180);

        WiFiManagerParameter cityParam("city", "City Name (Hebrew)", cityName, 64);
        wm.addParameter(&cityParam);

        char brightnessStr[4];
        snprintf(brightnessStr, sizeof(brightnessStr), "%d", matrixBrightness);
        WiFiManagerParameter brightnessParam("brightness", "Brightness (1-100)", brightnessStr, 4,
                                             "type=\"number\" min=\"1\" max=\"100\"");
        wm.addParameter(&brightnessParam);

        wm.setSaveParamsCallback([&cityParam, &brightnessParam]() {
            String val = urlDecode(String(cityParam.getValue()));
            val.trim();
            if (val.length() > 0) {
                val.toCharArray(cityName, 65);
                saveCityName(cityName);
                Serial.printf("[Setup] City saved: %s\n", cityName);
            }
            int bval = String(brightnessParam.getValue()).toInt();
            if (bval >= 1 && bval <= 100) {
                matrixBrightness = (uint8_t)bval;
                saveBrightness(matrixBrightness);
                FastLED.setBrightness(matrixBrightness);
                Serial.printf("[Setup] Brightness saved: %d\n", matrixBrightness);
            }
        });

        // No setAPCallback — calling FastLED.show() from inside a WiFiManager
        // callback crashes the ESP32-C3 WiFi driver (RMT interrupt conflict).
        // Display is set to "CONF" before autoConnect so the user knows to configure.
        drawCentered("CONF", CRGB(0, 0, 60));

        bool connected = forcePortal
            ? wm.startConfigPortal("RedAlert-Setup")
            : wm.autoConnect("RedAlert-Setup");
        if (!connected) {
            Serial.println("[Setup] timeout — restarting");
            ESP.restart();
        }
    }
    // WiFiManager freed here
    FastLED.setBrightness(matrixBrightness);

    Serial.printf("[Setup] WiFi OK. IP: %s  Heap: %d\n",
                  WiFi.localIP().toString().c_str(), ESP.getFreeHeap());

    fill_solid(leds, NUM_LEDS, CRGB(0, 60, 0));  // Green flash = connected
    FastLED.show();
    drawCentered("OK", CRGB(0, 80, 0));
    delay(1500);

    alertState  = SAFE;
    lastAlertMs = millis();
    drawCentered("SAFE", CRGB(0, 80, 0));

    // ── Start network task on Core 0 ───────────────────────────────────────────
    xTaskCreatePinnedToCore(networkTask, "netTask", 8192, nullptr, 1, nullptr, 0);
}

// ─── Button state ─────────────────────────────────────────────────────────────
bool          btnWasPressed = false;
unsigned long btnPressTime  = 0;

// ─── Loop (Core 1) ────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── Long-press button → force config portal on next boot ──────────────────
    bool btnPressed = (digitalRead(BUTTON_PIN) == LOW);
    if (btnPressed && !btnWasPressed) {
        btnPressTime  = now;
        btnWasPressed = true;
    } else if (!btnPressed) {
        btnWasPressed = false;
    } else if (btnPressed && now - btnPressTime >= LONG_PRESS_MS) {
        Serial.println("[Button] Long press → portal on next boot");
        saveForcePortal(true);
        ESP.restart();
    }

    // ── No WiFi ───────────────────────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        if (now - lastScrollMs >= SCROLL_MS) {
            lastScrollMs = now;
            drawText("NO WIFI", scrollX, CRGB(0, 0, 80));
            if (--scrollX < -textWidth("NO WIFI")) scrollX = 32;
        }
        delay(10);
        return;
    }

    updateDisplay();
}
