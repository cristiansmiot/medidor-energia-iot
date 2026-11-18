#ifndef METER_SECRETS_H
#define METER_SECRETS_H

// Copie este archivo como config/secrets.h y complete los valores de su
// despliegue. config/secrets.h esta en .gitignore y no debe subirse al
// repositorio.

// Broker MQTT: en Railway use el dominio y el puerto del proxy TCP del servicio
// Mosquitto (Settings > Networking > TCP Proxy), no el puerto interno 1883.
#define METER_MQTT_HOST      "su-broker.proxy.rlwy.net"
#define METER_MQTT_PORT      12345U
#define METER_MQTT_USERNAME  "medidor"
#define METER_MQTT_PASSWORD  "cambie-esta-clave"

// APN del operador de la SIM (Movistar Colombia por defecto en meter_config.h).
// #define METER_SIM7080G_APN "internet.movistar.com.co"

#endif
