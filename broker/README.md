# Bróker MQTT

La forma más simple de desplegar el bróker es la plantilla **Mosquitto Broker** de Railway, con las variables `MOSQUITTO_USERNAME` y `MOSQUITTO_PASSWORD` ([guía de despliegue](../docs/despliegue-railway.md)).

Esta carpeta contiene una configuración más estricta, para quien quiera reemplazar la de la plantilla:

| Archivo | Contenido |
|---|---|
| `mosquitto.conf` | Sin acceso anónimo, archivo de claves, lista de control de acceso, persistencia, `max_keepalive 120`, `max_connections 10`, y un oyente TLS opcional en el puerto 8883 |
| `acl` | Permisos por tópico para el usuario del medidor y del backend |
| `certs/generar_certificados.sh` | Genera una autoridad certificadora propia y el certificado del servidor para TLS |

## Uso con Railway

1. Monte un volumen en el servicio del bróker en `/mosquitto/config`.
2. Copie `mosquitto.conf` y `acl` al volumen con la CLI de Railway:

   ```bash
   railway run --service "Mosquitto Broker" cp mosquitto.conf /mosquitto/config/mosquitto.conf
   railway run --service "Mosquitto Broker" cp acl /mosquitto/config/acl
   ```

3. Cree el archivo de claves con `mosquitto_passwd -c /mosquitto/config/passwd <usuario>`.
4. Defina el comando de inicio `mosquitto -c /mosquitto/config/mosquitto.conf` y vuelva a desplegar el servicio.

Si no va a usar TLS, elimine del archivo de configuración el bloque del oyente 8883. Para usarlo, genere los certificados, cópielos a `/mosquitto/config/certs/`, publique el puerto 8883 con un proxy TCP y active `MQTT_USE_TLS` en el backend y `METER_MQTT_USE_TLS` en el firmware. El SIM7080G admite TLS 1.2, no 1.3.

Las credenciales y los certificados no deben subirse al repositorio; `.gitignore` los excluye.
