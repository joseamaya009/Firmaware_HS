# Pruebas de firmware

Antes de cargar una placa, compila:

```powershell
Set-Location firmware
pio run
```

La validación funcional requiere hardware: confirma el anuncio BLE, guarda
Wi-Fi, conexión MQTT, tres lecturas de cinco segundos y reactivación de BLE
después de `unlink`. El procedimiento completo está en
[../../docs/iot.md](../../docs/iot.md).
