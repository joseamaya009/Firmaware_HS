#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <cstring>
#include <esp_system.h>
#include <string>
#include <time.h>

#if __has_include("../provisioning/config.h")
#include "../provisioning/config.h"
#else
// Permite compilar desde un clon limpio; la configuración local sigue
// teniendo prioridad cuando provisioning/config.h existe.
#include "../provisioning/config.example.h"
#endif

#ifndef HIDROSMART_FIRMWARE_VERSION
#define HIDROSMART_FIRMWARE_VERSION "1.1.0"
#endif

#ifndef HIDROSMART_FLOW_FACTOR
#define HIDROSMART_FLOW_FACTOR 7.5f
#endif

#ifndef HIDROSMART_SAMPLE_INTERVAL_MS
#define HIDROSMART_SAMPLE_INTERVAL_MS 5000UL
#endif

#ifndef HIDROSMART_MQTT_KEEPALIVE_SECONDS
#define HIDROSMART_MQTT_KEEPALIVE_SECONDS 30
#endif

namespace {
constexpr char TELEMETRY_SUFFIX[] = "/telemetry";
constexpr char STATUS_SUFFIX[] = "/status";
constexpr char COMMANDS_SUFFIX[] = "/commands";
constexpr char NVS_NAMESPACE[] = "hidrosmart";
constexpr char NVS_NETWORKS_KEY[] = "networks";
constexpr char NVS_MQTT_HOST_KEY[] = "mqttHost";
constexpr char NVS_MQTT_PORT_KEY[] = "mqttPort";
constexpr char NVS_DEVICE_CODE_KEY[] = "deviceCode";
constexpr char NVS_BLE_LOCKED_KEY[] = "bleLocked";
constexpr char NVS_LINKED_HOME_KEY[] = "linkedHome";
constexpr char NVS_TOTAL_LITERS_KEY[] = "totalLiters";
constexpr char BLE_SERVICE_UUID[] = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
constexpr char BLE_CONFIG_CHAR_UUID[] = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
constexpr char BLE_STATUS_CHAR_UUID[] = "beb5483e-36e1-4688-b7f5-ea07361b26a9";
constexpr uint8_t MAX_WIFI_NETWORKS = 5;
constexpr unsigned long WIFI_TIMEOUT_MS = 15000UL;
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000UL;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000UL;
constexpr unsigned long TOTAL_SAVE_INTERVAL_MS = 300000UL;
constexpr float TOTAL_SAVE_MIN_DELTA_LITERS = 1.0f;
constexpr uint8_t CONFIG_BUTTON_PIN = 0;
constexpr size_t BLE_PAYLOAD_MAX = 768;

struct WifiCredential {
  String ssid;
  String password;
  String mqttHost;
  uint16_t mqttPort;
};

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Preferences preferences;
NimBLECharacteristic* configCharacteristic = nullptr;
NimBLECharacteristic* statusCharacteristic = nullptr;

WifiCredential knownNetworks[MAX_WIFI_NETWORKS];
uint8_t knownNetworkCount = 0;
String hardwareId;
String deviceCode;
String connectedSsid;
String mqttHost;
uint16_t mqttPort = HIDROSMART_MQTT_PORT;

volatile uint32_t pulseCount = 0;
uint32_t sequence = 0;
uint32_t bootId = 0;
float totalLiters = 0.0f;
float lastPersistedTotalLiters = 0.0f;
uint32_t pendingPulses = 0;
float pendingConsumptionLiters = 0.0f;
float pendingSampleSeconds = 0.0f;
unsigned long lastSampleAt = 0;
unsigned long lastTotalSaveAt = 0;
unsigned long lastWifiAttemptAt = 0;
unsigned long lastMqttAttemptAt = 0;
bool bleInitialized = false;
bool bleAdvertising = false;
volatile bool bleClientConnected = false;
char pendingBlePayload[BLE_PAYLOAD_MAX] = {};
volatile bool pendingBlePayloadReady = false;
portMUX_TYPE blePayloadMux = portMUX_INITIALIZER_UNLOCKED;
bool bleLocked = false;
bool linkedToHome = false;
bool configButtonPressed = false;
bool configButtonHandled = false;
unsigned long configButtonStartedAt = 0;

void startProvisioningBLE();
void stopProvisioningBLE();
void notifyStatus(const char* state);
void saveBleLock(bool locked);
void saveLinkedToHome(bool linked);
void applyBlePolicy();

bool takePendingBlePayload(String& payload) {
  char copiedPayload[BLE_PAYLOAD_MAX] = {};
  bool hasPayload = false;

  portENTER_CRITICAL(&blePayloadMux);
  if (pendingBlePayloadReady) {
    std::strncpy(copiedPayload, pendingBlePayload, BLE_PAYLOAD_MAX - 1);
    copiedPayload[BLE_PAYLOAD_MAX - 1] = '\0';
    pendingBlePayload[0] = '\0';
    pendingBlePayloadReady = false;
    hasPayload = true;
  }
  portEXIT_CRITICAL(&blePayloadMux);

  if (hasPayload) payload = String(copiedPayload);
  return hasPayload;
}

bool isConfiguredText(const char* value) {
  return value != nullptr && std::strlen(value) > 0 && std::strncmp(value, "CAMBIA_", 7) != 0;
}

bool isSafeDeviceCode(const String& value) {
  if (value.isEmpty() || value.length() > 100) return false;

  for (size_t index = 0; index < value.length(); index++) {
    const char character = value[index];
    const bool alphanumeric =
      (character >= 'A' && character <= 'Z') ||
      (character >= 'a' && character <= 'z') ||
      (character >= '0' && character <= '9');
    if (!alphanumeric && character != '-' && character != '_') return false;
  }
  return true;
}

String getHardwareId() {
  const uint64_t chipId = ESP.getEfuseMac();
  char id[20];
  snprintf(id, sizeof(id), "HS-%012llX", static_cast<unsigned long long>(chipId & 0xFFFFFFFFFFFFULL));
  return String(id);
}

String deviceTopic(const char* suffix) {
  return String("hidrosmart/devices/") + deviceCode + suffix;
}

String isoTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return "";

  char value[25];
  strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(value);
}

String statusPayload(const char* status) {
  const String timestamp = isoTimestamp();
  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  const String activeSsid = WiFi.status() == WL_CONNECTED && connectedSsid.isEmpty()
    ? WiFi.SSID()
    : connectedSsid;
  // Este campo viaja por MQTT y debe usar exactamente los estados que valida
  // el backend. Los estados detallados del proceso BLE siguen viajando por la
  // caracteristica BLE de estado.
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool mqttConnected = mqttClient.connected();
  const char* provisioningState = linkedToHome && std::strcmp(status, "ONLINE") == 0
    ? "COMPLETE"
    : (mqttConnected
      ? "MQTT_CONNECTED"
      : (wifiConnected ? "WIFI_CONNECTED" : (bleAdvertising ? "BLE_READY" : "PENDING")));
  JsonDocument document;
  document["deviceId"] = deviceCode;
  document["hardwareId"] = hardwareId;
  document["status"] = status;
  document["provisioningState"] = provisioningState;
  document["timestamp"] = timestamp;
  document["firmwareVersion"] = HIDROSMART_FIRMWARE_VERSION;
  document["lastIp"] = ip;
  document["linkedToHome"] = linkedToHome;
  document["bleLocked"] = bleLocked;
  if (!activeSsid.isEmpty()) document["ssid"] = activeSsid;
  document["wifiRssiDbm"] = WiFi.RSSI();

  String serialized;
  serializeJson(document, serialized);
  return serialized;
}

void IRAM_ATTR onPulse() {
  pulseCount++;
}

void saveNetworks() {
  JsonDocument document;
  JsonArray networks = document["networks"].to<JsonArray>();

  for (uint8_t index = 0; index < knownNetworkCount; index++) {
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = knownNetworks[index].ssid;
    network["password"] = knownNetworks[index].password;
    network["mqttHost"] = knownNetworks[index].mqttHost;
    network["mqttPort"] = knownNetworks[index].mqttPort;
  }

  String serialized;
  serializeJson(document, serialized);

  preferences.begin(NVS_NAMESPACE, false);
  preferences.putString(NVS_NETWORKS_KEY, serialized);
  preferences.end();
}

void saveNetwork(const String& ssid, const String& password, const String& brokerHost, uint16_t brokerPort) {
  for (uint8_t index = 0; index < knownNetworkCount; index++) {
    if (knownNetworks[index].ssid == ssid) {
      knownNetworks[index].password = password;
      knownNetworks[index].mqttHost = brokerHost;
      knownNetworks[index].mqttPort = brokerPort;
      saveNetworks();
      return;
    }
  }

  if (knownNetworkCount < MAX_WIFI_NETWORKS) {
    knownNetworks[knownNetworkCount] = {ssid, password, brokerHost, brokerPort};
    knownNetworkCount++;
  } else {
    for (uint8_t index = 1; index < MAX_WIFI_NETWORKS; index++) {
      knownNetworks[index - 1] = knownNetworks[index];
    }
    knownNetworks[MAX_WIFI_NETWORKS - 1] = {ssid, password, brokerHost, brokerPort};
  }

  saveNetworks();
}

void loadNetworks(const String& serialized) {
  knownNetworkCount = 0;
  if (serialized.isEmpty()) return;

  JsonDocument document;
  if (deserializeJson(document, serialized)) {
    Serial.println("[NVS] Redes guardadas invalidas; se ignoraran.");
    return;
  }

  JsonArray networks = document["networks"].as<JsonArray>();
  for (JsonObject network : networks) {
    if (knownNetworkCount >= MAX_WIFI_NETWORKS) break;
    const String ssid = network["ssid"] | "";
    if (ssid.isEmpty()) continue;
    knownNetworks[knownNetworkCount].ssid = ssid;
    knownNetworks[knownNetworkCount].password = network["password"] | "";
    knownNetworks[knownNetworkCount].mqttHost = network["mqttHost"] | mqttHost;
    knownNetworks[knownNetworkCount].mqttPort = network["mqttPort"] | static_cast<int>(mqttPort);
    if (knownNetworks[knownNetworkCount].mqttHost.isEmpty()) {
      knownNetworks[knownNetworkCount].mqttHost = HIDROSMART_MQTT_HOST;
    }
    if (knownNetworks[knownNetworkCount].mqttPort == 0) {
      knownNetworks[knownNetworkCount].mqttPort = HIDROSMART_MQTT_PORT;
    }
    knownNetworkCount++;
  }
}

void loadLegacyNetworks() {
  preferences.begin(NVS_NAMESPACE, true);
  const uint8_t legacyCount = preferences.getUChar("netCount", 0);

  for (uint8_t index = 0; index < legacyCount && index < MAX_WIFI_NETWORKS; index++) {
    const String suffix = String(index);
    const String ssid = preferences.getString((String("ssid") + suffix).c_str(), "");
    if (ssid.isEmpty()) continue;

    knownNetworks[knownNetworkCount].ssid = ssid;
    knownNetworks[knownNetworkCount].password = preferences.getString((String("pass") + suffix).c_str(), "");
    knownNetworks[knownNetworkCount].mqttHost = preferences.getString((String("host") + suffix).c_str(), mqttHost);
    knownNetworks[knownNetworkCount].mqttPort = preferences.getUShort(
      (String("port") + suffix).c_str(),
      mqttPort
    );
    if (knownNetworks[knownNetworkCount].mqttHost.isEmpty()) {
      knownNetworks[knownNetworkCount].mqttHost = HIDROSMART_MQTT_HOST;
    }
    if (knownNetworks[knownNetworkCount].mqttPort == 0) {
      knownNetworks[knownNetworkCount].mqttPort = HIDROSMART_MQTT_PORT;
    }
    knownNetworkCount++;
  }

  preferences.end();
}

void loadConfiguration() {
  preferences.begin(NVS_NAMESPACE, true);
  const String serializedNetworks = preferences.getString(NVS_NETWORKS_KEY, "");
  mqttHost = preferences.getString(NVS_MQTT_HOST_KEY, "");
  mqttPort = preferences.getUShort(NVS_MQTT_PORT_KEY, HIDROSMART_MQTT_PORT);
  deviceCode = preferences.getString(NVS_DEVICE_CODE_KEY, "");
  bleLocked = preferences.getBool(NVS_BLE_LOCKED_KEY, false);
  linkedToHome = preferences.getBool(NVS_LINKED_HOME_KEY, false);
  totalLiters = preferences.isKey(NVS_TOTAL_LITERS_KEY)
    ? preferences.getFloat(NVS_TOTAL_LITERS_KEY, 0.0f)
    : 0.0f;
  preferences.end();

  loadNetworks(serializedNetworks);
  if (knownNetworkCount == 0) {
    loadLegacyNetworks();
    if (knownNetworkCount > 0) {
      saveNetworks();
      Serial.println("[NVS] Redes antiguas migradas al formato actual.");
    }
  }

  if (deviceCode.isEmpty() && isConfiguredText(HIDROSMART_DEVICE_CODE)) {
    deviceCode = HIDROSMART_DEVICE_CODE;
  }
  if (deviceCode.isEmpty()) deviceCode = hardwareId;

  if (mqttHost.isEmpty()) mqttHost = HIDROSMART_MQTT_HOST;
  if (mqttPort == 0) mqttPort = HIDROSMART_MQTT_PORT;

  lastPersistedTotalLiters = totalLiters;

  // La asociacion es la autoridad para BLE. Un ESP32 sin hogar nunca debe
  // quedar bloqueado y debe poder volver a vincularse por Bluetooth.
  if (!linkedToHome && bleLocked) {
    bleLocked = false;
    saveBleLock(false);
  }

  // Compatibilidad de migracion: una red definida en config.h se prueba una vez
  // y solo se guarda en NVS desde connectKnownWifi cuando conecta correctamente.
  if (knownNetworkCount == 0 && isConfiguredText(HIDROSMART_WIFI_SSID)) {
    knownNetworks[0] = {
      HIDROSMART_WIFI_SSID,
      HIDROSMART_WIFI_PASSWORD,
      mqttHost,
      mqttPort
    };
    knownNetworkCount = 1;
  }
}

void saveMqttConfiguration(const String& host, uint16_t port) {
  mqttHost = host;
  mqttPort = port;

  preferences.begin(NVS_NAMESPACE, false);
  preferences.putString(NVS_MQTT_HOST_KEY, mqttHost);
  preferences.putUShort(NVS_MQTT_PORT_KEY, mqttPort);
  preferences.putString(NVS_DEVICE_CODE_KEY, deviceCode);
  preferences.end();
}

void saveBleLock(bool locked) {
  bleLocked = locked;
  preferences.begin(NVS_NAMESPACE, false);
  preferences.putBool(NVS_BLE_LOCKED_KEY, bleLocked);
  preferences.end();
}

void saveLinkedToHome(bool linked) {
  linkedToHome = linked;
  preferences.begin(NVS_NAMESPACE, false);
  preferences.putBool(NVS_LINKED_HOME_KEY, linkedToHome);
  preferences.end();
}

void saveTotalLiters() {
  preferences.begin(NVS_NAMESPACE, false);
  preferences.putFloat(NVS_TOTAL_LITERS_KEY, totalLiters);
  preferences.end();
  lastPersistedTotalLiters = totalLiters;
  lastTotalSaveAt = millis();
}

bool connectToWifi(const String& ssid, const String& password, unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false);
  connectedSsid = "";
  delay(200);
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.print("[WIFI] Probando red: ");
  Serial.println(ssid);

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.print("[WIFI] Conectado. IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("[WIFI] RSSI: ");
  Serial.println(WiFi.RSSI());
  connectedSsid = ssid;
  return true;
}

bool connectKnownWifi() {
  for (uint8_t index = 0; index < knownNetworkCount; index++) {
    if (connectToWifi(knownNetworks[index].ssid, knownNetworks[index].password, WIFI_TIMEOUT_MS)) {
      mqttHost = knownNetworks[index].mqttHost.isEmpty() ? String(HIDROSMART_MQTT_HOST) : knownNetworks[index].mqttHost;
      mqttPort = knownNetworks[index].mqttPort == 0 ? HIDROSMART_MQTT_PORT : knownNetworks[index].mqttPort;
      saveNetwork(knownNetworks[index].ssid, knownNetworks[index].password, mqttHost, mqttPort);
      saveMqttConfiguration(mqttHost, mqttPort);
      return true;
    }
  }
  return false;
}

void setBleStatus(const char* state, bool shouldNotify) {
  if (!statusCharacteristic) return;

  JsonDocument document;
  if (state != nullptr && std::strlen(state) > 0) document["state"] = state;
  document["hardwareId"] = hardwareId;
  document["deviceCode"] = deviceCode;
  document["firmwareVersion"] = HIDROSMART_FIRMWARE_VERSION;
  document["savedNetworks"] = knownNetworkCount;
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool mqttConnected = mqttClient.connected();
  document["wifiConnected"] = wifiConnected;
  document["mqttConnected"] = mqttConnected;
  document["bleEnabled"] = bleAdvertising;
  document["linkedToHome"] = linkedToHome;
  document["bleLocked"] = bleLocked;
  const String activeSsid = wifiConnected && connectedSsid.isEmpty() ? WiFi.SSID() : connectedSsid;
  if (!activeSsid.isEmpty()) document["ssid"] = activeSsid;
  if (wifiConnected) {
    document["ip"] = WiFi.localIP().toString();
    document["rssi"] = WiFi.RSSI();
  }

  String serialized;
  serializeJson(document, serialized);
  statusCharacteristic->setValue(serialized.c_str());
  if (shouldNotify) statusCharacteristic->notify();
  Serial.print("[BLE] Estado: ");
  Serial.println(serialized);
}

void notifyStatus(const char* state) {
  setBleStatus(state, true);
}

void handleMqttCommand(char* topic, uint8_t* payload, unsigned int length) {
  if (String(topic) != deviceTopic(COMMANDS_SUFFIX)) return;

  String serialized;
  serialized.reserve(length + 1);
  for (unsigned int index = 0; index < length; index++) {
    serialized += static_cast<char>(payload[index]);
  }

  JsonDocument document;
  if (deserializeJson(document, serialized)) return;

  String command = document["command"] | "";
  command.toLowerCase();

  if (command == "enable_ble") {
    saveBleLock(false);
    startProvisioningBLE();
    notifyStatus("ble_enabled");
    return;
  }

  if (command == "confirm_link") {
    saveLinkedToHome(true);
    saveBleLock(true);
    applyBlePolicy();
    notifyStatus("provisioning_complete");
    return;
  }

  if (command == "unlink") {
    saveLinkedToHome(false);
    saveBleLock(false);
    applyBlePolicy();
    notifyStatus("ble_enabled");
    return;
  }

  if (command == "disable_ble") {
    if (!linkedToHome) {
      notifyStatus("ble_disable_rejected");
      return;
    }
    saveBleLock(true);
    applyBlePolicy();
    notifyStatus("ble_disabled");
  }
}

bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqttHost.isEmpty()) return false;

  mqttClient.setServer(mqttHost.c_str(), mqttPort);
  mqttClient.setCallback(handleMqttCommand);
  const String clientId = deviceCode + "-" + String(bootId);
  const String statusTopic = deviceTopic(STATUS_SUFFIX);
  const String offlinePayload = statusPayload("OFFLINE");
  bool connected = false;

  if (std::strlen(HIDROSMART_MQTT_USERNAME) > 0) {
    connected = mqttClient.connect(
      clientId.c_str(), HIDROSMART_MQTT_USERNAME, HIDROSMART_MQTT_PASSWORD,
      statusTopic.c_str(), 1, true, offlinePayload.c_str()
    );
  } else {
    connected = mqttClient.connect(
      clientId.c_str(), statusTopic.c_str(), 1, true, offlinePayload.c_str()
    );
  }

  if (!connected) {
    Serial.print("[MQTT] Error de conexion, estado: ");
    Serial.println(mqttClient.state());
    return false;
  }

  const String onlinePayload = statusPayload("ONLINE");
  const bool published = mqttClient.publish(statusTopic.c_str(), onlinePayload.c_str(), true);
  const String commandsTopic = deviceTopic(COMMANDS_SUFFIX);
  const bool subscribed = mqttClient.subscribe(commandsTopic.c_str(), 1);
  Serial.print("[MQTT] Conectado a ");
  Serial.print(mqttHost);
  Serial.print(':');
  Serial.println(mqttPort);
  if (!subscribed) Serial.println("[MQTT] No se pudo suscribir al canal de comandos");
  return published && subscribed;
}

bool publishTelemetry(uint32_t pulses, float flowRateLpm, float consumptionLiters, float intervalSeconds) {
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] Broker desconectado; la lectura queda pendiente.");
    return false;
  }

  const uint32_t currentSequence = ++sequence;
  const String messageId = deviceCode + "-" + String(bootId) + "-" + String(currentSequence);
  const String timestamp = isoTimestamp();
  const String topic = deviceTopic(TELEMETRY_SUFFIX);
  char payload[512];

  snprintf(
    payload,
    sizeof(payload),
    "{\"mqttMessageId\":\"%s\",\"deviceId\":\"%s\",\"hardwareId\":\"%s\",\"timestamp\":\"%s\","
    "\"flowRateLpm\":%.3f,\"consumptionLiters\":%.5f,\"totalLiters\":%.5f,"
    "\"pulses\":%lu,\"sampleIntervalSeconds\":%.3f,\"wifiRssiDbm\":%ld,\"signalQuality\":%ld}",
    messageId.c_str(), deviceCode.c_str(), hardwareId.c_str(), timestamp.c_str(), flowRateLpm,
    consumptionLiters, totalLiters, pulses, intervalSeconds, static_cast<long>(WiFi.RSSI()),
    static_cast<long>(WiFi.RSSI())
  );

  const bool published = mqttClient.publish(topic.c_str(), payload, false);
  Serial.print("[MQTT] ");
  Serial.print(published ? "Telemetria enviada: " : "Error enviando telemetria: ");
  Serial.println(payload);
  return published;
}

void handleProvisioningPayload(const String& serialized) {
  JsonDocument document;
  if (deserializeJson(document, serialized)) {
    notifyStatus("invalid_json");
    return;
  }

  const String ssid = document["ssid"] | "";
  const String password = document["password"] | "";
  const String newMqttHost = document["mqttHost"] | mqttHost;
  const int requestedPort = document["mqttPort"] | static_cast<int>(mqttPort);
  const String requestedDeviceCode = document["deviceCode"] | deviceCode;

  if (ssid.isEmpty() || ssid.length() > 32) {
    notifyStatus("invalid_ssid");
    return;
  }
  if (password.length() > 63 || newMqttHost.length() > 253 || requestedPort < 1 || requestedPort > 65535) {
    notifyStatus("invalid_configuration");
    return;
  }
  if (!isSafeDeviceCode(requestedDeviceCode)) {
    notifyStatus("invalid_device_code");
    return;
  }

  const String previousDeviceCode = deviceCode;
  String effectivePassword = password;
  String savedPassword;
  bool hasSavedNetwork = false;
  for (uint8_t index = 0; index < knownNetworkCount; index++) {
    if (knownNetworks[index].ssid == ssid) {
      savedPassword = knownNetworks[index].password;
      hasSavedNetwork = true;
      break;
    }
  }

  // La app deja este campo vacio cuando no tiene una nueva contrasena Wi-Fi
  // que enviar. Reutilizar la guardada evita intentar entrar a una red segura
  // con una contrasena vacia.
  if (effectivePassword.isEmpty() && hasSavedNetwork) {
    effectivePassword = savedPassword;
    Serial.println("[WIFI] Se conserva la contrasena guardada para esta red.");
  }

  const bool wifiAlreadyOnRequestedNetwork = WiFi.status() == WL_CONNECTED &&
    (connectedSsid == ssid || WiFi.SSID() == ssid);
  const bool passwordUnchanged = password.isEmpty() ||
    (hasSavedNetwork && password == savedPassword);
  const bool keepCurrentWifiConnection = wifiAlreadyOnRequestedNetwork && passwordUnchanged;
  const bool deviceCodeChanging = !requestedDeviceCode.equalsIgnoreCase(previousDeviceCode);

  if (deviceCodeChanging && mqttClient.connected()) {
    const String previousStatusTopic = deviceTopic(STATUS_SUFFIX);
    const String previousOfflinePayload = statusPayload("OFFLINE");
    mqttClient.publish(previousStatusTopic.c_str(), previousOfflinePayload.c_str(), true);
    mqttClient.disconnect();
  }

  deviceCode = requestedDeviceCode;
  notifyStatus("configuration_received");
  notifyStatus("wifi_connecting");
  if (keepCurrentWifiConnection) {
    connectedSsid = ssid;
    Serial.println("[WIFI] La placa ya esta en esa red; se conserva la conexion.");
  } else if (!connectToWifi(ssid, effectivePassword, WIFI_TIMEOUT_MS)) {
    notifyStatus("wifi_failed");
    deviceCode = previousDeviceCode;
    return;
  }

  mqttHost = newMqttHost;
  mqttPort = static_cast<uint16_t>(requestedPort);
  saveNetwork(ssid, effectivePassword, mqttHost, mqttPort);
  saveMqttConfiguration(mqttHost, mqttPort);
  notifyStatus("wifi_connected");

  if (connectMqtt()) {
    if (linkedToHome) {
      saveBleLock(true);
      applyBlePolicy();
      notifyStatus("provisioning_complete");
    } else {
      // La red ya quedo guardada, pero el backend aun debe confirmar que el
      // equipo pertenece a un hogar. Mientras tanto BLE permanece disponible.
      notifyStatus("waiting_link_confirmation");
    }
  } else {
    notifyStatus("mqtt_unreachable");
  }
}

class ProvisioningCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const std::string value = characteristic->getValue();
    if (value.empty()) return;

    // No bloquear el callback GATT: conectar Wi-Fi y MQTT puede tardar varios
    // segundos y el navegador necesita recibir primero la respuesta del write.
    portENTER_CRITICAL(&blePayloadMux);
    std::strncpy(pendingBlePayload, value.c_str(), BLE_PAYLOAD_MAX - 1);
    pendingBlePayload[BLE_PAYLOAD_MAX - 1] = '\0';
    pendingBlePayloadReady = true;
    portEXIT_CRITICAL(&blePayloadMux);
  }
};

class ProvisioningServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    // Un intervalo corto y un timeout amplio evitan que Windows descarte el
    // enlace mientras descubre el servicio y sus caracteristicas GATT.
    server->updateConnParams(connInfo.getConnHandle(), 12, 24, 0, 200);
    bleClientConnected = true;
    Serial.println("[BLE] Cliente conectado; GATT listo.");
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    bleClientConnected = false;
    // NimBLE detiene el advertising al aceptar una conexion. Si el usuario
    // cancela el selector BLE o la conexion falla, debe volver a descubrirse
    // sin reiniciar el ESP32. Un equipo ya vinculado y bloqueado permanece
    // fuera de BLE hasta que el backend mande unlink o se use BOOT.
    if (!linkedToHome || !bleLocked) {
      NimBLEDevice::startAdvertising();
      bleAdvertising = true;
      setBleStatus("ready", false);
      Serial.print("[BLE] Cliente desconectado (razon ");
      Serial.print(reason);
      Serial.println("); advertising reiniciado.");
    }
  }
};

void startProvisioningBLE() {
  const String bleName = String("HidroSmart-") + hardwareId.substring(hardwareId.length() - 6);
  if (!bleInitialized) {
    NimBLEDevice::init(bleName.c_str());
    // Mantiene el enlace de provisionamiento estable durante el escaneo y la
    // lectura/escritura GATT, especialmente cuando el ESP32 esta junto a un
    // router Wi-Fi activo.
    NimBLEDevice::setPower(9);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ProvisioningServerCallbacks());
    NimBLEService* service = server->createService(BLE_SERVICE_UUID);
    configCharacteristic = service->createCharacteristic(
      BLE_CONFIG_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    configCharacteristic->setCallbacks(new ProvisioningCallbacks());
    statusCharacteristic = service->createCharacteristic(
      BLE_STATUS_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    configCharacteristic->setValue("");
    statusCharacteristic->setValue("{}");

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    // El nombre y el UUID de servicio no caben juntos en el anuncio BLE
    // principal. El nombre se entrega en la respuesta de escaneo para que
    // Chrome/Edge pueda descubrir el ESP32 sin truncar el anuncio.
    advertising->enableScanResponse(true);
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setName(bleName.c_str());
    bleInitialized = true;
  }

  if (!bleAdvertising) {
    // Inicia el GATT despues de registrar todos los atributos. En NimBLE
    // 2.x service->start() ya no registra el servicio; debe arrancar el
    // servidor antes de publicar el anuncio.
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server != nullptr && !server->start()) {
      Serial.println("[BLE] No se pudo iniciar el servidor GATT");
      return;
    }
    NimBLEDevice::startAdvertising();
    bleAdvertising = true;
    // El estado ready debe reflejar que el anuncio BLE ya esta activo.
    setBleStatus("ready", false);
    Serial.print("[BLE] Provisionamiento activo: ");
    Serial.println(bleName);
  }
}

void stopProvisioningBLE() {
  if (!bleInitialized) return;

  // Si el backend acaba de confirmar la vinculacion mientras el navegador
  // sigue conectado, cerrar tambien las conexiones GATT existentes. De lo
  // contrario el anuncio se detiene, pero el cliente puede seguir viendo BLE
  // como conectado y el equipo queda en un estado ambiguo.
  NimBLEServer* server = NimBLEDevice::getServer();
  if (server != nullptr) {
    for (const uint16_t connHandle : server->getPeerDevices()) {
      server->disconnect(connHandle);
    }
  }

  NimBLEDevice::getAdvertising()->stop();
  bleAdvertising = false;
  bleClientConnected = false;
  Serial.println("[BLE] Provisionamiento detenido: dispositivo asociado");
}

void applyBlePolicy() {
  // Sin hogar asociado, el ESP32 siempre debe poder ser descubierto y
  // vinculado, aunque conserve una red Wi-Fi en NVS.
  if (!linkedToHome) {
    if (bleLocked) saveBleLock(false);
    if (!bleAdvertising) startProvisioningBLE();
    return;
  }

  // La vinculacion confirmada es la autoridad para apagar BLE. Si se habilito
  // manualmente con BOOT o por un comando, se mantiene encendido hasta que el
  // backend confirme nuevamente disable_ble.
  if (!bleLocked) {
    if (!bleAdvertising) startProvisioningBLE();
    return;
  }

  stopProvisioningBLE();
}

void checkConfigurationButton() {
  const bool pressed = digitalRead(CONFIG_BUTTON_PIN) == LOW;

  if (pressed && !configButtonPressed) {
    configButtonPressed = true;
    configButtonHandled = false;
    configButtonStartedAt = millis();
  }

  if (pressed && !configButtonHandled && millis() - configButtonStartedAt >= 5000UL) {
    configButtonHandled = true;
    saveBleLock(false);
    startProvisioningBLE();
    notifyStatus("ble_enabled");
    Serial.println("[BLE] Configuracion manual activada con BOOT");
  }

  if (!pressed) {
    configButtonPressed = false;
    configButtonHandled = false;
  }
}
} // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("===============================");
  Serial.println("        HIDROSMART ESP32");
  Serial.println("===============================");

  bootId = esp_random();
  hardwareId = getHardwareId();
  loadConfiguration();

  Serial.print("[DEVICE] Hardware ID: ");
  Serial.println(hardwareId);
  Serial.print("[DEVICE] Device code: ");
  Serial.println(deviceCode);

  pinMode(HIDROSMART_FLOW_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(HIDROSMART_FLOW_PIN), onPulse, FALLING);
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  mqttClient.setKeepAlive(HIDROSMART_MQTT_KEEPALIVE_SECONDS);
  mqttClient.setBufferSize(768);

  if (connectKnownWifi()) {
    if (!connectMqtt()) {
      Serial.println("[MQTT] Mosquitto no disponible; se activa Bluetooth para corregir la configuracion.");
    }
  } else {
    Serial.println("[WIFI] No hay redes guardadas disponibles.");
  }

  applyBlePolicy();

  lastSampleAt = millis();
  lastTotalSaveAt = millis();
}

void loop() {
  const unsigned long now = millis();

  checkConfigurationButton();

  String blePayload;
  if (takePendingBlePayload(blePayload)) {
    handleProvisioningPayload(blePayload);
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiAttemptAt >= WIFI_RETRY_INTERVAL_MS) {
      lastWifiAttemptAt = now;
      connectKnownWifi();
    }
  } else {
    if (!mqttClient.connected() && now - lastMqttAttemptAt >= MQTT_RETRY_INTERVAL_MS) {
      lastMqttAttemptAt = now;
      connectMqtt();
    }
    mqttClient.loop();
  }

  if (now - lastSampleAt < HIDROSMART_SAMPLE_INTERVAL_MS) {
    delay(10);
    return;
  }

  const float intervalSeconds = (now - lastSampleAt) / 1000.0f;
  lastSampleAt = now;

  noInterrupts();
  const uint32_t pulses = pulseCount;
  pulseCount = 0;
  interrupts();

  // YF-S201: caudal (L/min) = pulsos/segundo / 7.5.
  const float pulsesPerSecond = pulses / intervalSeconds;
  const float flowRateLpm = pulsesPerSecond / HIDROSMART_FLOW_FACTOR;
  const float consumptionLiters = flowRateLpm * intervalSeconds / 60.0f;
  totalLiters += consumptionLiters;
  pendingPulses += pulses;
  pendingConsumptionLiters += consumptionLiters;
  pendingSampleSeconds += intervalSeconds;

  if ((now - lastTotalSaveAt >= TOTAL_SAVE_INTERVAL_MS) ||
      (totalLiters - lastPersistedTotalLiters >= TOTAL_SAVE_MIN_DELTA_LITERS)) {
    saveTotalLiters();
  }

  Serial.print("Pulsos: ");
  Serial.print(pulses);
  Serial.print(" | Caudal: ");
  Serial.print(flowRateLpm, 3);
  Serial.print(" L/min | Consumo intervalo: ");
  Serial.print(consumptionLiters, 5);
  Serial.print(" L | Total: ");
  Serial.print(totalLiters, 5);
  Serial.println(" L");

  // La telemetria conserva el periodo de 5 segundos aunque el navegador siga
  // conectado por BLE. BLE y MQTT pueden trabajar en paralelo; pausar MQTT
  // aqui dejaba lecturas acumuladas y rompia la sincronizacion con el frontend.
  if (mqttClient.connected() && pendingSampleSeconds > 0.0f) {
    const float pendingFlowRateLpm = pendingConsumptionLiters / (pendingSampleSeconds / 60.0f);
    if (publishTelemetry(pendingPulses, pendingFlowRateLpm, pendingConsumptionLiters, pendingSampleSeconds)) {
      pendingPulses = 0;
      pendingConsumptionLiters = 0.0f;
      pendingSampleSeconds = 0.0f;
    }
  }
}
