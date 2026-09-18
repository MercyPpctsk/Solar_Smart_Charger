// setup_web.h — Web UI & SoftAP configuration portal for Solar Smart Charger
// Styled after Slix Box v4 dark tech UI (Cyan #00d4ff on #1a1a2e)
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include "config.h"

static const uint32_t STA_RETRY_INTERVAL_MS = 30000UL;
static const uint32_t AP_STA_DEFER_MS       = 15000UL;
static uint32_t       _wifiNextRetryMs      = 0;

struct AppConfig {
  char dev_name[33]       = "";
  char wifi_ssid[33]      = "";
  char wifi_pass[65]      = "";
  char tb_host[65]        = "";
  uint16_t tb_port        = 1883;
  char tb_token[65]       = "";
  char tb_topic[65]       = "";
  uint32_t tb_interval    = 15;   // seconds
  char iqair_url[160]     = "";
  uint32_t iqair_interval = 30;   // minutes
};

static AppConfig   appConfig;
static WebServer   webServer(80);
static DNSServer   dnsServer;
static bool        g_wifiChanged   = false;
static bool        g_tbChanged     = false;
static bool        g_iqairChanged  = false;
static bool        g_rebootPending = false;
static uint32_t    g_rebootAtMs    = 0;

static const char* _apSsid() {
  static char ssid[33] = {0};
  if (appConfig.dev_name[0] != '\0') {
    snprintf(ssid, sizeof(ssid), "SOLAR-SETUP-%.19s", appConfig.dev_name);
  } else {
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    snprintf(ssid, sizeof(ssid), "SOLAR-SETUP-%02X%02X%02X", mac[3], mac[4], mac[5]);
  }
  return ssid;
}

static void loadAppConfig() {
  Preferences prefs;
  prefs.begin("app_cfg", false);

  String s_dev  = prefs.getString("dev_name", "");
  String s_ssid = prefs.getString("wifi_ssid", "");
  String s_pass = prefs.getString("wifi_pass", "");
  String s_host = prefs.getString("tb_host", "");
  uint16_t port = prefs.getUShort("tb_port", 0);
  String s_tok  = prefs.getString("tb_token", "");
  String s_top  = prefs.getString("tb_topic", "");
  uint32_t inv  = prefs.getUInt("tb_int", 0);
  String s_iq   = prefs.getString("iq_url", "");
  uint32_t iq_inv = prefs.getUInt("iq_int", 0);
  prefs.end();

  s_dev.trim();
  if (s_dev.length() > 0) {
    s_dev.toCharArray(appConfig.dev_name, sizeof(appConfig.dev_name));
  } else {
    appConfig.dev_name[0] = '\0';
  }

  // Fallbacks to config.h compiled defaults if NVS is empty (avoid phantom SSIDs)
  if (s_ssid.length() > 0 && s_ssid != "YOUR_WIFI_SSID") {
    s_ssid.toCharArray(appConfig.wifi_ssid, sizeof(appConfig.wifi_ssid));
  } else {
    appConfig.wifi_ssid[0] = '\0';
  }

  if (s_pass.length() > 0 && s_pass != "YOUR_WIFI_PASSWORD") {
    s_pass.toCharArray(appConfig.wifi_pass, sizeof(appConfig.wifi_pass));
  } else {
    appConfig.wifi_pass[0] = '\0';
  }

  if (s_host.length() > 0) s_host.toCharArray(appConfig.tb_host, sizeof(appConfig.tb_host));
  else strncpy(appConfig.tb_host, TB_MQTT_HOST, sizeof(appConfig.tb_host));

  appConfig.tb_port = (port > 0) ? port : TB_MQTT_PORT;

  if (s_tok.length() > 0) s_tok.toCharArray(appConfig.tb_token, sizeof(appConfig.tb_token));
  else strncpy(appConfig.tb_token, TB_MQTT_TOKEN, sizeof(appConfig.tb_token));

  if (s_top.length() > 0) s_top.toCharArray(appConfig.tb_topic, sizeof(appConfig.tb_topic));
  else strncpy(appConfig.tb_topic, TB_MQTT_TOPIC_TELEMETRY, sizeof(appConfig.tb_topic));

  appConfig.tb_interval = (inv > 0) ? inv : (TB_PUBLISH_INTERVAL_MS / 1000UL);

  if (s_iq.length() > 0) {
    s_iq.trim();
    while (s_iq.endsWith("%22") || s_iq.endsWith("\"") || s_iq.endsWith("\'")) {
      if (s_iq.endsWith("%22")) s_iq.remove(s_iq.length() - 3);
      else s_iq.remove(s_iq.length() - 1);
    }
    s_iq.trim();
    s_iq.toCharArray(appConfig.iqair_url, sizeof(appConfig.iqair_url));
  } else {
    strncpy(appConfig.iqair_url, IQAIR_STATION_URL, sizeof(appConfig.iqair_url));
  }

  appConfig.iqair_interval = (iq_inv > 0) ? iq_inv : 30;

  Serial.printf("[CFG ] Loaded: SSID='%s' TB='%s:%u' (int=%us) Token='%s' IQAir='%s' (int=%umin)\n",
                appConfig.wifi_ssid, appConfig.tb_host, appConfig.tb_port,
                appConfig.tb_interval, appConfig.tb_token, appConfig.iqair_url, appConfig.iqair_interval);
}

static void saveWifiConfig(const char* devName, const char* ssid, const char* pass, bool clearPass) {
  Preferences prefs;
  prefs.begin("app_cfg", false);

  if (devName) {
    String d = devName;
    d.trim();
    prefs.putString("dev_name", d);
    strncpy(appConfig.dev_name, d.c_str(), sizeof(appConfig.dev_name));
  }

  prefs.putString("wifi_ssid", ssid);
  strncpy(appConfig.wifi_ssid, ssid, sizeof(appConfig.wifi_ssid));

  if (clearPass) {
    prefs.putString("wifi_pass", "");
    appConfig.wifi_pass[0] = '\0';
  } else if (pass && strlen(pass) > 0) {
    prefs.putString("wifi_pass", pass);
    strncpy(appConfig.wifi_pass, pass, sizeof(appConfig.wifi_pass));
  }
  prefs.end();

  // Update SoftAP SSID broadcast if changed
  const char* newSsid = _apSsid();
  WiFi.softAP(newSsid, "solar1234", 1, 0, 4);

  g_wifiChanged = true;
  Serial.printf("[CFG ] WiFi/Dev config saved: Dev='%s' SSID='%s' AP='%s'\n",
                appConfig.dev_name, appConfig.wifi_ssid, newSsid);
}

static void saveCloudConfig(const char* host, uint16_t port, const char* token,
                             const char* topic, uint32_t intervalSec, const char* iqUrl, uint32_t iqIntervalMin) {
  Preferences prefs;
  prefs.begin("app_cfg", false);
  if (host && strlen(host) > 0) {
    prefs.putString("tb_host", host);
    strncpy(appConfig.tb_host, host, sizeof(appConfig.tb_host));
  }
  if (port > 0) {
    prefs.putUShort("tb_port", port);
    appConfig.tb_port = port;
  }
  if (token && strlen(token) > 0) {
    prefs.putString("tb_token", token);
    strncpy(appConfig.tb_token, token, sizeof(appConfig.tb_token));
  }
  if (topic && strlen(topic) > 0) {
    prefs.putString("tb_topic", topic);
    strncpy(appConfig.tb_topic, topic, sizeof(appConfig.tb_topic));
  }
  if (intervalSec > 0) {
    prefs.putUInt("tb_int", intervalSec);
    appConfig.tb_interval = intervalSec;
  }
  if (iqUrl && strlen(iqUrl) > 0) {
    String cleanUrl = iqUrl;
    cleanUrl.trim();
    while (cleanUrl.startsWith("\"") || cleanUrl.startsWith("\'")) cleanUrl.remove(0, 1);
    while (cleanUrl.endsWith("\"") || cleanUrl.endsWith("\'")) cleanUrl.remove(cleanUrl.length() - 1);
    cleanUrl.trim();
    prefs.putString("iq_url", cleanUrl);
    strncpy(appConfig.iqair_url, cleanUrl.c_str(), sizeof(appConfig.iqair_url));
  }
  if (iqIntervalMin > 0) {
    prefs.putUInt("iq_int", iqIntervalMin);
    appConfig.iqair_interval = iqIntervalMin;
  }
  prefs.end();
  g_tbChanged = true;
  g_iqairChanged = true;
  Serial.printf("[CFG ] Cloud config saved: TB='%s:%u' (int=%us) Token='%s' IQAir='%s' (int=%umin)\n",
                appConfig.tb_host, appConfig.tb_port, appConfig.tb_interval,
                appConfig.tb_token, appConfig.iqair_url, appConfig.iqair_interval);
}

// ---- HTML / Web UI rendering -----------------------------------------------
static String _buildWebPage(const String& savedMsg = "") {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>Solar Smart Charger &mdash; Setup Portal</title>"
                  "<style>"
                  "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif;"
                  "margin:0;padding:16px;background:#1a1a2e;color:#eee;}"
                  "h2{color:#00d4ff;margin:0 0 6px;font-size:22px;display:flex;align-items:center;gap:8px;}"
                  ".subtitle{color:#889;font-size:13px;margin-bottom:16px;}"
                  ".tabs{display:flex;gap:6px;margin-bottom:16px;border-bottom:1px solid #334;padding-bottom:2px;}"
                  ".tab{padding:10px 18px;background:#24243e;border:none;border-radius:8px 8px 0 0;"
                  "color:#aaa;cursor:pointer;font-size:14px;font-weight:600;transition:all 0.2s;}"
                  ".tab.active{background:#0a2a4a;color:#00d4ff;border-bottom:2px solid #00d4ff;}"
                  ".tab:hover{background:#162c4e;color:#fff;}"
                  ".panel{display:none;background:#0a2a4a;padding:20px;border-radius:0 10px 10px 10px;margin-bottom:16px;"
                  "box-shadow:0 4px 12px rgba(0,0,0,0.3);}"
                  ".panel.active{display:block;}"
                  "h3{color:#00d4ff;margin:0 0 14px;font-size:16px;border-bottom:1px solid #1a3a5a;padding-bottom:6px;}"
                  "label{display:block;margin:12px 0 4px;color:#8ab4f8;font-size:13px;font-weight:500;}"
                  "input[type=text],input[type=password],input[type=number]{width:100%;padding:10px 12px;"
                  "background:#16213e;color:#eee;border:1px solid #34495e;border-radius:6px;box-sizing:border-box;"
                  "font-size:14px;outline:none;transition:border-color 0.2s;}"
                  "input[type=text]:focus,input[type=password]:focus,input[type=number]:focus{border-color:#00d4ff;}"
                  ".btn{padding:10px 22px;background:#00d4ff;color:#000;border:none;border-radius:6px;font-size:14px;"
                  "font-weight:700;cursor:pointer;margin-top:16px;transition:background 0.2s;}"
                  ".btn:hover{background:#00b8d4;}"
                  ".btn.warn{background:#e74c3c;color:#fff;margin-top:10px;}"
                  ".btn.warn:hover{background:#c0392b;}"
                  ".alert{background:#164e32;border:1px solid #2ecc71;color:#a3e9a4;padding:10px 14px;"
                  "border-radius:6px;margin-bottom:16px;font-size:14px;}"
                  "table{border-collapse:collapse;width:100%;margin-top:14px;}"
                  "th,td{border:1px solid #233b58;padding:8px 12px;text-align:left;font-size:13px;}"
                  "th{background:#11253c;color:#8ab4f8;width:35%;}"
                  "td{background:#0a1e34;}"
                  ".badge{display:inline-block;padding:3px 8px;border-radius:12px;font-size:11px;font-weight:600;}"
                  ".badge.pass{background:#1b4d2e;color:#4ef072;}"
                  ".badge.fail{background:#4d1b1b;color:#f04e4e;}"
                  ".badge.info{background:#1b2e4d;color:#4ea8f0;}"
                  ".hint{color:#889;font-size:12px;margin-top:4px;}"
                  "</style></head><body>");

  html += F("<h2>&#9881; Solar Smart Charger &mdash; Setup</h2>");
  html += "<div class='subtitle'>Device: <strong>" + String(_apSsid()) + "</strong> | SoftAP IP: <strong>"
          + WiFi.softAPIP().toString() + "</strong></div>";

  if (savedMsg.length() > 0) {
    html += "<div class='alert'>&#10004; " + savedMsg + "</div>";
  }

  // Tabs Header
  html += F("<div class='tabs'>"
            "<button id='tab_wifi' class='tab active' onclick=\"openTab('wifi')\">&#128246; WiFi Settings</button>"
            "<button id='tab_cloud' class='tab' onclick=\"openTab('cloud')\">&#9729; ThingsBoard &amp; IQAir</button>"
            "</div>");

  // Tab 1: WiFi
  bool staConnected = (WiFi.status() == WL_CONNECTED);
  html += F("<div id='panel_wifi' class='panel active'>");
  html += F("<h3>Device &amp; WiFi Configuration</h3>");
  html += F("<form action='/save_wifi' method='post'>");
  html += F("<label>Device Name (ESP32 / SoftAP Suffix)</label>");
  html += "<input type='text' name='dev_name' value='" + String(appConfig.dev_name) + "' placeholder='e.g. Solar-01 (SoftAP: SOLAR-SETUP-Solar-01)' maxlength='19'>";
  html += F("<div class='hint'>Custom device name. If set, SoftAP SSID becomes <strong>SOLAR-SETUP-&lt;name&gt;</strong> instead of MAC address (leave empty to use default MAC).</div>");
  html += F("<label style='margin-top:14px;'>WiFi Network Name (SSID)</label>");
  html += "<input type='text' name='ssid' value='" + String(appConfig.wifi_ssid) + "' placeholder='Enter WiFi SSID' required>";
  html += F("<label>WiFi Password</label>");
  html += F("<input type='password' name='pass' placeholder='Leave empty to keep saved password'>");
  html += F("<div class='hint'>Saved password is protected. Enter a new password only if you wish to change it.</div>");
  html += F("<label style='margin-top:8px;'><input type='checkbox' name='clear_pass' value='1'> Clear password (Open Network)</label>");
  html += F("<button type='submit' class='btn'>Save WiFi &amp; Connect</button>");
  html += F("</form>");

  html += F("<h3 style='margin-top:24px;'>Network Status</h3>");
  html += F("<table>");
  html += "<tr><th>Device Name</th><td><strong>" + (appConfig.dev_name[0] != '\0' ? String(appConfig.dev_name) : String("<em>(Default: MAC)</em>")) + "</strong></td></tr>";
  html += "<tr><th>WiFi Status</th><td>" + String(staConnected ? "<span class='badge pass'>CONNECTED</span>" : "<span class='badge fail'>DISCONNECTED</span>") + "</td></tr>";
  html += "<tr><th>Station IP</th><td>" + (staConnected ? WiFi.localIP().toString() : String("-")) + "</td></tr>";
  html += "<tr><th>SoftAP SSID</th><td><strong>" + String(_apSsid()) + "</strong></td></tr>";
  html += "<tr><th>SoftAP IP</th><td>" + WiFi.softAPIP().toString() + "</td></tr>";
  html += "<tr><th>Signal Strength</th><td>" + (staConnected ? (String(WiFi.RSSI()) + " dBm") : String("-")) + "</td></tr>";
  html += "<tr><th>MAC Address</th><td>" + WiFi.macAddress() + "</td></tr>";
  html += F("</table></div>");

  // Tab 2: ThingsBoard & IQAir
  html += F("<div id='panel_cloud' class='panel'>");
  html += F("<form action='/save_cloud' method='post'>");
  html += F("<h3>ThingsBoard MQTT Configuration</h3>");
  html += F("<label>Server Host / IP</label>");
  html += "<input type='text' name='tb_host' value='" + String(appConfig.tb_host) + "' placeholder='thingsboard.weaverbase.com' required>";
  html += F("<label>Server Port (MQTT)</label>");
  html += "<input type='number' name='tb_port' value='" + String(appConfig.tb_port) + "' min='1' max='65535' required>";
  html += F("<label>Device Access Token</label>");
  html += "<input type='text' name='tb_token' value='" + String(appConfig.tb_token) + "' placeholder='e.g. 9YLXFadM8n7vfP2n8rWv' required>";
  html += F("<label>Telemetry Topic</label>");
  html += "<input type='text' name='tb_topic' value='" + String(appConfig.tb_topic) + "' placeholder='v1/devices/me/telemetry' required>";
  html += F("<label>Telemetry Interval (Seconds)</label>");
  html += "<input type='number' name='tb_int' value='" + String(appConfig.tb_interval) + "' min='5' max='3600' required>";

  html += F("<h3 style='margin-top:24px;'>IQAir Station Configuration</h3>");
  html += F("<label>IQAir Public Station URL</label>");
  html += "<input type='text' name='iq_url' value='" + String(appConfig.iqair_url) + "' placeholder='https://device.iqair.com/v2/...'>";
  html += F("<div class='hint'>Base public station endpoint (e.g. https://device.iqair.com/v2/&lt;station-id&gt;)</div>");
  html += F("<label style='margin-top:10px;'>IQAir Fetch Interval (Minutes)</label>");
  html += "<input type='number' name='iq_int' value='" + String(appConfig.iqair_interval) + "' min='1' max='1440' required>";
  html += F("<div class='hint'>Polling frequency from IQAir public endpoint (default: 30 minutes)</div>");

  html += F("<button type='submit' class='btn'>Save Cloud Settings</button>");
  html += F("</form>");

  html += F("<h3 style='margin-top:28px;'>System Maintenance</h3>");
  html += F("<form action='/reboot' method='post' onsubmit=\"return confirm('Restart ESP32-S3 now?');\">");
  html += F("<button type='submit' class='btn warn'>Restart Device</button>");
  html += F("</form></div>");

  // Tab JS
  html += F("<script>"
            "function openTab(name){"
            "document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));"
            "document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));"
            "document.getElementById('tab_'+name).classList.add('active');"
            "document.getElementById('panel_'+name).classList.add('active');"
            "window.location.hash=name;"
            "}"
            "if(window.location.hash){"
            "var h=window.location.hash.substring(1);"
            "if(document.getElementById('tab_'+h)) openTab(h);"
            "}"
            "</script></body></html>");
  return html;
}

static void _handleRoot() {
  String saved = "";
  if (webServer.hasArg("saved")) {
    String s = webServer.arg("saved");
    if (s == "wifi") saved = "WiFi settings saved! Attempting connection...";
    else if (s == "cloud") saved = "Cloud settings saved successfully!";
  }
  webServer.send(200, "text/html", _buildWebPage(saved));
}

static void _handleSaveWiFi() {
  String devName = webServer.arg("dev_name");
  String ssid    = webServer.arg("ssid");
  String pass    = webServer.arg("pass");
  bool clearPass = webServer.hasArg("clear_pass");
  devName.trim();
  ssid.trim();
  pass.trim();

  saveWifiConfig(devName.c_str(), ssid.c_str(), pass.c_str(), clearPass);

  webServer.sendHeader("Location", "/?saved=wifi#wifi");
  webServer.send(303);
}

static void _handleSaveCloud() {
  String host = webServer.arg("tb_host");
  uint16_t port = (uint16_t)webServer.arg("tb_port").toInt();
  String token = webServer.arg("tb_token");
  String topic = webServer.arg("tb_topic");
  uint32_t intervalSec = (uint32_t)webServer.arg("tb_int").toInt();
  String iqUrl = webServer.arg("iq_url");
  uint32_t iqIntervalMin = (uint32_t)webServer.arg("iq_int").toInt();

  host.trim();
  token.trim();
  topic.trim();
  iqUrl.trim();

  saveCloudConfig(host.c_str(), port, token.c_str(), topic.c_str(), intervalSec, iqUrl.c_str(), iqIntervalMin);

  webServer.sendHeader("Location", "/?saved=cloud#cloud");
  webServer.send(303);
}

static void _handleReboot() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<meta http-equiv='refresh' content='8;url=/'>"
                  "<title>Restarting...</title>"
                  "<style>body{background:#1a1a2e;color:#eee;font-family:sans-serif;text-align:center;padding-top:60px;}"
                  "h2{color:#00d4ff;}</style></head><body>"
                  "<h2>&#8635; Restarting ESP32-S3...</h2>"
                  "<p>Please wait 8 seconds, the page will refresh automatically.</p>"
                  "</body></html>");
  webServer.send(200, "text/html", html);
  g_rebootPending = true;
  g_rebootAtMs = millis() + 1000;
}

static void _handleCaptivePortal() {
  webServer.sendHeader("Location", "http://192.168.4.1/", true);
  webServer.send(302, "text/plain", "");
}

static void setupWebBegin() {
  loadAppConfig();

  // Mode AP+STA: AP is ALWAYS active for configuration
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  const char* apSsid = _apSsid();
  bool apOk = WiFi.softAP(apSsid, "solar1234", 1, 0, 4);
  Serial.printf("[SETUP] SoftAP '%s' -> %s, IP: %s (Password: solar1234)\n",
                apSsid, apOk ? "OK" : "FAILED", WiFi.softAPIP().toString().c_str());

  // Captive Portal DNS on port 53: redirect all hostnames to AP IP
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Web routes
  webServer.on("/", HTTP_GET, _handleRoot);
  webServer.on("/save_wifi", HTTP_POST, _handleSaveWiFi);
  webServer.on("/save_cloud", HTTP_POST, _handleSaveCloud);
  webServer.on("/reboot", HTTP_POST, _handleReboot);

  // Captive portal probes from iOS, Android, Windows
  webServer.on("/hotspot-detect.html", HTTP_GET, _handleRoot);
  webServer.on("/generate_204", HTTP_GET, _handleRoot);
  webServer.on("/canonical.html", HTTP_GET, _handleRoot);
  webServer.on("/connecttest.txt", HTTP_GET, _handleRoot);
  webServer.onNotFound(_handleCaptivePortal);

  webServer.begin();
  Serial.println("[SETUP] Web server listening on port 80");
}

static void setupWebTick() {
  dnsServer.processNextRequest();
  webServer.handleClient();

  if (g_rebootPending && millis() >= g_rebootAtMs) {
    Serial.println("[SYS ] Restarting now...");
    delay(100);
    ESP.restart();
  }

  // #AP-STABILITY (from Slix v4):
  // When a client is connected to SoftAP, DO NOT disrupt AP radio with channel scans!
  uint32_t now = millis();
  if (appConfig.wifi_ssid[0] != '\0' && WiFi.status() != WL_CONNECTED) {
    if (_wifiNextRetryMs == 0) {
      _wifiNextRetryMs = now + 10000UL;
    } else if ((int32_t)(now - _wifiNextRetryMs) >= 0) {
      uint8_t apClients = WiFi.softAPgetStationNum();
      if (apClients > 0) {
        // Operator is connected to setup AP — defer STA retry to keep AP rock solid
        _wifiNextRetryMs = now + AP_STA_DEFER_MS;
      } else {
        Serial.printf("[NET ] STA retry '%s' (throttled %lus)...\n",
                      appConfig.wifi_ssid, (unsigned long)(STA_RETRY_INTERVAL_MS / 1000UL));
        WiFi.setAutoReconnect(false);
        WiFi.begin(appConfig.wifi_ssid, appConfig.wifi_pass);
        _wifiNextRetryMs = now + STA_RETRY_INTERVAL_MS;
      }
    }
  }
}
