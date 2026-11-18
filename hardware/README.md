# Hardware

Esta carpeta contendrá el diseño de la tarjeta del medidor:

- `kicad/`: proyecto de KiCad (esquemático y PCB).
- `esquematico.pdf` y `pcb.pdf`: versiones para consulta.
- `bom.csv`: lista de materiales con referencia, valor, encapsulado y fabricante.

## Bloques del prototipo

| Bloque | Componente | Función |
|---|---|---|
| Medición | Analog Devices ADE9153A (tarjeta EV-ADE9153ASHIELDZ) | Valores eficaces, potencias, energía y frecuencia, con autocalibración mSure |
| Procesamiento | Espressif ESP32-S3 (8 MB de flash) | Firmware, almacenamiento y control del módem |
| Comunicación | SIMCom SIM7080G (HAT Waveshare) | LTE Cat-M1 y NB-IoT, pila MQTT interna |
| Almacenamiento | microSD de 16 GB por SPI, con buffer de tres estados en MISO | Registro local de lecturas y eventos |
| Hora | Maxim DS3231 | Reloj de tiempo real con batería |
| Interfaz | Pantalla OLED 128 × 64 I2C y teclado de membrana 1 × 4 | Consulta en sitio |
| Alimentación | Fuente HLK de 5 V y UPS con dos celdas 18650 | Operación con y sin red eléctrica |

Las conexiones con el ESP32-S3 están en [docs/firmware.md](../docs/firmware.md).

## Precauciones

El frente de medida está conectado a la red eléctrica. No conecte el puerto USB del ESP32-S3 a un computador mientras el medidor esté energizado sin un aislador USB, y trabaje con una fuente de CA aislada o un transformador de aislamiento.
