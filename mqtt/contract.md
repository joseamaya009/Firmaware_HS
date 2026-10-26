# Contrato MQTT del ESP32

## Topics

```text
hidrosmart/devices/{deviceCode}/telemetry
hidrosmart/devices/{deviceCode}/status
hidrosmart/devices/{deviceCode}/commands
```

`deviceCode` es el código lógico de la placa. `hardwareId` continúa siendo la
identidad física inmutable y viaja en cada estado y lectura.

## Telemetría

Publicada cada cinco segundos por defecto en `telemetry`:

```json
{
  "mqttMessageId": "HS-7A3F91C8D2E4-123456-42",
  "deviceId": "HS-7A3F91C8D2E4",
  "hardwareId": "HS-141F47470968",
  "timestamp": "2026-10-11T12:00:00Z",
  "flowRateLpm": 2.4,
  "consumptionLiters": 0.2,
  "totalLiters": 3.4,
  "pulses": 18,
  "sampleIntervalSeconds": 5,
  "wifiRssiDbm": -56,
  "signalQuality": -56
}
```

No cambies `mqttMessageId` al reintentar exactamente la misma lectura. El
backend lo usa para evitar duplicados.

El cálculo predeterminado del YF-S201 es:

```text
flowRateLpm = (pulsos / segundos) / 7.5
consumptionLiters = flowRateLpm × segundos / 60
```

## Estado

El estado MQTT es retenido y contiene, entre otros, `deviceId`, `hardwareId`,
`status`, `provisioningState`, `timestamp`, `firmwareVersion`, `lastIp`,
`linkedToHome`, `bleLocked`, `ssid` y `wifiRssiDbm`.

El campo `status` utiliza `ONLINE` u `OFFLINE`. `provisioningState` expresa
`PENDING`, `BLE_READY`, `WIFI_CONNECTED`, `MQTT_CONNECTED` o `COMPLETE`.

## Comandos

El backend publica JSON en `commands`:

```json
{ "command": "confirm_link" }
```

| Comando | Efecto |
| --- | --- |
| `confirm_link` | Marca hogar asociado y detiene BLE. |
| `unlink` | Elimina asociación, conserva Wi-Fi y vuelve a anunciar BLE. |
| `enable_ble` | Habilita BLE para configuración. |
| `disable_ble` | Solo se acepta si la placa ya está asociada. |

## BLE

El servicio BLE usa `4fafc201-1fb5-459e-8fcc-c5c9c331914b`; la característica
de escritura de configuración es `beb5483e-36e1-4688-b7f5-ea07361b26a8` y la
de lectura/notificación de estado es `beb5483e-36e1-4688-b7f5-ea07361b26a9`.

La configuración BLE admite `ssid`, `password`, `mqttHost`, `mqttPort` y
`deviceCode`. No incluye ni publica contraseñas MQTT; esas se compilan en
`config.h` cuando el broker las requiere.
