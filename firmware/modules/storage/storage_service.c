#include "storage_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "data_serializer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger.h"
#include "meter_config.h"
#include "meter_data.h"
#include "mqtt_topics.h"
#include "node_health.h"
#include "rtos_app_config.h"
#include "sd_storage.h"
#include "system_health.h"

typedef enum {
    STORAGE_REQ_TELEMETRY = 0,
    STORAGE_REQ_ALERT,
    STORAGE_REQ_EVENT,
    STORAGE_REQ_ACK_TELEMETRY,
    STORAGE_REQ_ACK_ALERT,
} storage_request_type_t;

typedef struct {
    storage_request_type_t type;
    char line[STORAGE_SERVICE_PAYLOAD_MAX];
} storage_request_t;

#define STORAGE_NORMAL_QUEUE_LEN 10U
#define STORAGE_HIGH_QUEUE_LEN    8U
#define STORAGE_STACK_LOG_PERIOD_MS (5LL * 60LL * 1000LL)

static const char *TAG = "storage_service";
static TaskHandle_t s_task_handle;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[TASK_STACK_STORAGE];
static StaticQueue_t s_normal_queue_buffer;
static StaticQueue_t s_high_queue_buffer;
static uint8_t s_normal_queue_storage[STORAGE_NORMAL_QUEUE_LEN * sizeof(storage_request_t)];
static uint8_t s_high_queue_storage[STORAGE_HIGH_QUEUE_LEN * sizeof(storage_request_t)];
static QueueHandle_t s_normal_queue;
static QueueHandle_t s_high_queue;
static StaticSemaphore_t s_snapshot_mutex_buffer;
static SemaphoreHandle_t s_snapshot_mutex;
static storage_service_snapshot_t s_snapshot;
static char s_telemetry_head[STORAGE_SERVICE_PAYLOAD_MAX];
static char s_alert_head[STORAGE_SERVICE_PAYLOAD_MAX];
/*
 * The storage service is single-threaded.  Keep its large formatting buffers
 * in static storage instead of placing 3.5-5.5 KiB on task_storage's stack.
 * This also leaves a measurable margin for FATFS/newlib call frames.
 */
static char s_line_scratch[STORAGE_SERVICE_LINE_MAX];
static char s_payload_scratch[STORAGE_SERVICE_PAYLOAD_MAX];
static bool s_telemetry_claimed;
static bool s_alert_claimed;
static bool s_telemetry_ack_queued;
static bool s_alert_ack_queued;
/* Errores de lectura consecutivos de la cabeza de cada cola (0 = telemetria,
 * 1 = alertas). Sobreviven al remontaje para acotar el ciclo de recuperacion. */
static uint8_t s_head_read_failures[2];
/* true desde que un montaje u operacion falla hasta el siguiente montaje
 * exitoso. Mientras tanto las alertas no se encolan para la SD (la tarea no
 * drena colas sin tarjeta) y el despachador usa su cola RAM de envio en vivo. */
static volatile bool s_sd_unavailable;
static uint32_t s_mount_failures;
static int64_t s_next_mount_ms;
static bool s_queue_overflow_reported;
static int64_t s_last_usage_ms;
static int64_t s_last_history_ms;
static int64_t s_last_stack_log_ms;

/*
 * rc5 (2026-11-07): entrega de telemetria por CURSOR sobre la auditoria.
 *
 * En E4-02 (q1-rc4) se perdieron 35 registros que estaban en la auditoria SD:
 * tras cada corte, el primer ACK que fallaba (cada ACK reescribia la cola
 * completa: copia a .tmp y dos renombres FAT) dejaba la cola sin el resto del
 * atraso. Los archivos de solo agregar (auditoria, historico) nunca perdieron
 * datos. Ahora la telemetria no tiene cola propia: cada registro se escribe una
 * sola vez en la auditoria y lo pendiente es todo lo que esta despues del
 * cursor, guardado en la NVS del ESP32 (no en la SD). Un ACK solo mueve el
 * cursor; si falla, el registro se reenvia (duplicado idempotente por
 * boot_id/seq), nunca se salta.
 */
typedef struct {
    uint64_t offset;   /* byte del siguiente registro por enviar */
    uint32_t boot_id;  /* ultimo registro confirmado (diagnostico) */
    uint32_t seq;
} storage_cursor_t;

#define STORAGE_CURSOR_NVS_NAMESPACE "sdq"
/* Cursor propio para perfiles sin MQTT: nunca mueven el cursor de la cola real
 * (ver METER_SD_TELEMETRY_AUDIT_FILE en meter_config.h). */
#if METER_COMM_ENABLE
#define STORAGE_CURSOR_NVS_KEY       "cur"
#else
#define STORAGE_CURSOR_NVS_KEY       "cur_loc"
#endif

static storage_cursor_t s_cursor;
static bool s_cursor_ready;
static bool s_cursor_nvs_ok;
static nvs_handle_t s_cursor_nvs;
static uint64_t s_head_next_offset;
static uint32_t s_head_boot;
static uint32_t s_head_seq;
static uint64_t s_audit_size;
static uint32_t s_audit_last_len;
static bool s_cursor_save_fail_reported;

/* Registros generados que aun no estan completos en la SD (auditoria y
 * luego historico). Si la tarjeta falla, esperan aqui y se escriben al
 * remontar, con su seq original. Con el anillo lleno se suspende la
 * generacion (no se consumen seq) y la ruta en vivo sin SD toma el relevo. */
#ifndef STORAGE_PENDING_SLOTS
#define STORAGE_PENDING_SLOTS 6U
#endif
#define STORAGE_HISTORY_ROW_MAX 384U
typedef struct {
    bool audit_done;
    char payload[STORAGE_SERVICE_PAYLOAD_MAX];
    char row[STORAGE_HISTORY_ROW_MAX];
} storage_pending_record_t;
static storage_pending_record_t s_pending[STORAGE_PENDING_SLOTS];
static uint8_t s_pending_first;
static uint8_t s_pending_count;

/*
 * rc6.3 (2026-11-10): en E4-03 el final de alerts_v1.jsonl quedo ilegible
 * (cluster FAT danado). Cada alerta nueva fallaba al guardarse -> remontaje
 * OK -> s_mount_failures volvia a 0 -> el siguiente fallo esperaba 1 s otra
 * vez: 322 remontajes en 7 min y la telemetria detenida hasta un chkdsk.
 *  - Un archivo de alertas o eventos que falla STORAGE_FILE_FAILURE_LIMIT
 *    veces seguidas (con remontajes en medio) se aparta como .badN; si ni
 *    eso se puede, se deja de usar hasta el proximo arranque. La telemetria
 *    (auditoria) y el historico no se tocan.
 *  - Varias recuperaciones seguidas alargan la espera aunque el montaje
 *    salga bien (STORAGE_RECOVERY_BURST_WINDOW_MS).
 *  - Errores sueltos de escritura (EMC al transmitir el modem: 0x108/0x107)
 *    se reintentan una vez antes de remontar; las lecturas, dos veces.
 */
#define STORAGE_FILE_FAILURE_LIMIT       3U
#define STORAGE_RECOVERY_BURST_WINDOW_MS 60000LL
#define STORAGE_WRITE_RETRY_DELAY_MS     30U
#define STORAGE_READ_RETRY2_DELAY_MS     100U
static uint8_t s_file_failures[2];  /* 0 = alertas, 1 = eventos */
static bool s_alert_file_disabled;
static bool s_event_file_disabled;
static int64_t s_last_recovery_ms;
static uint8_t s_recovery_burst;

static esp_err_t storage_refresh_telemetry_head(void);
static void storage_cursor_sync(void);

#if defined(STORAGE_SERVICE_TEST_HOOKS) && (STORAGE_SERVICE_TEST_HOOKS != 0)
static bool s_usage_injected;
static uint32_t s_injected_total_mb;
static uint32_t s_injected_free_mb;
#endif

static int64_t storage_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static void storage_lock(void)
{
    (void)xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
}

static void storage_unlock(void)
{
    (void)xSemaphoreGive(s_snapshot_mutex);
}

static void storage_set_mounted(bool mounted)
{
    storage_lock();
    s_snapshot.mounted = mounted;
    storage_unlock();
}

static void storage_update_pending_estimate_locked(void)
{
    /* Aproximado (tamano restante / largo del ultimo registro): solo
     * diagnostico; la entrega no depende de este numero. */
    uint32_t pending = s_pending_count;
    if (s_audit_size > s_cursor.offset) {
        const uint64_t rest = s_audit_size - s_cursor.offset;
        const uint64_t len = (s_audit_last_len > 0U) ? s_audit_last_len : 600U;
        pending += (uint32_t)((rest + len - 1U) / len);
    }
    s_snapshot.telemetry_pending = pending;
    s_snapshot.records_buffered = s_pending_count;
}

static const char *storage_path_for_kind(storage_record_kind_t kind)
{
    switch (kind) {
        case STORAGE_RECORD_HISTORY:   return METER_SD_HISTORY_FILE;
        case STORAGE_RECORD_TELEMETRY: return METER_SD_TELEMETRY_QUEUE_FILE;
        case STORAGE_RECORD_ALERT:     return METER_SD_ALERT_QUEUE_FILE;
        case STORAGE_RECORD_EVENT:     return METER_SD_EVENT_LOG_FILE;
        default:                       return NULL;
    }
}

static bool storage_line_valid(const char *line)
{
    if (line == NULL) {
        return false;
    }
    const size_t len = strnlen(line, STORAGE_SERVICE_PAYLOAD_MAX);
    return (len > 1U) && (len < STORAGE_SERVICE_PAYLOAD_MAX) &&
           (strchr(line, '\n') == NULL) && (strchr(line, '\r') == NULL);
}

static bool storage_json_complete(storage_request_type_t type, const char *line)
{
    if (!storage_line_valid(line)) {
        return false;
    }
    const size_t len = strlen(line);
    if (type == STORAGE_REQ_EVENT) {
        return (line[0] == '{') && (line[len - 1U] == '}');
    }
    if ((type == STORAGE_REQ_TELEMETRY) || (type == STORAGE_REQ_ALERT)) {
        return ((line[0] == '[') && (line[len - 1U] == ']')) ||
               ((line[0] == '{') && (line[len - 1U] == '}'));
    }
    return true;
}

static esp_err_t storage_enqueue(storage_request_type_t type,
                                 const char *line,
                                 bool high_priority)
{
    if ((s_normal_queue == NULL) || (s_high_queue == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    storage_request_t request = {.type = type};
    if (line != NULL) {
        if (!storage_json_complete(type, line)) {
            return ESP_ERR_INVALID_SIZE;
        }
        (void)snprintf(request.line, sizeof(request.line), "%s", line);
    }
    QueueHandle_t queue = high_priority ? s_high_queue : s_normal_queue;
    if (xQueueSend(queue, &request, 0) != pdTRUE) {
        storage_lock();
        ++s_snapshot.requests_dropped;
        storage_unlock();
        /* No generar otro evento cuando el registro que fallo ya era un
         * evento/alerta: evitar realimentar el despachador con la cola llena. */
        if ((type != STORAGE_REQ_EVENT) && (type != STORAGE_REQ_ALERT) &&
            !s_queue_overflow_reported) {
            s_queue_overflow_reported = true;
            (void)system_health_report_occurrence(
                SYSTEM_MODULE_SD, SYSTEM_EVENT_QUEUE_OVERFLOW,
                SYSTEM_SEVERITY_WARNING, ESP_ERR_NO_MEM,
                "storage request queue full");
        }
        return ESP_ERR_NO_MEM;
    }
    /* Una insercion posterior demuestra que volvio a existir capacidad. La
     * proxima saturacion podra generar una sola ocurrencia nueva. */
    s_queue_overflow_reported = false;
    return ESP_OK;
}

esp_err_t storage_service_submit_telemetry(const char *json_payload)
{
    return storage_enqueue(STORAGE_REQ_TELEMETRY, json_payload, false);
}

esp_err_t storage_service_submit_alert(const char *json_payload)
{
    /* Sin SD la alerta quedaria varada en la cola de solicitudes hasta un
     * remontaje que puede no llegar. El error hace que el despachador la
     * conserve en su cola RAM y la publique en vivo. rc6.3: igual si el
     * archivo de alertas quedo fuera de uso en este arranque. */
    if (s_sd_unavailable || s_alert_file_disabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return storage_enqueue(STORAGE_REQ_ALERT, json_payload, true);
}

esp_err_t storage_service_submit_event(const char *json_line)
{
    if (s_event_file_disabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return storage_enqueue(STORAGE_REQ_EVENT, json_line, false);
}

esp_err_t storage_service_ack_pending(storage_record_kind_t kind)
{
    if ((s_snapshot_mutex == NULL) ||
        ((kind != STORAGE_RECORD_TELEMETRY) && (kind != STORAGE_RECORD_ALERT))) {
        return ESP_ERR_INVALID_ARG;
    }
    bool *ack_queued = kind == STORAGE_RECORD_TELEMETRY
        ? &s_telemetry_ack_queued : &s_alert_ack_queued;
    storage_lock();
    const bool claimed = kind == STORAGE_RECORD_TELEMETRY
        ? s_telemetry_claimed : s_alert_claimed;
    if (*ack_queued || !claimed) {
        storage_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    *ack_queued = true;
    storage_unlock();
    const esp_err_t err = storage_enqueue(kind == STORAGE_RECORD_TELEMETRY
        ? STORAGE_REQ_ACK_TELEMETRY : STORAGE_REQ_ACK_ALERT, NULL,
        kind == STORAGE_RECORD_ALERT);
    if (err != ESP_OK) {
        storage_lock();
        *ack_queued = false;
        storage_unlock();
    }
    return err;
}

bool storage_service_live_fallback_needed(void)
{
    /* Un remontaje de ~1 s no justifica una medicion paralela con seq propia
     * (en E4-02 creo las seq 110 y 193, ausentes de la SD). Solo cuando la
     * tarjeta no acepta escrituras durante todo el anillo (6 periodos) se
     * publica en vivo. rc6: basta el anillo lleno, este o no montada; una
     * tarjeta que monta pero no escribe (Kingston SA08G, 2026-11-07) dejaba
     * el medidor sin generar ni publicar. */
    return s_pending_count >= STORAGE_PENDING_SLOTS;
}

esp_err_t storage_service_get_snapshot(storage_service_snapshot_t *out)
{
    if ((out == NULL) || (s_snapshot_mutex == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    storage_lock();
    *out = s_snapshot;
    out->requests_pending = (uint32_t)uxQueueMessagesWaiting(s_normal_queue) +
                            (uint32_t)uxQueueMessagesWaiting(s_high_queue);
    storage_unlock();
    return ESP_OK;
}

esp_err_t storage_service_peek_pending(storage_record_kind_t kind,
                                       char *out,
                                       size_t out_len)
{
    if ((out == NULL) || (out_len == 0U) || (s_snapshot_mutex == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *source = NULL;
    storage_lock();
    if ((kind == STORAGE_RECORD_TELEMETRY) && s_telemetry_claimed && !s_telemetry_ack_queued) {
        source = s_telemetry_head;
    } else if ((kind == STORAGE_RECORD_ALERT) && s_alert_claimed && !s_alert_ack_queued) {
        source = s_alert_head;
    }
    if ((source == NULL) || (source[0] == '\0')) {
        storage_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const size_t len = strlen(source);
    if (len >= out_len) {
        storage_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    (void)memcpy(out, source, len + 1U);
    storage_unlock();
    return ESP_OK;
}

static void storage_format_local(const system_record_base_t *base,
                                 char *out,
                                 size_t out_len)
{
    if ((base == NULL) || (out == NULL) || (out_len == 0U) ||
        !base->time_valid) {
        if ((out != NULL) && (out_len > 0U)) {
            out[0] = '\0';
        }
        return;
    }
    const time_t local_epoch = (time_t)base->timestamp_utc_s +
        ((time_t)SYSTEM_TIMEZONE_OFFSET_MIN * (time_t)60);
    struct tm local_tm = {0};
    (void)gmtime_r(&local_epoch, &local_tm);
    (void)strftime(out, out_len, "%Y-%m-%dT%H:%M:%S-05:00", &local_tm);
}

static esp_err_t storage_wrap_payload(system_record_type_t type,
                                      const char *type_name,
                                      const char *payload,
                                      char *out,
                                      size_t out_len)
{
    system_record_base_t base;
    esp_err_t err = system_record_base_create(type, &base);
    if (err != ESP_OK) {
        return err;
    }
    char local[40];
    storage_format_local(&base, local, sizeof(local));
    const int n = snprintf(
        out, out_len,
        "{\"v\":%u,\"device\":\"%s\",\"boot\":%lu,\"seq\":%lu,"
        "\"ts_utc\":%lld,\"local\":\"%s\",\"time_valid\":%s,"
        "\"uptime_ms\":%lld,\"type\":\"%s\",\"payload\":%s}",
        (unsigned)base.schema_version, mqtt_topics_device_id(),
        (unsigned long)base.boot_id, (unsigned long)base.sequence,
        (long long)base.timestamp_utc_s, local,
        base.time_valid ? "true" : "false", (long long)base.uptime_ms,
        type_name, payload);
    return ((n > 0) && (((size_t)n + 1U) < out_len))
               ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void storage_record_write_result(esp_err_t err)
{
    storage_lock();
    if (err == ESP_OK) {
        ++s_snapshot.writes_ok;
    } else {
        ++s_snapshot.writes_failed;
    }
    storage_unlock();
}

static void storage_schedule_recovery(esp_err_t err, const char *detail)
{
    (void)sd_storage_deinit();
    sd_storage_invalidate();
    storage_set_mounted(false);
    /* Antes de informar la falla: la propia alerta "SD ... failed" debe ir
     * por la cola RAM del despachador, no a una cola que ya no se drena. */
    s_sd_unavailable = true;
    ++s_mount_failures;
    uint32_t delay_ms = SYSTEM_RECOVERY_SLOW_PROBE_MS;
    if (s_mount_failures == 1U) {
        delay_ms = SYSTEM_RECOVERY_DELAY_1_MS;
    } else if (s_mount_failures == 2U) {
        delay_ms = SYSTEM_RECOVERY_DELAY_2_MS;
    } else if (s_mount_failures == 3U) {
        delay_ms = SYSTEM_RECOVERY_DELAY_3_MS;
    }
    /* rc6.3: rafagas de recuperaciones (montaje OK y fallo otra vez). */
    const int64_t now = storage_now_ms();
    if ((s_last_recovery_ms != 0) &&
        ((now - s_last_recovery_ms) < STORAGE_RECOVERY_BURST_WINDOW_MS)) {
        if (s_recovery_burst < UINT8_MAX) {
            ++s_recovery_burst;
        }
    } else {
        s_recovery_burst = 0U;
    }
    s_last_recovery_ms = now;
    uint32_t burst_delay_ms = 0U;
    if (s_recovery_burst >= 6U) {
        burst_delay_ms = SYSTEM_RECOVERY_SLOW_PROBE_MS;
    } else if (s_recovery_burst >= 4U) {
        burst_delay_ms = SYSTEM_RECOVERY_DELAY_3_MS;
    } else if (s_recovery_burst >= 2U) {
        burst_delay_ms = SYSTEM_RECOVERY_DELAY_2_MS;
    }
    if (burst_delay_ms > delay_ms) {
        delay_ms = burst_delay_ms;
    }
    s_next_mount_ms = now + (int64_t)delay_ms;
    (void)system_health_report_failure(
        SYSTEM_MODULE_SD, SYSTEM_EVENT_IO_ERROR, SYSTEM_SEVERITY_WARNING,
        err, detail);
}

/* rc6.3: aparta el archivo de alertas (alert=true) o de eventos que no se
 * puede usar. Primero intenta renombrarlo a .badN (no se borra nada); si la
 * entrada del directorio esta danada y tampoco se puede, deja de usarlo hasta
 * el proximo arranque: las alertas salen por la cola RAM en vivo y los
 * eventos siguen en el log del monitor serie. Una sola ocurrencia DATA_LOSS. */
static void storage_retire_file(bool alert, const char *why)
{
    const char *path = alert ? METER_SD_ALERT_QUEUE_FILE : METER_SD_EVENT_LOG_FILE;
    char name[64] = {0};
    const esp_err_t q = sd_storage_quarantine_file(path, name, sizeof(name));
    const bool moved = (q == ESP_OK) || (q == ESP_ERR_NOT_FOUND);
    if (!moved) {
        if (alert) {
            s_alert_file_disabled = true;
        } else {
            s_event_file_disabled = true;
        }
    }
    s_file_failures[alert ? 0U : 1U] = 0U;
    if (alert) {
        storage_lock();
        if (!s_alert_ack_queued) {
            s_alert_claimed = false;
            s_alert_head[0] = '\0';
        }
        s_snapshot.alerts_pending = 0U;
        if (moved) {
            ++s_snapshot.queue_files_quarantined;
        }
        storage_unlock();
    }
    char detail[SYSTEM_EVENT_DETAIL_LEN];
    (void)snprintf(detail, sizeof(detail), "%s file %s (%s)",
                   alert ? "alert" : "event", moved ? "set aside" : "disabled", why);
    LOG_ERROR(TAG, "%s: %s -> %s", detail, path, moved ? name : "sin usar hasta reiniciar");
    (void)system_health_report_occurrence(
        SYSTEM_MODULE_SD, SYSTEM_EVENT_DATA_LOSS, SYSTEM_SEVERITY_CRITICAL,
        moved ? ESP_ERR_INVALID_RESPONSE : q, detail);
}

static esp_err_t storage_recover_known_files(void)
{
    const char *const files[] = {
        METER_SD_HISTORY_FILE,
        METER_SD_TELEMETRY_QUEUE_FILE,
        METER_SD_ALERT_QUEUE_FILE,
        METER_SD_EVENT_LOG_FILE,
    };
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0U; i < (sizeof(files) / sizeof(files[0])); ++i) {
        const bool is_alert = (i == 2U);  /* indices de files[] arriba */
        const bool is_event = (i == 3U);
        if ((is_alert && s_alert_file_disabled) || (is_event && s_event_file_disabled)) {
            continue;
        }
        const esp_err_t err = sd_storage_recover_file(files[i]);
        if (err != ESP_OK) {
            LOG_WARN(TAG, "recovery failed for %s", files[i]);
            if (is_alert || is_event) {
                /* rc6.3: un archivo de alertas/eventos danado ya no impide
                 * montar (la auditoria y el historico siguen escribiendose). */
                storage_retire_file(is_alert, "recovery failed");
                continue;
            }
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }
    return first_error;
}

static bool storage_try_mount(void)
{
    (void)system_health_mark_recovering(SYSTEM_MODULE_SD, "SD mount/recovery");
    const esp_err_t err = sd_storage_init();
    if (err != ESP_OK) {
        storage_schedule_recovery(err, "SD mount failed");
        return false;
    }
    if (sd_storage_ensure_dir(METER_SD_BASE_DIR) != ESP_OK) {
        storage_schedule_recovery(ESP_FAIL, "SD data directory unavailable");
        return false;
    }
    const esp_err_t recovery_err = storage_recover_known_files();
    if (recovery_err != ESP_OK) {
        storage_schedule_recovery(recovery_err, "SD file recovery failed");
        return false;
    }
    s_mount_failures = 0U;
    s_next_mount_ms = 0;
    s_queue_overflow_reported = false;
    /* Tras un remontaje (p. ej. tarjeta reinsertada) la cabeza en RAM puede no
     * coincidir con la primera linea del archivo: se vuelve a leer. Una cabeza
     * con ACK ya encolado se conserva para no retirar la linea equivocada. */
    /* rc6.1: la cabeza de telemetria NO se borra. Se identifica por el cursor
     * (offset en la auditoria, que solo crece), y un remontaje no la cambia.
     * Borrarla hacia rechazar el ACK del registro que el modem estaba
     * publicando y reenviarlo: 12 duplicados en 14 seq en el banco rc6
     * (2026-11-07), y los errores de SD ocurren justo al transmitir. Una
     * tarjeta cambiada (auditoria mas corta) la limpia storage_cursor_sync. */
    storage_lock();
    if (!s_alert_ack_queued) {
        s_alert_claimed = false;
        s_alert_head[0] = '\0';
    }
    storage_unlock();
    storage_set_mounted(true);
    s_sd_unavailable = false;
    storage_cursor_sync();
    (void)system_health_report_ok(SYSTEM_MODULE_SD, "SD mounted and recovered");
    return true;
}

static esp_err_t storage_get_usage(uint32_t *total_mb, uint32_t *free_mb)
{
#if defined(STORAGE_SERVICE_TEST_HOOKS) && (STORAGE_SERVICE_TEST_HOOKS != 0)
    if (s_usage_injected) {
        *total_mb = s_injected_total_mb;
        *free_mb = s_injected_free_mb;
        return ESP_OK;
    }
#endif
    return sd_storage_get_usage(total_mb, free_mb);
}

static esp_err_t storage_reclaim_space(void)
{
    size_t dropped = 0U;
    size_t remaining = 0U;
    uint64_t dropped_bytes = 0U;
    esp_err_t err = sd_storage_trim_oldest_lines(
        METER_SD_HISTORY_FILE, 1U, METER_SD_HISTORY_PURGE_BATCH,
        &dropped, &dropped_bytes, &remaining);
    if ((err != ESP_OK) && (err != ESP_ERR_NOT_FOUND)) {
        return err;
    }
    if (dropped > 0U) {
        LOG_WARN(TAG, "SD critical: purged %u old history rows (%llu bytes)",
                 (unsigned)dropped, (unsigned long long)dropped_bytes);
        return ESP_OK;
    }

    bool head_claimed;
    storage_lock();
    head_claimed = s_telemetry_claimed;
    storage_unlock();
    err = sd_storage_trim_oldest_lines(
        METER_SD_TELEMETRY_QUEUE_FILE, head_claimed ? 1U : 0U,
        METER_SD_TELEMETRY_PURGE_BATCH, &dropped, &dropped_bytes, &remaining);
    if ((err != ESP_OK) && (err != ESP_ERR_NOT_FOUND)) {
        return err;
    }
    if (dropped == 0U) {
        return ESP_OK;
    }

    storage_lock();
    s_snapshot.telemetry_pending = (uint32_t)remaining + (head_claimed ? 1U : 0U);
    s_snapshot.data_loss_records += (uint32_t)dropped;
    s_snapshot.data_loss_bytes += dropped_bytes;
    storage_unlock();

    char detail[96];
    (void)snprintf(detail, sizeof(detail),
                   "telemetry purged: records=%u bytes=%llu",
                   (unsigned)dropped, (unsigned long long)dropped_bytes);
    (void)system_health_report_occurrence(
        SYSTEM_MODULE_SD, SYSTEM_EVENT_DATA_LOSS, SYSTEM_SEVERITY_CRITICAL,
        ESP_ERR_NO_MEM, detail);
    return ESP_OK;
}

static void storage_refresh_usage(void)
{
    uint32_t total_mb = 0U;
    uint32_t free_mb = 0U;
    const esp_err_t err = storage_get_usage(&total_mb, &free_mb);
    if ((err != ESP_OK) || (total_mb == 0U) || (free_mb > total_mb)) {
        storage_schedule_recovery((err == ESP_OK) ? ESP_FAIL : err,
                                  "SD capacity query failed");
        return;
    }
    const uint8_t used = (uint8_t)(((uint64_t)(total_mb - free_mb) * 100ULL) /
                                   (uint64_t)total_mb);
    storage_lock();
    s_snapshot.total_mb = total_mb;
    s_snapshot.free_mb = free_mb;
    s_snapshot.used_percent = used;
    storage_unlock();
    const bool reserve_reached = free_mb <= METER_SD_ALERT_RESERVE_MB;
    if ((used >= SYSTEM_SD_SPACE_CRITICAL_PCT) || reserve_reached) {
        (void)system_health_report_failure(
            SYSTEM_MODULE_SD, SYSTEM_EVENT_SD_FULL, SYSTEM_SEVERITY_CRITICAL,
            ESP_ERR_NO_MEM, "SD reserve mode: history/telemetry paused");
        const esp_err_t reclaim_err = storage_reclaim_space();
        if (reclaim_err != ESP_OK) {
            storage_schedule_recovery(reclaim_err, "SD retention rewrite failed");
        }
    } else if (used >= SYSTEM_SD_SPACE_WARNING_PCT) {
        (void)system_health_report_failure(
            SYSTEM_MODULE_SD, SYSTEM_EVENT_SD_LOW_SPACE, SYSTEM_SEVERITY_WARNING,
            ESP_ERR_NO_MEM, "SD usage at or above warning threshold");
    } else {
        (void)system_health_report_ok(SYSTEM_MODULE_SD, "SD space available");
    }
}

static bool storage_reserve_available(storage_request_type_t type)
{
    if ((type == STORAGE_REQ_ALERT) || (type == STORAGE_REQ_EVENT)) {
        return true;
    }
    storage_lock();
    const uint8_t used = s_snapshot.used_percent;
    const uint32_t free_mb = s_snapshot.free_mb;
    const uint32_t total_mb = s_snapshot.total_mb;
    storage_unlock();
    return (used < SYSTEM_SD_SPACE_CRITICAL_PCT) &&
           ((total_mb == 0U) || (free_mb > METER_SD_ALERT_RESERVE_MB));
}

static esp_err_t storage_write_request(const storage_request_t *request)
{
    if ((request == NULL) || !storage_reserve_available(request->type)) {
        return ESP_ERR_NO_MEM;
    }
    const char *path = NULL;
    char *const wrapped = s_line_scratch;
    const char *line = request->line;
    esp_err_t err = ESP_OK;
    switch (request->type) {
        case STORAGE_REQ_TELEMETRY:
            path = METER_SD_TELEMETRY_QUEUE_FILE;
            err = storage_wrap_payload(SYSTEM_RECORD_TELEMETRY_PENDING,
                                       "telemetry_pending", request->line,
                                       wrapped, STORAGE_SERVICE_LINE_MAX);
            line = wrapped;
            break;
        case STORAGE_REQ_ALERT:
            path = METER_SD_ALERT_QUEUE_FILE;
            err = storage_wrap_payload(SYSTEM_RECORD_ALERT_PENDING,
                                       "alert_pending", request->line,
                                       wrapped, STORAGE_SERVICE_LINE_MAX);
            line = wrapped;
            break;
        case STORAGE_REQ_EVENT:
            path = METER_SD_EVENT_LOG_FILE;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) {
        err = sd_storage_append_line(path, line);
    }
    if (err == ESP_OK) {
        storage_lock();
        if (request->type == STORAGE_REQ_TELEMETRY) {
            ++s_snapshot.telemetry_pending;
        } else if (request->type == STORAGE_REQ_ALERT) {
            ++s_snapshot.alerts_pending;
        }
        storage_unlock();
    }
    return err;
}

static esp_err_t storage_extract_payload(const char *line, char *out, size_t out_len)
{
    /* New telemetry is already an immutable SenML packet; old wrapped
     * queue entries remain readable after upgrading the firmware. */
    if (line[0] == '[') {
        const size_t len = strlen(line);
        if ((len >= out_len) || (line[len - 1U] != ']')) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, line, len + 1U);
        return ESP_OK;
    }
    const char marker[] = "\"payload\":";
    const char *begin = strstr(line, marker);
    const char *end = strrchr(line, '}');
    if ((begin == NULL) || (end == NULL)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    begin += sizeof(marker) - 1U;
    if (end <= begin) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t len = (size_t)(end - begin);
    if (len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    (void)memcpy(out, begin, len);
    out[len] = '\0';
    return ESP_OK;
}

static const char *storage_kind_name(storage_record_kind_t kind)
{
    return (kind == STORAGE_RECORD_ALERT) ? "alert" : "telemetry";
}

/* La cola completa pasa a <archivo>.badN (nada se borra). Ultimo recurso
 * cuando la cabeza no puede leerse ni retirarse: sin esto la cola quedaba
 * detenida en silencio (soak 2026-11-02). La telemetria sigue disponible en
 * el archivo de auditoria para recuperacion manual. */
static esp_err_t storage_quarantine_queue(storage_record_kind_t kind,
                                          const char *path)
{
    char name[64] = {0};
    const esp_err_t err = sd_storage_quarantine_file(path, name, sizeof(name));
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    storage_lock();
    if (kind == STORAGE_RECORD_ALERT) {
        s_snapshot.alerts_pending = 0U;
    } else {
        s_snapshot.telemetry_pending = 0U;
    }
    ++s_snapshot.queue_files_quarantined;
    storage_unlock();
    char detail[SYSTEM_EVENT_DETAIL_LEN];
    (void)snprintf(detail, sizeof(detail), "%s queue quarantined",
                   storage_kind_name(kind));
    LOG_ERROR(TAG, "%s: %s -> %s", detail, path, name);
    (void)system_health_report_occurrence(
        SYSTEM_MODULE_SD, SYSTEM_EVENT_DATA_LOSS, SYSTEM_SEVERITY_CRITICAL,
        ESP_ERR_INVALID_RESPONSE, detail);
    return ESP_OK;
}

/* Retira una cabeza que nunca podra publicarse (JSON truncado o linea mas
 * larga que el buffer). Antes quedaba sin reclamar y la cola parecia vacia
 * para siempre. Si tampoco puede retirarse, la cola pasa a cuarentena. */
static esp_err_t storage_discard_head(storage_record_kind_t kind,
                                      const char *path,
                                      const char *reason)
{
    size_t remaining = 0U;
    const esp_err_t err = sd_storage_drop_first_line(path, &remaining);
    if (err == ESP_ERR_INVALID_SIZE) {
        return storage_quarantine_queue(kind, path);
    }
    if ((err != ESP_OK) && (err != ESP_ERR_NOT_FOUND)) {
        return err;
    }
    storage_lock();
    if (kind == STORAGE_RECORD_ALERT) {
        s_snapshot.alerts_pending = (uint32_t)remaining;
    } else {
        s_snapshot.telemetry_pending = (uint32_t)remaining;
    }
    ++s_snapshot.queue_heads_discarded;
    storage_unlock();
    char detail[SYSTEM_EVENT_DETAIL_LEN];
    (void)snprintf(detail, sizeof(detail), "%s head discarded: %s",
                   storage_kind_name(kind), reason);
    LOG_WARN(TAG, "%s (%s, quedan %u)", detail, path, (unsigned)remaining);
    (void)system_health_report_occurrence(
        SYSTEM_MODULE_SD, SYSTEM_EVENT_DATA_LOSS, SYSTEM_SEVERITY_WARNING,
        ESP_ERR_INVALID_RESPONSE, detail);
    return ESP_OK;
}

static esp_err_t storage_refresh_head(storage_record_kind_t kind)
{
    if (kind == STORAGE_RECORD_TELEMETRY) {
        return storage_refresh_telemetry_head();
    }
    char *const line = s_line_scratch;
    char *const payload = s_payload_scratch;
    const char *path = storage_path_for_kind(kind);
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t idx = (kind == STORAGE_RECORD_ALERT) ? 1U : 0U;
    if ((kind == STORAGE_RECORD_ALERT) && s_alert_file_disabled) {
        return ESP_OK;  /* rc6.3: archivo fuera de uso en este arranque */
    }
    storage_lock();
    const bool already_claimed = (kind == STORAGE_RECORD_ALERT)
        ? s_alert_claimed : s_telemetry_claimed;
    storage_unlock();
    if (already_claimed) {
        /* La cabeza ya esta en RAM hasta su ACK; releer la tarjeta cada 50 ms
         * solo agregaba trafico al bus SPI compartido con el ADE. */
        return ESP_OK;
    }

    esp_err_t err = sd_storage_peek_line(path, line, STORAGE_SERVICE_LINE_MAX);
    /* rc4: en E4-01 hubo 5 "SD queue read failed" durante el vaciado de 485
     * registros, todos resueltos con un remontaje de 1 s. Un segundo intento
     * tras una pausa corta evita el remontaje si fue transitorio. rc6.3: en
     * E8 (10-11) fueron 39 en ~19 h con el modem transmitiendo; se agrega un
     * tercer intento a los 100 ms antes de remontar (leer no tiene efectos). */
    const uint32_t retry_ms[2] = {20U, STORAGE_READ_RETRY2_DELAY_MS};
    for (size_t r = 0U; (r < 2U) && (err != ESP_OK) && (err != ESP_ERR_NOT_FOUND) &&
                        (err != ESP_ERR_INVALID_SIZE); ++r) {
        vTaskDelay(pdMS_TO_TICKS(retry_ms[r]));
        err = sd_storage_peek_line(path, line, STORAGE_SERVICE_LINE_MAX);
        if ((err == ESP_OK) || (err == ESP_ERR_NOT_FOUND) ||
            (err == ESP_ERR_INVALID_SIZE)) {
            storage_lock();
            ++s_snapshot.queue_read_retries_ok;
            storage_unlock();
            LOG_WARN(TAG, "lectura de cola %s recuperada en el intento %u", path,
                     (unsigned)(r + 2U));
        }
    }
    if ((err != ESP_OK) && (err != ESP_ERR_NOT_FOUND) &&
        (err != ESP_ERR_INVALID_SIZE)) {
        /* Error de lectura: remontar. Si se repite sobre la misma cabeza, la
         * cola va a cuarentena para no bloquear la entrega indefinidamente. */
        if (s_head_read_failures[idx] < UINT8_MAX) {
            ++s_head_read_failures[idx];
        }
        if (s_head_read_failures[idx] >= 3U) {
            s_head_read_failures[idx] = 0U;
            return storage_quarantine_queue(kind, path);
        }
        return err;
    }
    s_head_read_failures[idx] = 0U;

    if (err == ESP_ERR_INVALID_SIZE) {
        return storage_discard_head(kind, path, "line too long");
    }
    bool available = false;
    if (err == ESP_OK) {
        if (storage_extract_payload(line, payload,
                                    STORAGE_SERVICE_PAYLOAD_MAX) != ESP_OK) {
            return storage_discard_head(kind, path, "invalid JSON");
        }
        available = true;
    }

    storage_lock();
    char *target = (kind == STORAGE_RECORD_ALERT) ? s_alert_head : s_telemetry_head;
    bool *claimed = (kind == STORAGE_RECORD_ALERT) ? &s_alert_claimed : &s_telemetry_claimed;
    if (!*claimed) {
        if (available) {
            (void)snprintf(target, STORAGE_SERVICE_PAYLOAD_MAX, "%s", payload);
            *claimed = true;
            if (kind == STORAGE_RECORD_ALERT) {
                if (s_snapshot.alerts_pending == 0U) {
                    s_snapshot.alerts_pending = 1U;
                }
            } else if (s_snapshot.telemetry_pending == 0U) {
                s_snapshot.telemetry_pending = 1U;
            }
        } else {
            target[0] = '\0';
        }
    }
    storage_unlock();
    return ESP_OK;
}

static esp_err_t storage_cursor_save(const storage_cursor_t *cursor)
{
    if (!s_cursor_nvs_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_blob(s_cursor_nvs, STORAGE_CURSOR_NVS_KEY,
                                 cursor, sizeof(*cursor));
    if (err == ESP_OK) {
        err = nvs_commit(s_cursor_nvs);
    }
    return err;
}

/* Tras cada montaje: carga el cursor (primera vez), mide la auditoria y
 * detecta una tarjeta cambiada (auditoria mas corta que el cursor). */
static void storage_cursor_sync(void)
{
    uint64_t size = 0U;
    const esp_err_t size_err = sd_storage_get_file_size(METER_SD_TELEMETRY_AUDIT_FILE,
                                                        &size);
    if ((size_err != ESP_OK) && (size_err != ESP_ERR_NOT_FOUND)) {
        return;  /* se reintenta en el proximo refresco */
    }
    if (!s_cursor_nvs_ok) {
        esp_err_t err = nvs_flash_init();
        if ((err == ESP_OK) || (err == ESP_ERR_NVS_INVALID_STATE)) {
            err = nvs_open(STORAGE_CURSOR_NVS_NAMESPACE, NVS_READWRITE, &s_cursor_nvs);
        }
        s_cursor_nvs_ok = (err == ESP_OK);
        if (!s_cursor_nvs_ok) {
            LOG_ERROR(TAG, "NVS del cursor no disponible: %s (cursor solo en RAM)",
                      esp_err_to_name(err));
        }
    }
    storage_lock();
    s_audit_size = size;
    storage_unlock();
    if (!s_cursor_ready) {
        storage_cursor_t stored = {0};
        size_t len = sizeof(stored);
        const esp_err_t get_err = s_cursor_nvs_ok
            ? nvs_get_blob(s_cursor_nvs, STORAGE_CURSOR_NVS_KEY, &stored, &len)
            : ESP_ERR_INVALID_STATE;
        if ((get_err == ESP_OK) && (len == sizeof(stored))) {
            s_cursor = stored;
            LOG_INFO(TAG, "cursor de entrega: offset %llu (ultimo ack boot=%lu seq=%lu)",
                     (unsigned long long)s_cursor.offset,
                     (unsigned long)s_cursor.boot_id, (unsigned long)s_cursor.seq);
        } else {
            /* Primer arranque con rc5: lo anterior de la auditoria ya salio
             * por la cola vieja o queda para recuperacion manual; se empieza
             * al final para no reenviar todo el historico. */
            s_cursor.offset = size;
            s_cursor.boot_id = 0U;
            s_cursor.seq = 0U;
            (void)storage_cursor_save(&s_cursor);
            char detail[96];  /* el evento lo recorta a SYSTEM_EVENT_DETAIL_LEN */
            (void)snprintf(detail, sizeof(detail), "cursor nuevo al final: %llu",
                           (unsigned long long)size);
            LOG_WARN(TAG, "%s", detail);
            (void)system_health_report_occurrence(
                SYSTEM_MODULE_SD, SYSTEM_EVENT_MODULE_READY, SYSTEM_SEVERITY_INFO,
                ESP_OK, detail);
        }
        s_cursor_ready = true;
    }
    if (s_cursor.offset > size) {
        /* Auditoria mas corta que el cursor: tarjeta nueva o archivo nuevo.
         * Todo lo que contiene es posterior: se envia desde el inicio. */
        char detail[96];  /* el evento lo recorta a SYSTEM_EVENT_DETAIL_LEN */
        (void)snprintf(detail, sizeof(detail), "cursor %llu > auditoria %llu: desde 0",
                       (unsigned long long)s_cursor.offset, (unsigned long long)size);
        LOG_WARN(TAG, "%s", detail);
        (void)system_health_report_occurrence(
            SYSTEM_MODULE_SD, SYSTEM_EVENT_IO_ERROR, SYSTEM_SEVERITY_WARNING,
            ESP_ERR_INVALID_SIZE, detail);
        s_cursor.offset = 0U;
        (void)storage_cursor_save(&s_cursor);
        storage_lock();
        if (!s_telemetry_ack_queued) {
            s_telemetry_claimed = false;
            s_telemetry_head[0] = '\0';
        }
        storage_unlock();
    }
    storage_lock();
    storage_update_pending_estimate_locked();
    storage_unlock();
}

/* Lee el numero que sigue a "n":"<nombre>", ... "v": en un registro SenML. */
static uint32_t storage_senml_u32(const char *payload, const char *name)
{
    char key[32];
    (void)snprintf(key, sizeof(key), "\"n\":\"%s\"", name);
    const char *at = strstr(payload, key);
    if (at == NULL) {
        return 0U;
    }
    const char *v = strstr(at, "\"v\":");
    if (v == NULL) {
        return 0U;
    }
    return (uint32_t)strtoul(v + 4, NULL, 10);
}

static void storage_skip_audit_line(uint64_t next_offset, const char *reason)
{
    char detail[96];  /* el evento lo recorta a SYSTEM_EVENT_DETAIL_LEN */
    (void)snprintf(detail, sizeof(detail), "auditoria: linea %llu saltada (%s)",
                   (unsigned long long)s_cursor.offset, reason);
    LOG_WARN(TAG, "%s", detail);
    s_cursor.offset = next_offset;
    (void)storage_cursor_save(&s_cursor);
    storage_lock();
    ++s_snapshot.queue_heads_discarded;
    if (s_audit_size < next_offset) {
        s_audit_size = next_offset;
    }
    storage_unlock();
    (void)system_health_report_occurrence(
        SYSTEM_MODULE_SD, SYSTEM_EVENT_DATA_LOSS, SYSTEM_SEVERITY_WARNING,
        ESP_ERR_INVALID_RESPONSE, detail);
}

static esp_err_t storage_refresh_telemetry_head(void)
{
    if (!s_cursor_ready) {
        storage_cursor_sync();
        if (!s_cursor_ready) {
            return ESP_OK;
        }
    }
    storage_lock();
    const bool claimed = s_telemetry_claimed;
    const bool nothing_new = (s_audit_size <= s_cursor.offset);
    storage_unlock();
    if (claimed || nothing_new) {
        /* Sin bytes nuevos despues del cursor no se toca la tarjeta: este
         * refresco corre cada 50 ms y el bus SPI es compartido con el ADE. */
        return ESP_OK;
    }
    char *const line = s_line_scratch;
    char *const payload = s_payload_scratch;
    uint64_t next = 0U;
    esp_err_t err = sd_storage_read_line_at(METER_SD_TELEMETRY_AUDIT_FILE,
                                            s_cursor.offset, line,
                                            STORAGE_SERVICE_LINE_MAX, &next);
    /* rc6.3: tercer intento a los 100 ms (ver storage_refresh_head). */
    const uint32_t retry_ms[2] = {20U, STORAGE_READ_RETRY2_DELAY_MS};
    for (size_t r = 0U; (r < 2U) && (err == ESP_FAIL); ++r) {
        vTaskDelay(pdMS_TO_TICKS(retry_ms[r]));
        err = sd_storage_read_line_at(METER_SD_TELEMETRY_AUDIT_FILE,
                                      s_cursor.offset, line,
                                      STORAGE_SERVICE_LINE_MAX, &next);
        if (err != ESP_FAIL) {
            storage_lock();
            ++s_snapshot.queue_read_retries_ok;
            storage_unlock();
        }
    }
    if (err == ESP_ERR_NOT_FOUND) {
        /* Nada completo desde el cursor (final cortado o tamano estimado de
         * mas): no se vuelve a leer hasta la proxima escritura. */
        storage_lock();
        s_telemetry_head[0] = '\0';
        s_audit_size = s_cursor.offset;
        storage_update_pending_estimate_locked();
        storage_unlock();
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        storage_skip_audit_line(next, "linea demasiado larga");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;  /* remontar y reintentar; nunca saltar por un error */
    }
    if ((storage_extract_payload(line, payload, STORAGE_SERVICE_PAYLOAD_MAX) != ESP_OK) ||
        (payload[0] != '[')) {
        storage_skip_audit_line(next, "JSON incompleto");
        return ESP_OK;
    }
    storage_lock();
    (void)snprintf(s_telemetry_head, STORAGE_SERVICE_PAYLOAD_MAX, "%s", payload);
    s_telemetry_claimed = true;
    s_head_next_offset = next;
    if (s_audit_size < next) {
        s_audit_size = next;
    }
    s_head_boot = storage_senml_u32(payload, "boot_id");
    s_head_seq = storage_senml_u32(payload, "seq");
    storage_update_pending_estimate_locked();
    if (s_snapshot.telemetry_pending == 0U) {
        s_snapshot.telemetry_pending = 1U;
    }
    storage_unlock();
    return ESP_OK;
}

static esp_err_t storage_ack_telemetry(void)
{
    storage_lock();
    const bool claimed = s_telemetry_claimed;
    storage_cursor_t moved = {
        .offset = s_head_next_offset, .boot_id = s_head_boot, .seq = s_head_seq,
    };
    storage_unlock();
    if (!claimed) {
        storage_lock();
        s_telemetry_ack_queued = false;
        storage_unlock();
        return ESP_OK;
    }
    /* Sin NVS el cursor vive solo en RAM (se informa una vez al iniciar). */
    const esp_err_t err = s_cursor_nvs_ok ? storage_cursor_save(&moved) : ESP_OK;
    storage_lock();
    if (err == ESP_OK) {
        s_cursor = moved;
        s_telemetry_head[0] = '\0';
        s_telemetry_claimed = false;
        storage_update_pending_estimate_locked();
    }
    s_telemetry_ack_queued = false;
    storage_unlock();
    return err;
}

static esp_err_t storage_ack(storage_record_kind_t kind)
{
    if (kind == STORAGE_RECORD_TELEMETRY) {
        return storage_ack_telemetry();
    }
    const char *path = storage_path_for_kind(kind);
    if (kind == STORAGE_RECORD_ALERT) {
        /* rc5: ACK idempotente. Si un ACK anterior retiro la linea pero
         * informo error, la cabeza del archivo ya no es la reclamada: no se
         * retira otra (antes se perdia la siguiente alerta sin enviar). */
        char *const line = s_line_scratch;
        char *const payload = s_payload_scratch;
        const esp_err_t peek_err = sd_storage_peek_line(path, line,
                                                        STORAGE_SERVICE_LINE_MAX);
        if (peek_err == ESP_FAIL) {
            storage_lock();
            s_alert_ack_queued = false;
            storage_unlock();
            return ESP_FAIL;
        }
        bool same = false;
        if ((peek_err == ESP_OK) &&
            (storage_extract_payload(line, payload, STORAGE_SERVICE_PAYLOAD_MAX) == ESP_OK)) {
            storage_lock();
            same = (strcmp(payload, s_alert_head) == 0);
            storage_unlock();
        }
        if (!same) {
            LOG_WARN(TAG, "ACK de alerta: la cabeza ya no es la reclamada; no se retira otra");
            storage_lock();
            s_alert_head[0] = '\0';
            s_alert_claimed = false;
            s_alert_ack_queued = false;
            storage_unlock();
            return ESP_OK;
        }
    }
    size_t remaining = 0U;
    const esp_err_t err = sd_storage_drop_first_line(path, &remaining);
    storage_lock();
    char *head = (kind == STORAGE_RECORD_ALERT) ? s_alert_head : s_telemetry_head;
    bool *claimed = (kind == STORAGE_RECORD_ALERT) ? &s_alert_claimed : &s_telemetry_claimed;
    if ((err == ESP_OK) || (err == ESP_ERR_NOT_FOUND)) {
        head[0] = '\0';
        *claimed = false;
    }
    if (kind == STORAGE_RECORD_TELEMETRY) { s_telemetry_ack_queued = false; }
    else { s_alert_ack_queued = false; }
    if ((err == ESP_OK) || (err == ESP_ERR_NOT_FOUND)) {
        if (kind == STORAGE_RECORD_ALERT) {
            s_snapshot.alerts_pending = (uint32_t)remaining;
        } else {
            s_snapshot.telemetry_pending = (uint32_t)remaining;
        }
    }
    storage_unlock();
    return (err == ESP_ERR_NOT_FOUND) ? ESP_OK : err;
}

/* rc6.3: errores que vale la pena reintentar (no los de estado o tamano). */
static bool storage_retryable(esp_err_t err)
{
    return (err != ESP_OK) && (err != ESP_ERR_NO_MEM) && (err != ESP_ERR_INVALID_ARG) &&
           (err != ESP_ERR_INVALID_SIZE) && (err != ESP_ERR_INVALID_STATE);
}

/* rc6.3: append con un reintento. Si el primer intento escribio la linea y
 * fallo al cerrar, queda repetida: en la auditoria el backend la descarta por
 * boot_id/seq (duplicado idempotente); en el historico se filtra por seq. */
static esp_err_t storage_append_retry(const char *path, const char *line)
{
    esp_err_t err = sd_storage_append_line(path, line);
    if (storage_retryable(err)) {
        vTaskDelay(pdMS_TO_TICKS(STORAGE_WRITE_RETRY_DELAY_MS));
        err = sd_storage_append_line(path, line);
        if (err == ESP_OK) {
            LOG_WARN(TAG, "append %s recuperado al segundo intento", path);
        }
    }
    return err;
}

static const char k_history_header[] =
    "schema,device,boot_id,sequence,timestamp_utc,local_bogota,time_valid,"
    "uptime_ms,type,vrms,irms_a,p_w,q_var,s_va,pf,f_hz,e_wh,temp_c,pq";

/* Genera el registro del periodo en el anillo RAM (aunque la SD no este).
 * Con el anillo lleno no consume seq: devuelve ESP_ERR_NO_MEM. */
static esp_err_t storage_generate_record(void)
{
    if (s_pending_count >= STORAGE_PENDING_SLOTS) {
        storage_lock();
        ++s_snapshot.generation_suspended;
        storage_unlock();
        return ESP_ERR_NO_MEM;
    }
    system_record_base_t base;
    esp_err_t err = system_record_base_create(SYSTEM_RECORD_MEASUREMENT, &base);
    if (err != ESP_OK) {
        return err;
    }
    /* Separate measurement sequence: health events never create PDR gaps. */
    base.sequence = node_health_data_generated_inc();
    const MeterData_t meter = meter_data_get_snapshot();
    const uint8_t slot = (uint8_t)((s_pending_first + s_pending_count) %
                                   STORAGE_PENDING_SLOTS);
    storage_pending_record_t *const rec = &s_pending[slot];
    char local[40];
    char temp_cell[24] = {0};
    storage_format_local(&base, local, sizeof(local));
    if (meter.temperature_valid != 0U) {
        (void)snprintf(temp_cell, sizeof(temp_cell), "%.2f", (double)meter.temperature);
    }
    const int n = snprintf(
        rec->row, sizeof(rec->row),
        "%u,%s,%lu,%lu,%lld,%s,%s,%lld,measurement,%.3f,%.4f,%.3f,"
        "%.3f,%.3f,%.4f,%.3f,%.4f,%s,%lu",
        (unsigned)base.schema_version, mqtt_topics_device_id(),
        (unsigned long)base.boot_id, (unsigned long)base.sequence,
        (long long)base.timestamp_utc_s, local,
        base.time_valid ? "true" : "false", (long long)base.uptime_ms,
        (double)meter.vrms, (double)meter.irms_a,
        (double)meter.active_power_w, (double)meter.reactive_power_var,
        (double)meter.apparent_power_va, (double)meter.power_factor,
        (double)meter.frequency, meter.energy_total_nvs_wh,
        temp_cell, (unsigned long)meter.pq_status);
    if ((n <= 0) || ((size_t)n >= sizeof(rec->row))) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = data_serializer_build_senml_datos_record(&meter, &base, rec->payload,
                                                   sizeof(rec->payload));
    if (err != ESP_OK) {
        return err;
    }
    rec->audit_done = false;
    ++s_pending_count;
    storage_lock();
    storage_update_pending_estimate_locked();
    storage_unlock();
    LOG_INFO(TAG, "measurement generated boot=%lu seq=%lu time_valid=%u buffered=%u",
             (unsigned long)base.boot_id, (unsigned long)base.sequence,
             (unsigned)base.time_valid, (unsigned)s_pending_count);
    return ESP_OK;
}

/* Escribe en orden los registros del anillo: primero la auditoria (fuente de
 * la entrega; "these exact bytes go over MQTT") y luego el historico. */
static esp_err_t storage_flush_pending(void)
{
    while (s_pending_count > 0U) {
        storage_pending_record_t *const rec = &s_pending[s_pending_first];
        esp_err_t err = ESP_OK;
        if (!rec->audit_done) {
            err = storage_append_retry(METER_SD_TELEMETRY_AUDIT_FILE, rec->payload);
            if (err != ESP_OK) {
                return err;
            }
            rec->audit_done = true;
            const uint32_t len = (uint32_t)strlen(rec->payload) + 1U;
            uint64_t exact = 0U;
            const bool have_exact =
                (sd_storage_get_file_size(METER_SD_TELEMETRY_AUDIT_FILE, &exact) == ESP_OK);
            storage_lock();
            /* El separador de una linea cortada agrega un byte: se usa el
             * tamano real cuando se puede leer. */
            s_audit_size = have_exact ? exact : (s_audit_size + len);
            s_audit_last_len = len;
            storage_unlock();
        }
        uint64_t size = 0U;
        err = sd_storage_get_file_size(METER_SD_HISTORY_FILE, &size);
        if ((err == ESP_ERR_NOT_FOUND) || ((err == ESP_OK) && (size == 0U))) {
            err = storage_append_retry(METER_SD_HISTORY_FILE, k_history_header);
        }
        if (err == ESP_OK) {
            err = storage_append_retry(METER_SD_HISTORY_FILE, rec->row);
        }
        if (err != ESP_OK) {
            return err;
        }
        s_pending_first = (uint8_t)((s_pending_first + 1U) % STORAGE_PENDING_SLOTS);
        --s_pending_count;
        storage_lock();
        storage_update_pending_estimate_locked();
        storage_unlock();
    }
    return ESP_OK;
}

static void storage_process_request(const storage_request_t *request)
{
    esp_err_t err;
    if (request->type == STORAGE_REQ_ACK_TELEMETRY) {
        err = storage_ack(STORAGE_RECORD_TELEMETRY);
    } else {
        const bool is_ack = (request->type == STORAGE_REQ_ACK_ALERT);
        err = is_ack ? storage_ack(STORAGE_RECORD_ALERT) : storage_write_request(request);
        if (storage_retryable(err)) {
            /* rc6.3: un error suelto (EMC) se reintenta antes de remontar. El
             * ACK de alertas es idempotente; una alerta o evento repetido por
             * un cierre fallido que si escribio es inofensivo. */
            vTaskDelay(pdMS_TO_TICKS(STORAGE_WRITE_RETRY_DELAY_MS));
            err = is_ack ? storage_ack(STORAGE_RECORD_ALERT) : storage_write_request(request);
            if (err == ESP_OK) {
                LOG_WARN(TAG, "operacion SD tipo %d recuperada al segundo intento",
                         (int)request->type);
            }
        }
        const bool alert_file = (request->type == STORAGE_REQ_ALERT) || is_ack;
        const bool event_file = (request->type == STORAGE_REQ_EVENT);
        if ((alert_file || event_file) && (err != ESP_ERR_NO_MEM)) {
            const size_t idx = alert_file ? 0U : 1U;
            if (err == ESP_OK) {
                s_file_failures[idx] = 0U;
            } else {
                if (s_file_failures[idx] < UINT8_MAX) {
                    ++s_file_failures[idx];
                }
                if (s_file_failures[idx] >= STORAGE_FILE_FAILURE_LIMIT) {
                    /* Fallo tras remontajes: el problema es el archivo, no la
                     * tarjeta. Se aparta y no se remonta otra vez por el. */
                    storage_record_write_result(err);
                    storage_retire_file(alert_file, "repeated write failures");
                    return;
                }
            }
        }
    }
    if (request->type == STORAGE_REQ_ACK_TELEMETRY) {
        /* rc5: el ACK de telemetria solo escribe el cursor en NVS. Un fallo
         * no es de la tarjeta: el registro se reenvia y no se remonta la SD. */
        if ((err != ESP_OK) && !s_cursor_save_fail_reported) {
            s_cursor_save_fail_reported = true;
            (void)system_health_report_occurrence(
                SYSTEM_MODULE_NVS, SYSTEM_EVENT_IO_ERROR, SYSTEM_SEVERITY_WARNING,
                err, "cursor de entrega no guardado; se reenvia");
        } else if (err == ESP_OK) {
            s_cursor_save_fail_reported = false;
        }
        return;
    }
    storage_record_write_result(err);
    if (err == ESP_ERR_NO_MEM) {
        storage_lock();
        ++s_snapshot.requests_dropped;
        storage_unlock();
        return;
    }
    if (err != ESP_OK) {
        storage_schedule_recovery(err, "SD queued operation failed");
    }
}

static void task_storage(void *arg)
{
    (void)arg;
    for (;;) {
        const int64_t now = storage_now_ms();
        if ((s_last_stack_log_ms == 0) ||
            ((now - s_last_stack_log_ms) >= STORAGE_STACK_LOG_PERIOD_MS)) {
            LOG_INFO(TAG, "stack task_storage: %u unidades libres (minimo historico)",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            s_last_stack_log_ms = now;
        }
        /* rc5: la generacion sigue el periodo aunque la SD no este; el
         * registro espera en RAM con su seq y se escribe al remontar. */
        if ((now - s_last_history_ms) >= (int64_t)METER_SD_HISTORY_PERIOD_MS) {
            s_last_history_ms = now;
            const esp_err_t gen_err = storage_generate_record();
            if ((gen_err != ESP_OK) && (gen_err != ESP_ERR_NO_MEM)) {
                LOG_WARN(TAG, "no se pudo generar el registro: %s",
                         esp_err_to_name(gen_err));
            }
        }
        if (!sd_storage_is_mounted()) {
            if (now >= s_next_mount_ms) {
                (void)storage_try_mount();
            }
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }
        if ((now - s_last_usage_ms) >= (int64_t)METER_SD_USAGE_REFRESH_MS) {
            storage_refresh_usage();
            s_last_usage_ms = now;
            if (!sd_storage_is_mounted()) {
                continue;
            }
        }
        /* rc6: nada entra a la auditoria antes de fijar el cursor. En el
         * primer arranque el cursor se pone al final del archivo; si el anillo
         * se escribia antes, sus registros quedaban detras del cursor y no se
         * enviaban (seq 1-6 del boot 179757096). El cursor se fija al montar
         * o en el refresco de la cabeza de telemetria al final de esta vuelta. */
        if ((s_pending_count > 0U) && s_cursor_ready &&
            storage_reserve_available(STORAGE_REQ_TELEMETRY)) {
            const esp_err_t err = storage_flush_pending();
            storage_record_write_result(err);
            if (err != ESP_OK) {
                storage_schedule_recovery(err, "SD history write failed");
                continue;
            }
        }
        storage_request_t request;
        if ((xQueueReceive(s_high_queue, &request, 0) == pdTRUE) ||
            (xQueueReceive(s_normal_queue, &request, 0) == pdTRUE)) {
            storage_process_request(&request);
            continue;
        }
        esp_err_t head_err = storage_refresh_head(STORAGE_RECORD_ALERT);
        if (head_err == ESP_OK) {
            head_err = storage_refresh_head(STORAGE_RECORD_TELEMETRY);
        }
        if (head_err != ESP_OK) {
            storage_schedule_recovery(head_err, "SD queue read failed");
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(50U));
    }
}

esp_err_t storage_service_start(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK;
    }
    if (!system_health_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)data_serializer_init();
    (void)node_health_init();
    s_snapshot_mutex = xSemaphoreCreateMutexStatic(&s_snapshot_mutex_buffer);
    s_normal_queue = xQueueCreateStatic(STORAGE_NORMAL_QUEUE_LEN,
                                        sizeof(storage_request_t),
                                        s_normal_queue_storage,
                                        &s_normal_queue_buffer);
    s_high_queue = xQueueCreateStatic(STORAGE_HIGH_QUEUE_LEN,
                                      sizeof(storage_request_t),
                                      s_high_queue_storage,
                                      &s_high_queue_buffer);
    if ((s_snapshot_mutex == NULL) || (s_normal_queue == NULL) ||
        (s_high_queue == NULL)) {
        return ESP_ERR_NO_MEM;
    }
    (void)memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_next_mount_ms = 0;
    s_last_stack_log_ms = 0;
    s_task_handle = xTaskCreateStaticPinnedToCore(
        task_storage, "task_storage", TASK_STACK_STORAGE, NULL,
        TASK_PRIO_STORAGE, s_task_stack, &s_task_buffer, tskNO_AFFINITY);
    if (s_task_handle == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

#if defined(STORAGE_SERVICE_TEST_HOOKS) && (STORAGE_SERVICE_TEST_HOOKS != 0)
void storage_service_test_inject_usage(uint32_t total_mb, uint32_t free_mb)
{
    s_injected_total_mb = total_mb;
    s_injected_free_mb = free_mb;
    s_usage_injected = true;
}

void storage_service_test_clear_usage(void)
{
    s_usage_injected = false;
}
#endif
