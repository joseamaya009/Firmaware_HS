#pragma once

#define HIDROSMART_WIFI_SSID "CAMBIA_WIFI_SSID"
#define HIDROSMART_WIFI_PASSWORD "CAMBIA_WIFI_PASSWORD"
// Nombre DNS estable de la red local. Debe resolver hacia el PC/servidor que
// publica Mosquitto; no uses "mosquitto", que solo existe dentro de Docker.
#define HIDROSMART_MQTT_HOST "hidrosmart-broker.local"
#define HIDROSMART_MQTT_PORT 1883
// Vacio: usa el hardwareId HS-XXXXXX generado por el ESP32.
// Para una placa preasignada, usa UN codigo por ESP32, por ejemplo
// "HS-7A3F91C8D2E4", "HS-B6E04D79A153" o "HS-2C8A5F01E7B9".
// No reutilices el mismo codigo en dos placas.
#define HIDROSMART_DEVICE_CODE ""
// En el perfil MQTT seguro, el usuario debe coincidir con HIDROSMART_DEVICE_CODE
// y la clave debe ser unica para esta placa. Nunca confirmes config.h en Git.
#define HIDROSMART_MQTT_USERNAME ""
#define HIDROSMART_MQTT_PASSWORD ""
#define HIDROSMART_FIRMWARE_VERSION "1.1.0"
#define HIDROSMART_FLOW_PIN 27
#define HIDROSMART_FLOW_FACTOR 7.5f
#define HIDROSMART_SAMPLE_INTERVAL_MS 5000UL
#define HIDROSMART_MQTT_KEEPALIVE_SECONDS 30
