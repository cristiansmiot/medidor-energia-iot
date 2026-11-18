# Despliegue de la plataforma en Railway

La plataforma usa cuatro servicios en un mismo proyecto de [Railway](https://railway.com): una base de datos PostgreSQL, el bróker Mosquitto, el backend y la aplicación web. Esta guía los crea en ese orden, porque cada servicio necesita datos del anterior.

## Antes de empezar

- Una cuenta de Railway con un plan que permita cuatro servicios activos.
- Una copia de este repositorio en su cuenta de GitHub (botón *Fork*), conectada a Railway desde *Account Settings > Integrations*.
- Una SIM con plan de datos y cobertura LTE-M para el medidor. Con una lectura por minuto el medidor transmite unos 2,4 MB por día, alrededor de 73 MB al mes, sobre todo por el tráfico de control de TCP y MQTT.

## 1. Proyecto y base de datos

1. En Railway, cree un proyecto vacío (*New Project > Empty Project*).
2. Agregue la base de datos con *Create > Database > PostgreSQL*. Railway genera las credenciales y expone la variable `DATABASE_URL` del servicio.

## 2. Bróker MQTT

1. Agregue el bróker desde la plantilla de Mosquitto: *Create > Template* y busque **Mosquitto Broker** (repositorio `Lima-e-Silva/mosquitto_railway_template`).
2. Defina las variables que pide la plantilla:

   | Variable | Valor |
   |---|---|
   | `MOSQUITTO_USERNAME` | Usuario que usarán el medidor y el backend |
   | `MOSQUITTO_PASSWORD` | Una clave larga y aleatoria |

3. Publique el puerto 1883 con un proxy TCP: *Settings > Networking > TCP Proxy*, puerto interno `1883`. Railway asigna un dominio y un puerto públicos, por ejemplo `su-broker.proxy.rlwy.net:12345`. Anótelos: los usan el firmware y el backend.

La carpeta [`broker/`](../broker) contiene una configuración más estricta que la de la plantilla, con lista de control de acceso por tópico, `max_keepalive 120` y persistencia. Su uso es opcional y se describe en [broker/README.md](../broker/README.md).

## 3. Backend

1. Agregue el servicio con *Create > GitHub Repo* y elija su copia del repositorio.
2. En *Settings > Source*, defina **Root Directory** = `backend`. Railway lee `backend/railway.toml`: construye con Nixpacks, arranca con `uvicorn app.main:app` y usa `/health` como verificación de salud.
3. En *Variables*, defina:

   | Variable | Valor | Nota |
   |---|---|---|
   | `DATABASE_URL` | `${{Postgres.DATABASE_URL}}` | Variable de referencia al servicio PostgreSQL |
   | `ENVIRONMENT` | `production` | |
   | `DEBUG` | `false` | |
   | `SECRET_KEY` | Cadena aleatoria de 48 caracteres o más | Firma los tokens de sesión |
   | `MQTT_BROKER` | Dominio del proxy TCP del bróker | Paso 2.3 |
   | `MQTT_PORT` | Puerto del proxy TCP | Paso 2.3 |
   | `MQTT_USERNAME` / `MQTT_PASSWORD` | Las del bróker | |
   | `MQTT_USE_TLS` | `false` | `true` solo si configuró TLS en el bróker |
   | `MQTT_TOPIC_BASE` | `medidor/#` | |
   | `ADMIN_EMAIL` / `ADMIN_PASSWORD` | Correo y clave del primer administrador | Se usan solo si la base está vacía |
   | `CORS_ORIGINS` | Dominio de la aplicación web | Se completa en el paso 4.4 |
   | `OFFLINE_UMBRAL_MIN` | `15` | Minutos sin mensajes para marcar el medidor desconectado |
   | `VOLTAJE_NOMINAL`, `VOLTAJE_TOLERANCIA_PCT`, `VOLTAJE_PERSISTENCIA_S` | `120`, `10`, `60` | Banda de 108 a 132 V y persistencia de 1 min |

4. Genere el dominio público en *Settings > Networking > Generate Domain*.
5. Abra `https://<dominio-del-backend>/health`. La respuesta debe indicar la base PostgreSQL y el cliente MQTT conectado. Si indica SQLite, la variable `DATABASE_URL` no quedó bien referenciada.

La documentación interactiva de la API queda en `https://<dominio-del-backend>/docs`.

## 4. Aplicación web

1. Agregue otro servicio con *Create > GitHub Repo* y el mismo repositorio.
2. Defina **Root Directory** = `frontend`. Railway lee `frontend/railway.toml`: ejecuta `npm run build` y sirve la carpeta `dist`.
3. Defina la variable `VITE_API_URL` = `https://<dominio-del-backend>`, sin barra final. Vite la incorpora durante la construcción, de modo que cualquier cambio exige volver a desplegar el servicio.
4. Genere el dominio público y agréguelo a `CORS_ORIGINS` en el backend.

## 5. Primer ingreso

1. Ingrese a la aplicación web con `ADMIN_EMAIL` y `ADMIN_PASSWORD`. Si no definió la clave, el backend generó una temporal y la escribió en su registro (*Deployments > View Logs*).
2. Cambie la clave en *Perfil*.
3. Cree los usuarios en *Usuarios* con el rol que corresponda: `operador` puede enviar comandos; `visualizador` solo consulta los medidores que se le asignen.

## 6. Medidor

1. Copie `firmware/config/secrets.example.h` como `firmware/config/secrets.h` y escriba el dominio y el puerto del proxy TCP, y el usuario y la clave del bróker.
2. Compile y cargue el perfil `medidor` ([firmware.md](firmware.md)).
3. Al conectarse, el medidor publica su estado y el backend lo registra automáticamente con el identificador `SM-` seguido de la dirección MAC del ESP32-S3. Desde *Medidores* se le puede cambiar el nombre y la ubicación.
4. En el detalle del medidor deben aparecer las lecturas cada minuto en *Variables* e *Histórico*.

## Actualizaciones

Cada cambio en la rama principal vuelve a desplegar el backend o la aplicación web, según la carpeta que cambie (`backend/` o `frontend/`). El bróker y la base de datos no se ven afectados.

## Problemas frecuentes

| Síntoma | Causa probable |
|---|---|
| El medidor registra la red pero no publica | Dominio o puerto del proxy TCP equivocados en `secrets.h`, o usuario y clave distintos de los del bróker |
| La aplicación web muestra errores de red | `VITE_API_URL` mal escrita, o el dominio de la aplicación no está en `CORS_ORIGINS` |
| El medidor aparece desconectado aunque publica | El reloj del medidor no está sincronizado; use la orden *Sincronizar hora* |
| `/health` indica SQLite | `DATABASE_URL` no está definida como variable de referencia |
