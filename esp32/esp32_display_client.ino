/*
  ESP32 Minimal Display Client v0.2.0
  - WiFiManager captive portal for WiFi setup
  - Pairs with server: POST /pair/start with hardware_uid (MAC)
  - Displays 6-digit code for user to claim on the web
  - Polls /device/config?device_token=... and renders lines on OLED
  - Improved error handling and recovery
  - Optional certificate validation for production

  Requires libraries:
    - WiFiManager by tzapu
    - U8g2 by olikraus (for SSD1306/SH1106)
    - ArduinoJSON
    - HTTPClient (built-in) and WiFiClientSecure
    - Preferences (built-in) for NVS storage
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ---------- CONFIG ----------
const char* SERVER_BASE = "https://esp32-display-api.onrender.com";
const int POLL_FALLBACK_SEC = 10;
const int MAX_RETRY_ATTEMPTS = 3;
const int WIFI_TIMEOUT_SEC = 120;
const int HTTP_TIMEOUT_MS = 15000;
const bool VALIDATE_CERTIFICATES = false; // Set to true for production with proper certs

// Choose your display (one of these)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// Global state
String deviceToken = "";
String pairCode = "";
String lastError = "";
int consecutiveErrors = 0;
Preferences preferences;

WiFiClientSecure secureClient;
HTTPClient http;

// Certificate for production (replace with your actual certificate)
// This is the ISRG Root X1 certificate used by Let's Encrypt
// For development, set VALIDATE_CERTIFICATES to false
const char* rootCACertificate = \
"-----BEGIN CERTIFICATE-----\n" \
"MIIFYDCCBEigAwIBAgIQQAF3ITfU6UK47naqPGQKtzANBgkqhkiG9w0BAQsFADA/\n" \
"MSQwIgYDVQQKExtEaWdpdGFsIFNpZ25hdHVyZSBUcnVzdCBDby4xFzAVBgNVBAMT\n" \
"DkRTVCBSb290IENBIFgzMB4XDTIxMDEyMDE5MTQwM1oXDTI0MDkzMDE4MTQwM1ow\n" \
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n" \
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwggIiMA0GCSqGSIb3DQEB\n" \
"AQUAA4ICDwAwggIKAoICAQCt6CRz9BQ385ueK1coHIe+3LffOJCMbjzmV6B493XC\n" \
"ov71am72AE8o295ohmxEk7axY/0UEmu/H9LqMZshftEzPLpI9d1537O4/xLxIZpL\n" \
"wYqGcWlKZmZsj348cL+tKSIG8+TA5oCu4kuPt5l+lAOf00eXfJlII1PoOK5PCm+D\n" \
"-----END CERTIFICATE-----\n";

void drawCentered(const String& line, int y) {
  int w = u8g2.getStrWidth(line.c_str());
  int x = (128 - w) / 2;
  if (x < 0) x = 0;
  u8g2.drawStr(x, y, line.c_str());
}

void drawStatus(const String& status) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  drawCentered(status, 32);
  if (!lastError.isEmpty()) {
    u8g2.setFont(u8g2_font_5x7_tf);
    drawCentered("Last error:", 45);
    drawCentered(lastError.substring(0, 20), 55);
  }
  u8g2.sendBuffer();
}

void logError(const String& error) {
  Serial.println("ERROR: " + error);
  lastError = error;
  consecutiveErrors++;
}

void screenPairCode(const String& code) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  drawCentered("Go to your site and enter", 12);
  drawCentered("this code:", 24);
  u8g2.setFont(u8g2_font_logisoso32_tf);
  drawCentered(code, 56);
  
  // Show WiFi status
  u8g2.setFont(u8g2_font_5x7_tf);
  String wifiInfo = "IP: " + WiFi.localIP().toString();
  drawCentered(wifiInfo.substring(0, 20), 8);
  
  u8g2.sendBuffer();
}

void screenLines(const String lines[], int count) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  int y = 14;
  for (int i = 0; i < count; i++) {
    drawCentered(lines[i], y);
    y += 14;
  }
  u8g2.sendBuffer();
}

bool postJson(const String& url, const String& payload, String& out) {
  Serial.println("POST " + url);
  
  if (VALIDATE_CERTIFICATES) {
    secureClient.setCACert(rootCACertificate);
  } else {
    secureClient.setInsecure();
  }
  
  if (!http.begin(secureClient, url)) {
    logError("HTTP begin failed");
    return false;
  }
  
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP32-Display/0.2.0");
  
  int code = http.POST(payload);
  Serial.printf("HTTP Response: %d\n", code);
  
  if (code > 0) {
    out = http.getString();
    Serial.println("Response: " + out.substring(0, 200));
  } else {
    logError("HTTP POST failed: " + String(code));
  }
  
  http.end();
  return code >= 200 && code < 300;
}

bool getUrl(const String& url, String& out) {
  if (VALIDATE_CERTIFICATES) {
    secureClient.setCACert(rootCACertificate);
  } else {
    secureClient.setInsecure();
  }
  
  if (!http.begin(secureClient, url)) {
    logError("HTTP begin failed");
    return false;
  }
  
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("User-Agent", "ESP32-Display/0.2.0");
  
  int code = http.GET();
  
  if (code > 0) {
    out = http.getString();
  } else {
    logError("HTTP GET failed: " + String(code));
  }
  
  http.end();
  return code >= 200 && code < 300;
}

bool pairWithServer() {
  Serial.println("Starting pairing process...");
  drawStatus("Pairing...");
  
  String mac = WiFi.macAddress();
  String url = String(SERVER_BASE) + "/pair/start";
  String payload;
  
  StaticJsonDocument<128> doc;
  doc["hardware_uid"] = "esp32:" + mac;
  serializeJson(doc, payload);
  
  String resp;
  for (int attempt = 1; attempt <= MAX_RETRY_ATTEMPTS; attempt++) {
    Serial.printf("Pairing attempt %d/%d\n", attempt, MAX_RETRY_ATTEMPTS);
    
    if (postJson(url, payload, resp)) {
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, resp);
      
      if (!err) {
        pairCode = String((const char*)doc["pair_code"]);
        deviceToken = String((const char*)doc["device_token"]);
        
        // Save device token to preferences
        preferences.putString("device_token", deviceToken);
        
        Serial.println("Pairing successful! Code: " + pairCode);
        screenPairCode(pairCode);
        consecutiveErrors = 0;
        return true;
      } else {
        logError("JSON parse error: " + String(err.c_str()));
      }
    }
    
    if (attempt < MAX_RETRY_ATTEMPTS) {
      delay(2000 * attempt); // Exponential backoff
    }
  }
  
  logError("Pairing failed after " + String(MAX_RETRY_ATTEMPTS) + " attempts");
  drawStatus("Pairing failed");
  return false;
}

int pollAndRender() {
  if (deviceToken.length() == 0) {
    Serial.println("No device token, need to pair");
    return POLL_FALLBACK_SEC;
  }
  
  String url = String(SERVER_BASE) + "/device/config?device_token=" + deviceToken;
  String resp;
  
  // Reset watchdog
  esp_task_wdt_reset();
  
  if (!getUrl(url, resp)) {
    consecutiveErrors++;
    Serial.printf("Config GET failed (consecutive errors: %d)\n", consecutiveErrors);
    
    // If too many consecutive errors, try to re-pair
    if (consecutiveErrors >= 5) {
      Serial.println("Too many errors, clearing token and re-pairing");
      deviceToken = "";
      preferences.remove("device_token");
      consecutiveErrors = 0;
      return 1; // Trigger immediate re-pair
    }
    
    String lines[3] = {"Network error", "Retrying...", "(" + String(consecutiveErrors) + " errors)"};
    screenLines(lines, 3);
    return min(POLL_FALLBACK_SEC * consecutiveErrors, 60); // Backoff
  }
  
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, resp);
  
  if (err) {
    logError("JSON parse error: " + String(err.c_str()));
    String lines[2] = {"Bad JSON", "Retrying..."};
    screenLines(lines, 2);
    return POLL_FALLBACK_SEC;
  }
  
  // Check for error responses
  if (doc.containsKey("detail")) {
    String error = doc["detail"].as<String>();
    if (error.indexOf("token") >= 0) {
      Serial.println("Token error, clearing and re-pairing");
      deviceToken = "";
      preferences.remove("device_token");
      return 1;
    }
    logError("Server error: " + error);
    String lines[2] = {"Server error", error.substring(0, 16)};
    screenLines(lines, 2);
    return POLL_FALLBACK_SEC;
  }
  
  // Success - reset error counter
  consecutiveErrors = 0;
  
  JsonArray arr = doc["render"]["lines"].as<JsonArray>();
  int i = 0;
  String lines[4];
  
  for (JsonVariant v : arr) {
    if (i >= 4) break;
    lines[i++] = String(v.as<const char*>());
  }
  
  if (i == 0) {
    lines[0] = "No data";
    i = 1;
  }
  
  screenLines(lines, i);
  int nextPoll = doc["next_poll_sec"] | POLL_FALLBACK_SEC;
  return max(nextPoll, 1); // Minimum 1 second
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32 Display Client v0.2.0 ===");
  
  // Initialize watchdog (compatible with both old and new framework versions)
  #ifdef ESP_IDF_VERSION_MAJOR
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
      // New API for ESP-IDF 5.x
      esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 30000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
      };
      esp_task_wdt_init(&wdt_config);
    #else
      // Old API for ESP-IDF 4.x
      esp_task_wdt_init(30, true);
    #endif
  #else
    // Fallback for very old versions
    esp_task_wdt_init(30, true);
  #endif
  esp_task_wdt_add(NULL);
  
  // Initialize preferences
  preferences.begin("esp32-display", false);
  
  // Display init
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x12_tf);
  drawStatus("Booting...");
  delay(1000);
  
  // Try to load saved device token
  deviceToken = preferences.getString("device_token", "");
  if (deviceToken.length() > 0) {
    Serial.println("Found saved device token: " + deviceToken.substring(0, 10) + "...");
  }
  
  drawStatus("WiFi setup");
  
  // WiFi config portal with improved settings
  WiFiManager wm;
  wm.setTimeout(WIFI_TIMEOUT_SEC);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180); // 3 minutes for config
  wm.setAPStaticIPConfig(IPAddress(192,168,1,1), IPAddress(192,168,1,1), IPAddress(255,255,255,0));
  
  String apName = "ESP32-Display-" + WiFi.macAddress().substring(12);
  apName.replace(":", "");
  
  if (!wm.autoConnect(apName.c_str())) {
    logError("WiFi failed");
    drawStatus("WiFi failed");
    delay(3000);
    ESP.restart();
  }
  
  Serial.println("WiFi connected!");
  Serial.println("IP: " + WiFi.localIP().toString());
  Serial.println("Signal strength: " + String(WiFi.RSSI()) + " dBm");
  
  drawStatus("WiFi connected");
  delay(1000);
  
  // Check if we need to pair
  if (deviceToken.length() == 0) {
    if (!pairWithServer()) {
      delay(5000);
      ESP.restart();
    }
  } else {
    // Test existing token
    drawStatus("Testing token...");
    int result = pollAndRender();
    if (result == 1) { // Token invalid, need to re-pair
      if (!pairWithServer()) {
        delay(5000);
        ESP.restart();
      }
    }
  }
  
  Serial.println("Setup complete!");
}

void loop() {
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    drawStatus("WiFi reconnecting");
    WiFi.reconnect();
    delay(5000);
    return;
  }
  
  int waitSec = pollAndRender();
  if (waitSec <= 0) waitSec = POLL_FALLBACK_SEC;
  
  Serial.printf("Next poll in %d seconds\n", waitSec);
  
  // Wait with periodic watchdog reset and status updates
  for (int i = 0; i < waitSec * 10; i++) {
    delay(100);
    
    // Reset watchdog every second
    if (i % 10 == 0) {
      esp_task_wdt_reset();
    }
    
    // Check for manual reset (if you add a button later)
    // Button logic could go here to clear token and re-pair
  }
}