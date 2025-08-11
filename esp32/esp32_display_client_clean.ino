/*
  ESP32 Minimal Display Client v0.2.0 - Clean WiFi Version
  
  This version clears WiFi settings on first boot, then operates normally.
  Use this if you're having WiFi connection issues due to old stored credentials.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>

// ---------- CONFIG ----------
const char* SERVER_BASE = "https://esp32-display-api.onrender.com";
const int POLL_FALLBACK_SEC = 10;
const int MAX_RETRY_ATTEMPTS = 3;
const int WIFI_TIMEOUT_SEC = 120;
const int HTTP_TIMEOUT_MS = 15000;
const bool VALIDATE_CERTIFICATES = false; // Set to true for production
const bool FORCE_WIFI_RESET = true; // Set to false after first successful setup

// Choose your display
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
  
  secureClient.setInsecure();
  
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
  secureClient.setInsecure();
  
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
      delay(2000 * attempt);
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
  
  if (!getUrl(url, resp)) {
    consecutiveErrors++;
    Serial.printf("Config GET failed (consecutive errors: %d)\n", consecutiveErrors);
    
    if (consecutiveErrors >= 5) {
      Serial.println("Too many errors, clearing token and re-pairing");
      deviceToken = "";
      preferences.remove("device_token");
      consecutiveErrors = 0;
      return 1;
    }
    
    String lines[3] = {"Network error", "Retrying...", "(" + String(consecutiveErrors) + " errors)"};
    screenLines(lines, 3);
    return min(POLL_FALLBACK_SEC * consecutiveErrors, 60);
  }
  
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, resp);
  
  if (err) {
    logError("JSON parse error: " + String(err.c_str()));
    String lines[2] = {"Bad JSON", "Retrying..."};
    screenLines(lines, 2);
    return POLL_FALLBACK_SEC;
  }
  
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
  return max(nextPoll, 1);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32 Display Client v0.2.0 (Clean WiFi) ===");
  
  preferences.begin("esp32-display", false);
  
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x12_tf);
  drawStatus("Booting...");
  delay(1000);
  
  deviceToken = preferences.getString("device_token", "");
  if (deviceToken.length() > 0) {
    Serial.println("Found saved device token: " + deviceToken.substring(0, 10) + "...");
  }
  
  drawStatus("WiFi setup");
  
  WiFiManager wm;
  
  // Force reset WiFi settings if requested
  if (FORCE_WIFI_RESET) {
    Serial.println("FORCE_WIFI_RESET=true - Clearing stored WiFi credentials");
    drawStatus("Clearing WiFi");
    wm.resetSettings();
    WiFi.disconnect(true, true);
    delay(2000);
  }
  
  wm.setTimeout(WIFI_TIMEOUT_SEC);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);
  
  String mac = WiFi.macAddress();
  String apName = "ESP32-Display-" + mac.substring(12);
  apName.replace(":", "");
  
  Serial.println("AP Name: " + apName);
  drawStatus("AP: " + apName);
  delay(2000);
  
  if (!wm.autoConnect(apName.c_str())) {
    logError("WiFi failed");
    drawStatus("WiFi failed");
    delay(3000);
    ESP.restart();
  }
  
  Serial.println("WiFi connected!");
  Serial.println("IP: " + WiFi.localIP().toString());
  
  drawStatus("WiFi connected");
  delay(1000);
  
  if (deviceToken.length() == 0) {
    if (!pairWithServer()) {
      delay(5000);
      ESP.restart();
    }
  } else {
    drawStatus("Testing token...");
    int result = pollAndRender();
    if (result == 1) {
      if (!pairWithServer()) {
        delay(5000);
        ESP.restart();
      }
    }
  }
  
  Serial.println("Setup complete!");
}

void loop() {
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
  
  for (int i = 0; i < waitSec * 10; i++) {
    delay(100);
  }
}