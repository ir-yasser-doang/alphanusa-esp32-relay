#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Update.h>
#include <SuperDMZ.h>

// ===== Konfigurasi =====
#define RELAY_PIN 47
const char* AP_SSID_DEFAULT     = "Alphanusa IoT Team";
const char* AP_PASSWORD_DEFAULT = "alphanusa098765";
const char* MQTT_BROKER_DEFAULT = "202.10.37.231";
const int   MQTT_PORT_DEFAULT   = 1883;
const char* MQTT_DEVICE_ID_DEFAULT = "DEV-001";
const char* SUPERDMZ_TOKEN = "";
#define MAX_HISTORY 3
#define MAX_EVENT_LOG 5

// ===== Objek =====
WebServer server(80);
Preferences preferences;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600);
SuperDMZ tunnel;

// ===== State =====
bool relayOn = false;
String currentAP_SSID = AP_SSID_DEFAULT;
String currentAP_PASS = AP_PASSWORD_DEFAULT;
bool staConnected = false;
String staSSID = "";
String staIP = "";
int staRSSI = 0;
bool internetAvailable = false;
unsigned long lastInternetCheck = 0;
const unsigned long INTERNET_CHECK_INTERVAL = 30000;
unsigned long lastWiFiReconnectAttempt = 0;
String mqttBroker = MQTT_BROKER_DEFAULT;
int    mqttPort   = MQTT_PORT_DEFAULT;
String mqttDeviceId = MQTT_DEVICE_ID_DEFAULT;
String mqttClientId = "";
bool   mqttConnected = false;
unsigned long lastMqttReconnect = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
unsigned long lastActiveUntil = 0;
unsigned long graceSeconds = 0;
String overrideMode = "AUTO";
int lastRev = 0;
unsigned long lastMqttConnectedTime = 0;
const unsigned long FAILSAFE_TIMEOUT = 24 * 3600 * 1000;
bool sudahSinkronWaktu = false;

// ===== History =====
struct WifiHistory { String ssid; String pass; String status; unsigned long timestamp; };
WifiHistory histories[MAX_HISTORY];
int historyCount = 0;
struct EventLog { String event; unsigned long timestamp; };
EventLog eventLogs[MAX_EVENT_LOG];
int eventLogCount = 0;

// ===== Deklarasi =====
void handleRoot(); void handleScan(); void handleSaveAP(); void handleConnectSTA(); void handleDisconnectSTA();
void handleStatus(); void handleHistory(); void handleDeleteHistory(); void handleClearHistory();
void handleSaveMQTT(); void handleForceOn(); void handleForceOff(); void handleEventLog();
void checkInternet(); bool tryConnectSTA(String ssid, String pass, String &errorMsg);
void loadHistory(); void saveHistory(); void addToHistory(String ssid, String pass, String status);
void deleteHistoryByIndex(int index); String getHistoryJSON();
void loadMQTTConfig(); void saveMQTTConfig(); void setupMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishRelayStatus(); void setRelay(bool state); void evaluateRelayState();
void addEventLog(String event); String getEventLogJSON();
void saveRelayStateToNVS(); bool loadRelayStateFromNVS(); 
void handleOTAUpload();
void handleOtaFinish();

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Alphanusa IoT Team ===");

  pinMode(RELAY_PIN, OUTPUT);
  relayOn = loadRelayStateFromNVS();
  digitalWrite(RELAY_PIN, relayOn ? HIGH : LOW);

  preferences.begin("ap-config", true);
  currentAP_SSID = preferences.getString("ap_ssid", AP_SSID_DEFAULT);
  currentAP_PASS = preferences.getString("ap_pass", AP_PASSWORD_DEFAULT);
  preferences.end();

  loadMQTTConfig();

  uint64_t chipId = ESP.getEfuseMac();
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "ESP32-%04X%08X", (uint16_t)(chipId >> 32), (uint32_t)chipId);
  mqttClientId = String(clientId);

  loadHistory();

  preferences.begin("wifi-sta", true);
  staSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("pass", "");
  preferences.end();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(currentAP_SSID.c_str(), currentAP_PASS.c_str());

  if (staSSID.length() > 0) {
    String errMsg;
    if (tryConnectSTA(staSSID, savedPass, errMsg)) addToHistory(staSSID, savedPass, "success");
    else addToHistory(staSSID, savedPass, "failed");
  }

  setupMQTT();
  timeClient.begin();

  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/save-ap", HTTP_POST, handleSaveAP);
  server.on("/connect-sta", HTTP_POST, handleConnectSTA);
  server.on("/disconnect-sta", HTTP_POST, handleDisconnectSTA);
  server.on("/save-mqtt", HTTP_POST, handleSaveMQTT);
  server.on("/force-on", handleForceOn);
  server.on("/force-off", handleForceOff);
  server.on("/status", handleStatus);
  server.on("/history", handleHistory);
  server.on("/event-log", handleEventLog);
  server.on("/delete-history", HTTP_POST, handleDeleteHistory);
  server.on("/clear-history", HTTP_POST, handleClearHistory);
  server.on("/api/ota", HTTP_POST, handleOtaFinish, handleOTAUpload);

  server.onNotFound([]() {
    IPAddress clientIP = server.client().remoteIP();
    String ipStr = clientIP.toString();
    bool isLocal = ipStr.startsWith("192.168.") || ipStr.startsWith("10.") ||
                   ipStr.startsWith("172.16.") || ipStr.startsWith("172.17.") ||
                   ipStr.startsWith("172.18.") || ipStr.startsWith("172.19.") ||
                   ipStr.startsWith("172.2") || ipStr.startsWith("172.30.") || ipStr.startsWith("172.31.");
    if (!isLocal) server.send(403, "text/html", "<h2>403 Akses Ditolak</h2>");
    else server.send(404, "text/plain", "Not Found");
  });

  server.begin();

  if (strlen(SUPERDMZ_TOKEN) > 10) {
    tunnel.begin(SUPERDMZ_TOKEN, 80);
    addEventLog("SuperDMZ tunnel dimulai");
  }
  addEventLog("Sistem siap");
}

// ===== Loop =====
void loop() {
  server.handleClient();
  tunnel.loop();

  if (WiFi.status() != WL_CONNECTED && staSSID.length() > 0) {
    if (millis() - lastWiFiReconnectAttempt > 15000) {
      WiFi.disconnect();
      preferences.begin("wifi-sta", true);
      String savedPass = preferences.getString("pass", "");
      preferences.end();
      WiFi.begin(staSSID.c_str(), savedPass.c_str());
      lastWiFiReconnectAttempt = millis();
    }
  }

  if (timeClient.update()) sudahSinkronWaktu = true;

  if (WiFi.status() == WL_CONNECTED && !staConnected) {
    staConnected = true;
    staIP = WiFi.localIP().toString();
  } else if (WiFi.status() != WL_CONNECTED && staConnected) {
    staConnected = false;
  }

  if (millis() - lastInternetCheck >= INTERNET_CHECK_INTERVAL) {
    lastInternetCheck = millis();
    if (staConnected) checkInternet();
    else internetAvailable = false;
  }

  if (staConnected && internetAvailable) {
    if (!mqttClient.connected()) {
      mqttConnected = false;
      if (millis() - lastMqttReconnect >= MQTT_RECONNECT_INTERVAL) {
        lastMqttReconnect = millis();
        String lwtTopic = "unit/" + mqttDeviceId + "/status";
        if (mqttClient.connect(mqttClientId.c_str(), lwtTopic.c_str(), 1, true, "offline")) {
          mqttConnected = true;
          lastMqttConnectedTime = millis();
          mqttClient.publish(lwtTopic.c_str(), "online", true);
          String subTopic = "unit/" + mqttDeviceId + "/lease/set";
          mqttClient.subscribe(subTopic.c_str(), 1);
          publishRelayStatus();
        }
      }
    } else {
      mqttConnected = true;
      mqttClient.loop();
    }
  }
  evaluateRelayState();
}

// ===== Fungsi =====
void handleOtaFinish() {
  bool ok = !Update.hasError();
  String msg = ok ? "OTA Received" : String("err:") + Update.errorString();
  
  // Balas ke SuperDMZ, lalu restart
  server.send(ok ? 200 : 500, "text/plain", msg);
  addEventLog("OTA finish: " + msg);
  if (ok) { 
    delay(1000); 
    ESP.restart(); 
  }
}

void handleOTAUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    addEventLog("OTA Start");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      addEventLog("OTA Begin Failed");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      addEventLog("OTA Write Failed");
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      addEventLog("OTA Success, menunggu restart...");
    } else {
      addEventLog("OTA End Failed: " + String(Update.errorString()));
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  String expectedTopic = "unit/" + mqttDeviceId + "/lease/set";
  if (String(topic) == expectedTopic) {
    lastActiveUntil = doc["active_until"] | 0;
    graceSeconds = doc["grace_s"] | 0;
    overrideMode = doc["override"] | "AUTO";
    lastRev = doc["rev"] | 0;
    evaluateRelayState();
    JsonDocument stateDoc;
    stateDoc["rev"] = lastRev;
    stateDoc["relay_on"] = relayOn;
    stateDoc["reason"] = (overrideMode != "AUTO") ? "override" : "lease_active";
    String statePayload; serializeJson(stateDoc, statePayload);
    mqttClient.publish(("unit/" + mqttDeviceId + "/lease/state").c_str(), statePayload.c_str());
  }
}

void evaluateRelayState() {
  bool shouldBeOn = false;
  unsigned long now = timeClient.getEpochTime();
  if (!sudahSinkronWaktu) return;
  if (lastActiveUntil == 0 && overrideMode == "AUTO") return;
  if (!mqttConnected && (millis() - lastMqttConnectedTime < FAILSAFE_TIMEOUT)) {
    if (!relayOn) setRelay(true);
    return;
  }
  if (overrideMode == "ON") shouldBeOn = true;
  else if (overrideMode == "OFF") shouldBeOn = false;
  else shouldBeOn = (now < (lastActiveUntil + graceSeconds));
  if (relayOn != shouldBeOn) { setRelay(shouldBeOn); publishRelayStatus(); }
}

void publishRelayStatus() {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["relay_on"] = relayOn;
  doc["reason"] = (overrideMode != "AUTO") ? "override" : "lease_active";
  String payload; serializeJson(doc, payload);
  mqttClient.publish(("unit/" + mqttDeviceId + "/lease/state").c_str(), payload.c_str(), true);
}

void setRelay(bool state) { relayOn = state; digitalWrite(RELAY_PIN, state ? HIGH : LOW); saveRelayStateToNVS(); }
void saveRelayStateToNVS() { preferences.begin("relay-state", false); preferences.putBool("state", relayOn); preferences.end(); }
bool loadRelayStateFromNVS() { preferences.begin("relay-state", true); bool s = preferences.getBool("state", false); preferences.end(); return s; }

void setupMQTT() {
  mqttClient.setServer(mqttBroker.c_str(), mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  mqttClient.setBufferSize(1024);
}

void loadMQTTConfig() {
  preferences.begin("mqtt-config", true);
  mqttBroker = preferences.getString("broker", MQTT_BROKER_DEFAULT);
  mqttPort = preferences.getInt("port", MQTT_PORT_DEFAULT);
  mqttDeviceId = preferences.getString("device_id", MQTT_DEVICE_ID_DEFAULT);
  preferences.end();
}
void saveMQTTConfig() {
  preferences.begin("mqtt-config", false);
  preferences.putString("broker", mqttBroker);
  preferences.putInt("port", mqttPort);
  preferences.putString("device_id", mqttDeviceId);
  preferences.end();
}

bool tryConnectSTA(String ssid, String pass, String &errorMsg) {
  WiFi.disconnect(false, false); delay(200);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) { delay(500); }
  if (WiFi.status() == WL_CONNECTED) {
    staConnected = true; staSSID = ssid; staIP = WiFi.localIP().toString();
    return true;
  }
  return false;
}

void checkInternet() { WiFiClient client; internetAvailable = client.connect("8.8.8.8", 53); client.stop(); }

void handleScan() {
  int n = WiFi.scanNetworks(); String json = "[";
  for (int i = 0; i < n; i++) { if (i > 0) json += ","; json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}"; }
  json += "]"; WiFi.scanDelete(); server.send(200, "application/json", json);
}

void handleSaveAP() {
  if (server.hasArg("ap_ssid") && server.hasArg("ap_pass")) {
    preferences.begin("ap-config", false);
    preferences.putString("ap_ssid", server.arg("ap_ssid"));
    preferences.putString("ap_pass", server.arg("ap_pass"));
    preferences.end();
    server.send(200, "text/html", "<h2>AP disimpan! Restart...</h2>"); delay(3000); ESP.restart();
  }
}

void handleConnectSTA() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    preferences.begin("wifi-sta", false);
    preferences.putString("ssid", server.arg("ssid"));
    preferences.putString("pass", server.arg("pass"));
    preferences.end();
    String errMsg; tryConnectSTA(server.arg("ssid"), server.arg("pass"), errMsg);
    server.sendHeader("Location", "/"); server.send(303);
  }
}

void handleDisconnectSTA() { preferences.begin("wifi-sta", false); preferences.clear(); preferences.end(); WiFi.disconnect(true); ESP.restart(); }
void handleSaveMQTT() {
  if (server.hasArg("broker") && server.hasArg("port") && server.hasArg("device_id")) {
    mqttBroker = server.arg("broker"); mqttPort = server.arg("port").toInt(); mqttDeviceId = server.arg("device_id");
    saveMQTTConfig(); server.send(200, "text/html", "<h2>MQTT disimpan! Restart...</h2>"); delay(3000); ESP.restart();
  }
}

void handleForceOn() { setRelay(true); server.sendHeader("Location", "/"); server.send(303); }
void handleForceOff() { setRelay(false); server.sendHeader("Location", "/"); server.send(303); }

void handleStatus() {
  String json = "{\"relay\":" + String(relayOn ? "true" : "false") + ",\"sta_connected\":" + String(staConnected ? "true" : "false") +
                ",\"internet\":" + String(internetAvailable ? "true" : "false") + ",\"mqtt_connected\":" + String(mqttConnected ? "true" : "false") +
                ",\"mqtt_broker\":\"" + mqttBroker + "\",\"mqtt_device_id\":\"" + mqttDeviceId + "\"}";
  server.send(200, "application/json", json);
}

void handleHistory() { server.send(200, "application/json", getHistoryJSON()); }
void handleEventLog() { server.send(200, "application/json", getEventLogJSON()); }
void handleDeleteHistory() { if (server.hasArg("index")) { deleteHistoryByIndex(server.arg("index").toInt()); server.send(200, "text/plain", "OK"); } }
void handleClearHistory() { historyCount = 0; saveHistory(); server.send(200, "text/plain", "OK"); }

void loadHistory() {
  preferences.begin("wifi-hist", true); historyCount = preferences.getInt("count", 0);
  for (int i = 0; i < historyCount; i++) {
    histories[i].ssid = preferences.getString(("ssid" + String(i)).c_str(), "");
    histories[i].pass = preferences.getString(("pass" + String(i)).c_str(), "");
    histories[i].status = preferences.getString(("stat" + String(i)).c_str(), "");
  }
  preferences.end();
}
void saveHistory() {
  preferences.begin("wifi-hist", false); preferences.putInt("count", historyCount);
  for (int i = 0; i < historyCount; i++) {
    preferences.putString(("ssid" + String(i)).c_str(), histories[i].ssid);
    preferences.putString(("pass" + String(i)).c_str(), histories[i].pass);
    preferences.putString(("stat" + String(i)).c_str(), histories[i].status);
  }
  preferences.end();
}
void addToHistory(String ssid, String pass, String status) {
  int existingIdx = -1;
  for (int i = 0; i < historyCount; i++) { if (histories[i].ssid == ssid) { existingIdx = i; break; } }
  if (existingIdx >= 0) {
    for (int i = existingIdx; i > 0; i--) histories[i] = histories[i - 1];
  } else {
    int newCount = historyCount < MAX_HISTORY ? historyCount + 1 : MAX_HISTORY;
    for (int i = newCount - 1; i > 0; i--) histories[i] = histories[i - 1];
    historyCount = newCount;
  }
  histories[0].ssid = ssid; histories[0].pass = pass; histories[0].status = status;
  saveHistory();
}
void deleteHistoryByIndex(int index) {
  if (index < 0 || index >= historyCount) return;
  for (int i = index; i < historyCount - 1; i++) histories[i] = histories[i + 1];
  historyCount--; saveHistory();
}
String getHistoryJSON() {
  String json = "[";
  for (int i = 0; i < historyCount; i++) {
    if (i > 0) json += ",";
    json += "{\"index\":" + String(i) + ",\"ssid\":\"" + histories[i].ssid + "\",\"pass\":\"" + histories[i].pass + "\",\"status\":\"" + histories[i].status + "\"}";
  }
  json += "]"; return json;
}

void addEventLog(String event) {
  for (int i = MAX_EVENT_LOG - 1; i > 0; i--) eventLogs[i] = eventLogs[i - 1];
  eventLogs[0].event = event; eventLogs[0].timestamp = millis() / 1000;
  if (eventLogCount < MAX_EVENT_LOG) eventLogCount++;
}
String getEventLogJSON() {
  String json = "[";
  for (int i = 0; i < eventLogCount; i++) {
    if (i > 0) json += ",";
    json += "{\"event\":\"" + eventLogs[i].event + "\",\"time\":" + String(eventLogs[i].timestamp) + "}";
  }
  json += "]"; return json;
}

// ===== Halaman Web =====
void handleRoot() {
  preferences.begin("wifi-sta", true);
  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("pass", "");
  preferences.end();

  String s = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>Alphanusa IoT Team</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #f0f2f5; padding: 16px; }
    .card { background: white; border-radius: 12px; padding: 20px; margin-bottom: 16px; }
    h3 { border-bottom: 1px solid #ddd; padding-bottom: 8px; }
    input { width: 100%; padding: 10px; margin: 6px 0; border: 1px solid #ccc; border-radius: 6px; }
    button { padding: 12px; margin: 4px 0; border-radius: 8px; border: none; color: white; font-weight: bold; cursor: pointer; width: 100%;}
    .btn-primary { background: #2196F3; }
    .btn-success { background: #4CAF50; }
    .btn-danger { background: #f44336; }
    .small { font-size: 12px; color: #666; }
    .log-item { font-size: 12px; padding: 4px 0; border-bottom: 1px solid #eee; }
  </style>
</head>
<body>
  <h1>Alphanusa IoT Team</h1>
  <div class='card'>
    <h3>Status Relay</h3>
    <p id='relayStatus' style='font-size:28px; font-weight:bold;'>--</p>
    <a href='/force-on'><button class='btn-success'>Paksa Nyala</button></a>
    <a href='/force-off'><button class='btn-danger'>Paksa Padam</button></a>
  </div>
  <div class='card'>
    <h3>Log Kejadian</h3>
    <div id='eventLogList'><p class='small'>Memuat...</p></div>
  </div>
  <div class='card'>
    <h3>Pengaturan UHUUUUYYYY</h3>
    <div class='indicator'><span id='mqttStatus'>Memuat...</span></div>
    <form action='/save-mqtt' method='POST'>
      <label>Broker</label><input type='text' name='broker' id='broker' value='__MQTT_BROKER__'>
      <label>Port</label><input type='number' name='port' id='port' value='__MQTT_PORT__'>
      <label>Device ID</label><input type='text' name='device_id' id='device_id' value='__MQTT_DEVICE_ID__'>
      <button type='submit' class='btn-primary'>Simpan MQTT</button>
    </form>
  </div>
  <div class='card'>
    <h3>WiFi (STA)</h3>
    <div class='indicator'><span id='staStatus'>Memuat...</span></div>
    <button class='btn-primary' onclick='scanWiFi()'>Scan WiFi</button>
    <div id='scanResults' style='max-height:150px; overflow-y:auto; margin-top:10px;'></div>
    <form action='/connect-sta' method='POST'>
      <label>SSID</label><input type='text' name='ssid' id='ssid' value='__SAVED_SSID__'>
      <label>Password</label><input type='password' name='pass' id='sta_pass' value='__SAVED_PASS__'>
      <button type='submit' class='btn-primary'>Hubungkan</button>
    </form>
  </div>
  <div class='card'>
    <h3>Riwayat Koneksi</h3>
    <div id='historyList'><p class='small'>Memuat...</p></div>
    <button class='btn-danger' onclick='clearHistory()'>Hapus Semua</button>
  </div>
  <script>
    function loadStatus() {
      fetch('/status').then(r => r.json()).then(d => {
        document.getElementById('relayStatus').textContent = d.relay ? 'ON' : 'OFF';
        document.getElementById('relayStatus').style.color = d.relay ? '#4CAF50' : '#f44336';
        document.getElementById('mqttStatus').textContent = d.mqtt_connected ? 'MQTT: Terhubung' : 'MQTT: Terputus';
        document.getElementById('staStatus').textContent = d.sta_connected ? 'WiFi: Terhubung' : 'WiFi: Terputus';
      });
    }
    function loadHistory() {
      fetch('/history').then(r => r.json()).then(list => {
        var c = document.getElementById('historyList');
        if (!list.length) { c.innerHTML = '<p class="small">Belum ada</p>'; return; }
        c.innerHTML = '';
        list.forEach(item => {
          var d = document.createElement('div'); d.className = 'history-item';
          d.innerHTML = '<span>' + item.ssid + '</span><button class="btn-danger" style="width:auto;padding:4px 8px;" onclick="deleteHistory(' + item.index + ')">X</button>';
          c.appendChild(d);
        });
      });
    }
    function loadEventLog() {
      fetch('/event-log').then(r => r.json()).then(list => {
        var c = document.getElementById('eventLogList');
        if (!list.length) { c.innerHTML = '<p class="small">Belum ada</p>'; return; }
        c.innerHTML = '';
        list.forEach(item => {
          var d = document.createElement('div'); d.className = 'log-item';
          d.textContent = '[' + item.time + 's] ' + item.event;
          c.appendChild(d);
        });
      });
    }
    function scanWiFi() {
      var c = document.getElementById('scanResults'); c.innerHTML = 'Memindai...';
      fetch('/scan').then(r => r.json()).then(nets => {
        c.innerHTML = ''; if (!nets.length) { c.innerHTML = 'Tidak ada WiFi'; return; }
        nets.forEach(n => {
          var d = document.createElement('div'); d.className = 'log-item';
          d.textContent = n.ssid + ' (' + n.rssi + ' dBm)';
          d.style.cursor = 'pointer';
          d.onclick = function() { document.getElementById('ssid').value = n.ssid; };
          c.appendChild(d);
        });
      });
    }
    function deleteHistory(idx) { fetch('/delete-history', {method:'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: 'index='+idx}).then(loadHistory); }
    function clearHistory() { fetch('/clear-history', {method:'POST'}).then(loadHistory); }
    loadStatus(); loadHistory(); loadEventLog();
    setInterval(loadStatus, 5000);
  </script>
</body>
</html>
)rawliteral";
  s.replace("__SAVED_SSID__", savedSSID);
  s.replace("__SAVED_PASS__", savedPass);
  s.replace("__MQTT_BROKER__", mqttBroker);
  s.replace("__MQTT_PORT__", String(mqttPort));
  s.replace("__MQTT_DEVICE_ID__", mqttDeviceId);
  server.send(200, "text/html", s);
}