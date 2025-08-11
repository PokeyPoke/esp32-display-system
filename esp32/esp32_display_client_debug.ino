/*
  ESP32 Display Client - Debug Version
  Simplified version for troubleshooting WiFi and connection issues
  
  Use this version first to ensure WiFi setup works before using the full version.
*/

#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <U8g2lib.h>
#include <Wire.h>

// ---------- CONFIG ----------
const char* SERVER_BASE = "https://your-render-url-here.onrender.com"; // Update this later
const int WIFI_TIMEOUT_SEC = 180; // 3 minutes

// Choose your display (one of these)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

void drawCentered(const String& line, int y) {
  int w = u8g2.getStrWidth(line.c_str());
  int x = (128 - w) / 2;
  if (x < 0) x = 0;
  u8g2.drawStr(x, y, line.c_str());
}

void showStatus(const String& status) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  drawCentered(status, 32);
  u8g2.sendBuffer();
  Serial.println("Status: " + status);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32 Display Debug v1.0 ===");
  
  // Display init
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x12_tf);
  showStatus("Booting...");
  delay(2000);
  
  // Show MAC address for debugging
  String mac = WiFi.macAddress();
  Serial.println("MAC Address: " + mac);
  showStatus("MAC: " + mac.substring(12));
  delay(2000);
  
  showStatus("WiFi Setup");
  Serial.println("Starting WiFi setup...");
  
  // WiFi config portal with extended timeout
  WiFiManager wm;
  
  // Debug settings
  wm.setDebugOutput(true);
  wm.setTimeout(WIFI_TIMEOUT_SEC);
  wm.setConnectTimeout(30); // 30 seconds to connect
  wm.setConfigPortalTimeout(300); // 5 minutes for config portal
  
  // Create unique AP name
  String apName = "ESP32-Debug-" + mac.substring(12);
  apName.replace(":", "");
  
  Serial.println("AP Name will be: " + apName);
  showStatus("AP: " + apName);
  delay(3000);
  
  // Try to connect
  if (!wm.autoConnect(apName.c_str())) {
    Serial.println("Failed to connect to WiFi");
    showStatus("WiFi Failed");
    delay(5000);
    ESP.restart();
  }
  
  // Connected successfully
  Serial.println("WiFi connected!");
  Serial.println("IP: " + WiFi.localIP().toString());
  Serial.println("Signal: " + String(WiFi.RSSI()) + " dBm");
  
  showStatus("Connected!");
  delay(2000);
  
  String ip = WiFi.localIP().toString();
  showStatus("IP: " + ip);
  
  Serial.println("Setup complete - entering main loop");
}

void loop() {
  // Check WiFi status
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected!");
    showStatus("WiFi Lost");
    delay(5000);
    ESP.restart();
    return;
  }
  
  // Show connection info
  showStatus("WiFi OK");
  delay(5000);
  
  String ip = WiFi.localIP().toString();
  showStatus("IP: " + ip);
  delay(5000);
  
  showStatus("Ready for config");
  delay(5000);
}