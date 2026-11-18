# Mensajes MQTT

## Tópicos

Cada medidor se identifica con `SM-` seguido de la dirección MAC del ESP32-S3, por ejemplo `SM-A1B2C3D4E5F6`. Ese identificador no cambia si se reemplaza el módem o la SIM.

| Tópico | Sentido | Contenido | QoS |
|---|---|---|---|
| `medidor/{id}/datos` | Medidor → plataforma | Una lectura eléctrica por minuto | 1 |
| `medidor/{id}/estado` | Medidor → plataforma | Salud del nodo y de la conexión, cada 30 min y al conectar (retenido) | 1 |
| `medidor/{id}/alerta` | Medidor → plataforma | Eventos de tensión, pérdida de red eléctrica y fallas de periféricos | 1 |
| `medidor/{id}/cmd` | Plataforma → medidor | Órdenes remotas | 1 |

El intervalo de *keepalive* es de 120 s, el máximo que acepta el bróker.

## Lectura (`/datos`)

Los mensajes usan la estructura de SenML (RFC 8428) en JSON. El primer elemento lleva el nombre base y el tiempo base; los demás, cada medida con su unidad.

```json
[{"bn":"urn:dev:serial:SM-A1B2C3D4E5F6:/","bt":1794024109.000},
 {"n":"boot_id","v":2429797755},{"n":"seq","v":127},
 {"n":"ts_device","v":1794024109},{"n":"time_valid","vb":true},
 {"n":"uptime_ms","v":7620011},{"n":"fw","vs":"1.1.0-obj4-q1-rc6.4"},
 {"n":"cal","v":16},{"n":"vrms","u":"V","v":120.012},
 {"n":"irms","u":"A","v":0.8135},{"n":"p_act","u":"W","v":97.402},
 {"n":"p_react","u":"VAR","v":-4.508},{"n":"p_ap","u":"VA","v":97.640},
 {"n":"pf","u":"/","v":0.9976},{"n":"freq","u":"Hz","v":60.000},
 {"n":"e_act","u":"Wh","v":5192.4981},{"n":"temp","u":"Cel","v":25.63},
 {"n":"temp_ok","vb":true},{"n":"pq","u":"count","v":0}]
```

| Campo | Unidad | Descripción |
|---|---|---|
| `bn` | | Nombre base con el identificador del medidor |
| `bt` | s | Hora UTC en que se generó la lectura (época Unix) |
| `boot_id` | | Número aleatorio generado en cada arranque |
| `seq` | | Consecutivo de la lectura dentro del arranque |
| `ts_device` | s | Hora del reloj del medidor al generar la lectura |
| `time_valid` | | Verdadero si la hora proviene de una fuente sincronizada |
| `uptime_ms` | ms | Tiempo desde el arranque |
| `fw` | | Versión del firmware |
| `cal` | | Registro de estado de calibración del ADE9153A |
| `vrms` | V | Tensión eficaz |
| `irms` | A | Corriente eficaz |
| `p_act` | W | Potencia activa |
| `p_react` | var | Potencia reactiva (con signo) |
| `p_ap` | VA | Potencia aparente |
| `pf` | | Factor de potencia |
| `freq` | Hz | Frecuencia |
| `e_act` | Wh | Energía activa acumulada, conservada tras un apagado |
| `temp` | °C | Temperatura interna del ADE9153A |
| `temp_ok` | | Verdadero si la lectura de temperatura es válida |
| `pq` | | Indicador de tensión fuera de banda en el momento de la lectura |

El par (`boot_id`, `seq`) identifica cada lectura. El backend descarta las repetidas y así distingue una lectura perdida de una retrasada o duplicada. Las lecturas que no se pudieron publicar quedan en la microSD y se envían, en orden, cuando vuelve el enlace.

Las unidades `VAR` y `Wh` no son unidades primarias del registro de SenML; la primera debería escribirse `var` y la segunda está registrada solo como unidad secundaria (RFC 8798). Corregirlas no cambia los valores.

## Órdenes (`/cmd`)

La aplicación web envía las órdenes a través del backend, que registra quién y cuándo las envió.

| Orden en la aplicación | Texto publicado | Efecto en el medidor |
|---|---|---|
| Sincronizar hora | `sync_time:<época UTC>` | Ajusta el reloj del medidor y del RTC |
| Solicitar estado | `status` | Publica `/estado` de inmediato |
| Reiniciar | `reset` | Reinicia el ESP32-S3 conservando la energía acumulada |
| Corte de suministro | `relay_off` | Abre el relé de corte |
| Restaurar suministro | `relay_on` | Cierra el relé de corte |

El firmware atiende el corte y la restauración, pero el relé no está cableado en el prototipo.
