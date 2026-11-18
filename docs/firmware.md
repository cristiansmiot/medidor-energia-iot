# Firmware del medidor

El firmware está escrito en C sobre ESP-IDF 5 con FreeRTOS y se compila con PlatformIO. Corre en un ESP32-S3 con 8 MB de flash.

## Conexiones

| Periférico | Interfaz | Pines del ESP32-S3 |
|---|---|---|
| ADE9153A (medición) | SPI2, 1 MHz | MOSI 11, SCLK 12, MISO 13, CS 10, RESET 14, IRQ 47 |
| microSD | SPI2 compartido con el ADE9153A, 4 MHz | MOSI 11, SCLK 12, MISO 13, CS 7 |
| SIM7080G (LTE Cat-M1) | UART1, 57 600 baudios | TX 16, RX 17, PWRKEY 4 |
| Pantalla OLED 128 × 64 (0x3C) | I2C, 100 kHz | SDA 8, SCL 9 |
| Reloj DS3231 (0x68) | I2C compartido con la pantalla | SDA 8, SCL 9 |
| Teclado de membrana 1 × 4 | GPIO con pull-up interno | 2, 1, 41, 42 |
| LED de estado, de red y de error | GPIO | 38, 15, 48 |

La microSD comparte el bus SPI con el ADE9153A. La rama MISO de la tarjeta debe quedar en alta impedancia cuando su CS está en alto; por eso la tarjeta lleva un buffer de tres estados. Los pines están en `config/board_config.h`.

## Configuración

1. Copie `config/secrets.example.h` como `config/secrets.h` y complete el dominio y el puerto del bróker y sus credenciales. Ese archivo no se versiona.
2. Los parámetros de operación están en `config/meter_config.h`: banda de tensión, períodos de publicación, escalera de recuperación celular, APN y tamaños de las colas.

## Compilación y carga

Con [PlatformIO](https://platformio.org) instalado:

```bash
cd firmware
pio run -e medidor                 # compila
pio run -e medidor -t upload       # carga por USB
pio device monitor                 # consola serie a 115 200 baudios
```

| Perfil | Uso |
|---|---|
| `medidor` | Operación normal: una lectura por minuto, publicación con QoS 1 y `/estado` cada 30 min |
| `metrologia` | Comunicación apagada y una muestra cada 5 s en la microSD, para caracterizar la medición en banco sin consumir datos |
| `test_*` | Pruebas individuales de la pantalla, el reloj, la microSD y el módem |

## Funcionamiento

El firmware se organiza en tareas de FreeRTOS con prioridades separadas: medición, calidad de tensión, almacenamiento, comunicaciones, interfaz y supervisión. Un supervisor reinicia el equipo si una tarea deja de reportar.

- **Medición.** Lee el ADE9153A una vez por segundo, integra la energía en doble precisión y la guarda en memoria no volátil.
- **Tensión.** Compara la tensión eficaz con la banda de 108 a 132 V cada 250 ms y detecta la pérdida de la red eléctrica.
- **Almacenamiento.** Cada lectura se escribe en la microSD con su identificador antes de publicarse. Un cursor en memoria no volátil marca la última lectura confirmada por el bróker; tras un corte, el reenvío continúa desde ese punto.
- **Comunicaciones.** El módem mantiene la sesión MQTT con su pila interna. Si el enlace falla, el firmware aplica una recuperación escalonada: reintento de la sesión MQTT, reactivación del contexto de datos, reinicio funcional del módem y, como último nivel, apagado y encendido del módem, con tiempos mínimos entre intentos para no gastar datos.
- **Hora.** Toma la hora de la red celular cuando el operador la entrega, del RTC al arrancar y de la orden `sync_time` de la plataforma.

## Archivos en la microSD

La tarjeta, en FAT32, guarda cada archivo en la carpeta `smartmeter/`:

| Archivo | Contenido |
|---|---|
| `telemetry_audit_v1.jsonl` | Cada lectura en SenML, tal como se publica |
| `history_v1.csv` | Las mismas lecturas en CSV, para revisarlas sin herramientas |
| `events_v1.jsonl` | Eventos del sistema: arranques, red celular, tensión, microSD |
| `alerts_v1.jsonl` | Alertas pendientes de publicar |

Después de copiar la tarjeta en un computador conviene renombrar la carpeta `smartmeter/`: de lo contrario, el medidor intentará reenviar lecturas que ya están en la plataforma. Use tarjetas de 16 GB de buena calidad; algunas tarjetas de 8 GB no aceptan escritura en este bus.
