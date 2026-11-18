# Plataforma web

## Roles

| Rol | Puede |
|---|---|
| `super_admin` | Todo, incluida la gestión de usuarios y la asignación de medidores |
| `operador` | Consultar todos los medidores, editarlos, enviar órdenes y revisar la auditoría |
| `visualizador` | Consultar solo los medidores que se le asignen |

Las sesiones usan tokens JWT con una vigencia de 24 h.

## Pantallas

| Pantalla | Contenido |
|---|---|
| Resumen | Medidores conectados y desconectados, consumo por hora y alarmas activas |
| Medidores | Lista con estado de conexión, última lectura y señal celular |
| Detalle del medidor | Pestañas de resumen, variables eléctricas, histórico, eventos y comandos |
| Eventos | Alarmas de tensión, desconexiones y fallas reportadas por los medidores |
| Auditoría | Órdenes enviadas y cambios de configuración, con usuario y hora |
| Usuarios | Alta, roles y asignación de medidores (solo `super_admin`) |

En la pestaña **Histórico** se elige una ventana reciente (de 24 h a un año) o un rango de fechas, hasta 31 días. Si el rango tiene más lecturas de las que se pueden graficar, el backend las promedia en el intervalo más corto que cabe. Desde la misma pestaña se exportan las lecturas a CSV.

## Alarmas de tensión

El backend abre una alarma cuando dos lecturas consecutivas, separadas al menos 60 s, quedan fuera de la banda de 108 a 132 V (120 V ±10 %), y la cierra con la primera lectura que vuelve a la banda. El medidor también publica una alerta inmediata por `/alerta` cuando detecta el evento.

## API

La API REST queda en `/api/v1` y su documentación interactiva en `/docs`.

| Ruta | Uso |
|---|---|
| `POST /auth/login` | Inicio de sesión |
| `GET /dispositivos/` | Lista de medidores |
| `GET /mediciones/{id}/ultima` | Última lectura |
| `GET /mediciones/{id}/historico?horas=24` | Serie de una ventana reciente |
| `GET /mediciones/{id}/historico?desde=…&hasta=…` | Serie de un rango de fechas (UTC) |
| `GET /mediciones/{id}/exportar?desde=…&hasta=…` | Lecturas en CSV (requiere sesión) |
| `GET /salud/{id}/metricas` | Entrega, latencia y señal del medidor |
| `GET /eventos/` | Alarmas y eventos |
| `POST /dispositivos/{id}/comando` | Envío de una orden al medidor |
| `GET /health` | Estado del servicio, la base de datos y el cliente MQTT |

Las rutas de consulta de lecturas responden sin sesión en esta versión. Antes de un uso con datos de personas, conviene exigir sesión en todas ellas.
