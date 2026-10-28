# Firmware ESP32

Firmware Arduino/PlatformIO del sensor HidroSmart. Lee un caudalímetro,
conserva redes Wi-Fi en NVS, se aprovisiona por BLE y publica telemetría MQTT.

## Preparación

1. Copia `provisioning/config.example.h` como `provisioning/config.h`.
2. Define Wi-Fi, host MQTT de red estable, código lógico y parámetros del
   sensor. `config.h` está ignorado por Git.
3. Instala PlatformIO y ejecuta desde la raíz de este repositorio:

```powershell
pio run
pio run -t upload --upload-port COM6
pio device monitor --baud 115200
```

Si `COM6` está ocupado, cierra el monitor serie, Arduino IDE o cualquier
programa que esté usando el puerto y vuelve a cargar.

## Comportamiento BLE

- La placa obtiene un `hardwareId` estable desde el eFuse.
- Anuncia `HidroSmart-XXXXXX` por BLE al arrancar, incluso con Wi-Fi guardado
  o después de asociarse a un hogar.
- Cuando recibe Wi-Fi por BLE, guarda la red en NVS y trata de conectar MQTT.
- `confirm_link` marca la asociación, mantiene BLE activo y conserva la red.
- `unlink` conserva Wi-Fi, borra solo la asociación y reactiva BLE.
- `disable_ble` lo desactiva temporalmente; al reiniciar vuelve a anunciarse.
- Mantener BOOT aproximadamente cinco segundos también habilita BLE.

El servicio BLE no usa pairing ni PIN. Cualquier equipo cercano puede enviar
una nueva configuración Wi-Fi; mantén el acceso físico a la placa bajo control.

## Telemetría

El intervalo predeterminado es cinco segundos (`HIDROSMART_SAMPLE_INTERVAL_MS`).
Cada mensaje contiene un `mqttMessageId` único por arranque y secuencia para
permitir deduplicación en el backend. Consulta el
[contrato MQTT](mqtt/contract.md) y el [flujo IoT general](../docs/iot.md).

## Configuración MQTT

La configuración del `.env` y el envío del host por BLE están en
[`docs/mqtt-esp32-red-local.md`](../docs/mqtt-esp32-red-local.md); el ciclo de
vinculación está en [`docs/dispositivos-iot.md`](../docs/dispositivos-iot.md).

El host debe ser visible desde la Wi-Fi del ESP32; no uses `localhost` ni
`mosquitto`, pues solo funcionan dentro del equipo o la red Docker. Para la red
local usa un nombre DNS/mDNS estable o una reserva DHCP gestionada.

El broker integrado sirve solo para desarrollo sin TLS. Las macros
`HIDROSMART_MQTT_USERNAME` y `HIDROSMART_MQTT_PASSWORD` permiten que el
firmware use un broker autenticado cuando se configure infraestructura segura.

## Hardware de referencia

- Placa: ESP32 Dev Module.
- Caudalímetro: YF-S201.
- Pin de pulsos predeterminado: GPIO 27.
- Factor de flujo predeterminado: `7.5`.

Calibra el factor con una medición real antes de usar datos de consumo para
decisiones o cobro.
