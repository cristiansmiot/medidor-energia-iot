#include "sd_storage.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ff.h"
#include "board_config.h"
#include "logger.h"
#include "spi_shared_bus.h"

static const char *TAG = "sd_storage";

/* Solo banco (rc5): inyecta fallas de la tarjeta con probabilidad
 * METER_SD_FAULT_INJECT_PERMIL/1000 por operacion, para probar que ningun
 * error de SD pierde registros. 0 en todos los perfiles de produccion. */
#ifndef METER_SD_FAULT_INJECT_PERMIL
#define METER_SD_FAULT_INJECT_PERMIL 0
#endif
#if METER_SD_FAULT_INJECT_PERMIL > 0
#include "esp_random.h"
static bool sd_fault_injected(const char *where)
{
    const bool hit = (esp_random() % 1000U) < (uint32_t)METER_SD_FAULT_INJECT_PERMIL;
    if (hit) {
        LOG_WARN("sd_storage", "FALLA INYECTADA (banco): %s", where);
    }
    return hit;
}
#define SD_FAULT(where) sd_fault_injected(where)
#else
#define SD_FAULT(where) false
#endif

static bool s_mounted;
static sdmmc_card_t *s_card;
static StaticSemaphore_t s_storage_mutex_buffer;
static SemaphoreHandle_t s_storage_mutex;

static esp_err_t sd_storage_build_path(const char *relpath,
                                       const char *suffix,
                                       char *out,
                                       size_t out_len)
{
    if ((relpath == NULL) || (relpath[0] == '\0') ||
        (strstr(relpath, "..") != NULL) || (out == NULL) || (out_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    const int n = snprintf(out, out_len, "%s/%s%s", SD_MOUNT_POINT, relpath,
                           (suffix != NULL) ? suffix : "");
    return ((n > 0) && ((size_t)n < out_len)) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static bool sd_storage_path_exists(const char *path)
{
    struct stat st;
    return (path != NULL) && (stat(path, &st) == 0);
}

static bool sd_storage_lock(void)
{
    return (s_storage_mutex != NULL) &&
           (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) == pdTRUE);
}

static void sd_storage_unlock(void)
{
    if (s_storage_mutex != NULL) {
        (void)xSemaphoreGive(s_storage_mutex);
    }
}

esp_err_t sd_storage_init(void)
{
    if (s_storage_mutex == NULL) {
        s_storage_mutex = xSemaphoreCreateMutexStatic(&s_storage_mutex_buffer);
        if (s_storage_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_mounted) {
        return ESP_OK;
    }

    // 1) Bus SPI2 compartido: la inicializacion centralizada es idempotente.
    esp_err_t err = spi_shared_bus_init();
    if (err != ESP_OK) {
        LOG_ERROR(TAG, "inicializacion SPI2 compartido fallo: %s",
                  esp_err_to_name(err));
        return err;
    }

    // 2) Configuracion del slot SDSPI. CS manejado por el driver.
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = (gpio_num_t)SD_PIN_CS;
    slot_cfg.host_id = (spi_host_device_t)SD_SPI_HOST;

    // 3) Host SDSPI (velocidad final post-init negociada).
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = (int)(SD_SPI_CLK_HZ / 1000);

    // 4) Parametros de montaje VFS+FAT.
    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,  // si usuario inserto SD sin formato,
                                          // que sea explicito reformatearla.
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                  &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            LOG_ERROR(TAG, "mount fallo: tarjeta sin FAT o corrupta. "
                           "Formatear FAT32 y reintentar.");
        } else {
            LOG_ERROR(TAG, "mount fallo: %s", esp_err_to_name(err));
        }
        // No liberar el bus compartido: el ADE9153A puede estar agregado.
        return err;
    }

    s_mounted = true;

    if (s_card != NULL) {
        const uint64_t cap_mb =
            ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) /
            (1024ULL * 1024ULL);
        LOG_INFO(TAG,
                 "SD montada OK en %s: %s, %llu MB, SPI2=%d Hz (MOSI=%d MISO=%d SCLK=%d CS=%d)",
                 SD_MOUNT_POINT,
                 s_card->cid.name,
                 (unsigned long long)cap_mb,
                 SD_SPI_CLK_HZ,
                 SD_PIN_MOSI, SD_PIN_MISO, SD_PIN_SCLK, SD_PIN_CS);
    }
    return ESP_OK;
}

bool sd_storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t sd_storage_deinit(void)
{
    if (s_card == NULL) {
        s_mounted = false;
        return ESP_OK;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_mounted = false;
    s_card = NULL;
    sd_storage_unlock();
    return err;
}

void sd_storage_invalidate(void)
{
    s_mounted = false;
}

esp_err_t sd_storage_append_line(const char *relpath, const char *line)
{
    if ((relpath == NULL) || (line == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    char fullpath[128];
    if (sd_storage_build_path(relpath, NULL, fullpath, sizeof(fullpath)) != ESP_OK) {
        sd_storage_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    if (SD_FAULT("append antes de escribir")) {
        sd_storage_unlock();
        return ESP_FAIL;
    }
    /* rc5: si una escritura anterior quedo cortada (sin '\n' final), la
     * linea nueva empieza en un renglon propio. Sin esto se pegaba a la
     * cortada y ambas quedaban ilegibles para el lector por posicion. */
    bool needs_separator = false;
    struct stat st;
    if ((stat(fullpath, &st) == 0) && (st.st_size > 0)) {
        FILE *probe = fopen(fullpath, "r");
        if (probe == NULL) {
            LOG_WARN(TAG, "fopen(%s, 'r') fallo al revisar el final", fullpath);
            sd_storage_unlock();
            return ESP_FAIL;
        }
        int last = EOF;
        if (fseek(probe, -1L, SEEK_END) == 0) {
            last = fgetc(probe);
        }
        const bool probe_failed = (ferror(probe) != 0) || (last == EOF);
        (void)fclose(probe);
        if (probe_failed) {
            LOG_WARN(TAG, "no se pudo leer el final de %s", fullpath);
            sd_storage_unlock();
            return ESP_FAIL;
        }
        needs_separator = (last != '\n');
    }

    FILE *f = fopen(fullpath, "a");
    if (f == NULL) {
        LOG_WARN(TAG, "fopen(%s, 'a') fallo", fullpath);
        sd_storage_unlock();
        return ESP_FAIL;
    }
    if (needs_separator && (fputc('\n', f) == EOF)) {
        (void)fclose(f);
        LOG_WARN(TAG, "no se pudo separar la linea cortada en %s", fullpath);
        sd_storage_unlock();
        return ESP_FAIL;
    }

    const size_t line_len = strlen(line);
    size_t written = fwrite(line, 1U, line_len, f);
    bool newline_written = true;
    if (written == line_len) {
        if ((line_len == 0U) || (line[line_len - 1U] != '\n')) {
            const char nl = '\n';
            newline_written = (fwrite(&nl, 1U, 1U, f) == 1U);
        }
    }
    const int close_err = fclose(f);

    if ((written != line_len) || !newline_written) {
        LOG_WARN(TAG, "escritura parcial %s: %u/%u",
                 fullpath, (unsigned)written, (unsigned)line_len);
        sd_storage_unlock();
        return ESP_FAIL;
    }
    if (close_err != 0) {
        LOG_WARN(TAG, "fclose fallo %s", fullpath);
        sd_storage_unlock();
        return ESP_FAIL;
    }
    if (SD_FAULT("append escrito pero informado como fallido")) {
        sd_storage_unlock();
        return ESP_FAIL;
    }
    sd_storage_unlock();
    return ESP_OK;
}

esp_err_t sd_storage_drain_lines(const char *relpath,
                                 size_t max_lines,
                                 sd_line_consumer_t consumer,
                                 void *ctx,
                                 size_t *out_remaining)
{
    if ((relpath == NULL) || (consumer == NULL) || (max_lines == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    char fullpath[128];
    char tmppath[128];
    char bakpath[128];
    if (sd_storage_build_path(relpath, NULL, fullpath, sizeof(fullpath)) != ESP_OK) {
        sd_storage_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if ((sd_storage_build_path(relpath, ".tmp", tmppath, sizeof(tmppath)) != ESP_OK) ||
        (sd_storage_build_path(relpath, ".bak", bakpath, sizeof(bakpath)) != ESP_OK)) {
        sd_storage_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    FILE *in = fopen(fullpath, "r");
    if (in == NULL) {
        if (out_remaining != NULL) {
            *out_remaining = 0U;
        }
        sd_storage_unlock();
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    // Buffer de linea estatico (protegido por el mutex del driver): cabe
    // cualquier linea que storage_service puede leer como cabeza de cola.
    static char line[SD_STORAGE_LINE_MAX];
    size_t consumed = 0U;
    size_t remaining = 0U;
    bool consumer_stopped = false;
    FILE *out = NULL;

    while (fgets(line, sizeof(line), in) != NULL) {
        // Quitar el '\n' final para entregar la linea limpia al consumidor.
        const size_t len = strlen(line);
        if ((len > 0U) && (line[len - 1U] == '\n')) {
            line[len - 1U] = '\0';
        } else if (!feof(in)) {
            LOG_WARN(TAG, "linea truncada en %s; backlog intacto", fullpath);
            (void)fclose(in);
            if (out != NULL) {
                (void)fclose(out);
                (void)remove(tmppath);
            }
            sd_storage_unlock();
            return ESP_ERR_INVALID_SIZE;
        }
        if (line[0] == '\0') {
            continue; // linea vacia: descartar silenciosamente
        }

        if (!consumer_stopped && (consumed < max_lines)) {
            if (consumer(line, ctx) == ESP_OK) {
                ++consumed;
                continue;
            }
            consumer_stopped = true;
        }

        // Linea no consumida: preservarla en el archivo temporal.
        if (out == NULL) {
            out = fopen(tmppath, "w");
            if (out == NULL) {
                LOG_WARN(TAG, "no se pudo crear %s - backlog intacto", tmppath);
                (void)fclose(in);
                // Lo consumido se repetira en el proximo drenado; el backend
                // tolera duplicados (mediciones idempotentes por timestamp).
                sd_storage_unlock();
                return ESP_FAIL;
            }
        }
        if ((fputs(line, out) < 0) || (fputc('\n', out) == EOF)) {
            LOG_WARN(TAG, "escritura fallida en %s - backlog intacto", tmppath);
            (void)fclose(in);
            (void)fclose(out);
            (void)remove(tmppath);
            sd_storage_unlock();
            return ESP_FAIL;
        }
        ++remaining;
    }

    /* fgets() devuelve NULL tanto en EOF como ante un error de lectura. Sin
     * esta comprobacion un error a mitad de archivo se tomaba como fin y el
     * reemplazo descartaba todas las lineas posteriores. */
    if (ferror(in) != 0) {
        LOG_WARN(TAG, "error de lectura en %s - backlog intacto", fullpath);
        (void)fclose(in);
        if (out != NULL) {
            (void)fclose(out);
            (void)remove(tmppath);
        }
        sd_storage_unlock();
        return ESP_FAIL;
    }

    (void)fclose(in);
    if (out != NULL) {
        const int flush_err = fflush(out);
        const int output_close_err = fclose(out);
        if ((flush_err != 0) || (output_close_err != 0)) {
            sd_storage_unlock();
            return ESP_FAIL;
        }
        out = NULL;
        (void)remove(bakpath);
        if (rename(fullpath, bakpath) != 0) {
            LOG_WARN(TAG, "rename %s -> %s fallo", fullpath, bakpath);
            (void)remove(tmppath);
            sd_storage_unlock();
            return ESP_FAIL;
        }
        if (rename(tmppath, fullpath) != 0) {
            LOG_WARN(TAG, "rename %s -> %s fallo", tmppath, fullpath);
            (void)rename(bakpath, fullpath);
            sd_storage_unlock();
            return ESP_FAIL;
        }
        (void)remove(bakpath);
        if (SD_FAULT("drenado aplicado pero informado como fallido")) {
            sd_storage_unlock();
            return ESP_FAIL;
        }
    } else {
        // Todo consumido: el backlog quedo vacio.
        (void)remove(fullpath);
    }

    if (consumed > 0U) {
        LOG_INFO(TAG, "backlog %s: %u lineas drenadas, %u pendientes",
                 relpath, (unsigned)consumed, (unsigned)remaining);
    }
    if (out_remaining != NULL) {
        *out_remaining = remaining;
    }
    sd_storage_unlock();
    return ESP_OK;
}

esp_err_t sd_storage_recover_file(const char *relpath)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    char main_path[128];
    char tmp_path[128];
    char bak_path[128];
    esp_err_t err = ESP_OK;
    if ((sd_storage_build_path(relpath, NULL, main_path, sizeof(main_path)) != ESP_OK) ||
        (sd_storage_build_path(relpath, ".tmp", tmp_path, sizeof(tmp_path)) != ESP_OK) ||
        (sd_storage_build_path(relpath, ".bak", bak_path, sizeof(bak_path)) != ESP_OK)) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (sd_storage_path_exists(main_path)) {
        (void)remove(tmp_path);
        (void)remove(bak_path);
    } else if (sd_storage_path_exists(bak_path)) {
        if (rename(bak_path, main_path) != 0) {
            err = ESP_FAIL;
        } else {
            (void)remove(tmp_path);
        }
    } else if (sd_storage_path_exists(tmp_path) &&
               (rename(tmp_path, main_path) != 0)) {
        err = ESP_FAIL;
    }
done:
    sd_storage_unlock();
    return err;
}

/* rc6: una escritura fallida puede dejar basura (NUL o 0xFF de un cluster
 * asignado sin datos) delante de un registro valido del mismo renglon. Sin
 * esto el renglon entero se descartaba con el registro. Si el renglon no
 * empieza como JSON, se conserva desde el ultimo inicio de registro SenML. */
static void sd_strip_garbage_prefix(char *line, size_t len)
{
    static const char k_mark[] = "[{\"bn\"";
    const size_t mlen = sizeof(k_mark) - 1U;
    if ((len < mlen) || (line[0] == '[') || (line[0] == '{')) {
        return;
    }
    bool found = false;
    size_t at = 0U;
    for (size_t i = len - mlen + 1U; i-- > 0U;) {
        if (memcmp(&line[i], k_mark, mlen) == 0) {
            found = true;
            at = i;
            break;
        }
    }
    if (!found) {
        return;
    }
    LOG_WARN(TAG, "renglon con %u bytes de basura antes del registro: rescatado",
             (unsigned)at);
    memmove(line, &line[at], len - at);
    line[len - at] = '\0';
}

esp_err_t sd_storage_read_line_at(const char *relpath, uint64_t offset,
                                  char *out, size_t out_len,
                                  uint64_t *out_next_offset)
{
    if ((relpath == NULL) || (out == NULL) || (out_len < 2U) ||
        (out_next_offset == NULL) || (offset > (uint64_t)LONG_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    char path[128];
    if (sd_storage_build_path(relpath, NULL, path, sizeof(path)) != ESP_OK) {
        sd_storage_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    *out_next_offset = offset;
    if (SD_FAULT("lectura por posicion")) {
        sd_storage_unlock();
        return ESP_FAIL;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        sd_storage_unlock();
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        result = ESP_FAIL;
        goto done;
    }
    for (;;) {
        const long start = ftell(f);
        if (start < 0L) {
            result = ESP_FAIL;
            break;
        }
        if (fgets(out, (int)out_len, f) == NULL) {
            /* EOF limpio: no hay mas lineas. Error: remontar, nunca saltar. */
            result = (ferror(f) != 0) ? ESP_FAIL : ESP_ERR_NOT_FOUND;
            break;
        }
        const long end = ftell(f);
        if (end < start) {
            result = ESP_FAIL;
            break;
        }
        /* Bytes realmente leidos (strlen se corta con un NUL de basura). */
        const size_t n = (size_t)(end - start);
        if ((n > 0U) && (n < out_len) && (out[n - 1U] == '\n')) {
            size_t len = n - 1U;
            out[len] = '\0';
            if ((len > 0U) && (out[len - 1U] == '\r')) {
                --len;
                out[len] = '\0';
            }
            *out_next_offset = (uint64_t)end;
            sd_strip_garbage_prefix(out, len);
            if (out[0] == '\0') {
                continue;  /* renglon vacio: seguir con el siguiente */
            }
            result = ESP_OK;
            break;
        }
        if (feof(f)) {
            /* Final sin '\n': escritura cortada o en curso. Se espera; la
             * siguiente escritura la cierra con un separador. */
            result = ESP_ERR_NOT_FOUND;
            break;
        }
        /* Linea mas larga que el buffer (basura o dos registros pegados):
         * informar donde termina para que el servicio la salte. */
        int c = 0;
        long pos = end;
        while ((c = fgetc(f)) != EOF) {
            ++pos;
            if (c == '\n') {
                break;
            }
        }
        if (c == EOF) {
            result = (ferror(f) != 0) ? ESP_FAIL : ESP_ERR_NOT_FOUND;
            break;
        }
        *out_next_offset = (uint64_t)pos;
        out[0] = '\0';
        result = ESP_ERR_INVALID_SIZE;
        break;
    }
done:
    (void)fclose(f);
    if (result == ESP_FAIL) {
        out[0] = '\0';
        LOG_WARN(TAG, "error de lectura en %s (offset %llu)", path,
                 (unsigned long long)offset);
    }
    sd_storage_unlock();
    return result;
}

esp_err_t sd_storage_peek_line(const char *relpath, char *out, size_t out_len)
{
    if ((out == NULL) || (out_len < 2U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    char path[128];
    if (sd_storage_build_path(relpath, NULL, path, sizeof(path)) != ESP_OK) {
        sd_storage_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        sd_storage_unlock();
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    /* Devuelve la primera linea NO vacia, la misma que retira
     * sd_storage_drop_first_line(); una linea vacia al inicio ya no se
     * confunde con cola vacia. Un error de lectura se informa como ESP_FAIL
     * para que el servicio remonte la SD en vez de dar la cola por vacia. */
    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (;;) {
        if (fgets(out, out_len, f) == NULL) {
            result = (ferror(f) != 0) ? ESP_FAIL : ESP_ERR_NOT_FOUND;
            break;
        }
        const bool complete = (strchr(out, '\n') != NULL) || feof(f);
        if (!complete) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        out[strcspn(out, "\r\n")] = '\0';
        if (out[0] != '\0') {
            result = ESP_OK;
            break;
        }
    }
    (void)fclose(f);
    if (result != ESP_OK) {
        out[0] = '\0';
        if (result == ESP_FAIL) {
            LOG_WARN(TAG, "error de lectura en %s", path);
        }
    }
    sd_storage_unlock();
    return result;
}

esp_err_t sd_storage_quarantine_file(const char *relpath,
                                     char *out_name,
                                     size_t out_name_len)
{
    if (relpath == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    char src[128];
    char dst[128];
    esp_err_t err = sd_storage_build_path(relpath, NULL, src, sizeof(src));
    if (err != ESP_OK) {
        sd_storage_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd_storage_path_exists(src)) {
        sd_storage_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    /* Nunca se borra ni se sobrescribe: el contenido queda para revision o
     * recuperacion manual (la telemetria tambien esta en el archivo de auditoria). */
    err = ESP_ERR_NO_MEM;
    for (unsigned n = 1U; n <= 9U; ++n) {
        char suffix[8];
        (void)snprintf(suffix, sizeof(suffix), ".bad%u", n);
        if ((sd_storage_build_path(relpath, suffix, dst, sizeof(dst)) != ESP_OK) ||
            sd_storage_path_exists(dst)) {
            continue;
        }
        err = (rename(src, dst) == 0) ? ESP_OK : ESP_FAIL;
        if ((err == ESP_OK) && (out_name != NULL) && (out_name_len > 0U)) {
            (void)snprintf(out_name, out_name_len, "%s%s", relpath, suffix);
        }
        break;
    }
    if (err == ESP_OK) {
        LOG_WARN(TAG, "%s puesto en cuarentena como %s", src, dst);
    } else {
        LOG_WARN(TAG, "no se pudo poner %s en cuarentena (%s)", src, esp_err_to_name(err));
    }
    sd_storage_unlock();
    return err;
}

static esp_err_t sd_storage_drop_consumer(const char *line, void *ctx)
{
    (void)line;
    (void)ctx;
    return ESP_OK;
}

esp_err_t sd_storage_drop_first_line(const char *relpath, size_t *out_remaining)
{
    return sd_storage_drain_lines(relpath, 1U, sd_storage_drop_consumer,
                                  NULL, out_remaining);
}

esp_err_t sd_storage_trim_oldest_lines(const char *relpath,
                                       size_t preserve_lines,
                                       size_t max_drop,
                                       size_t *out_dropped,
                                       uint64_t *out_dropped_bytes,
                                       size_t *out_remaining)
{
    if ((relpath == NULL) || (max_drop == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    char fullpath[128];
    char tmppath[128];
    char bakpath[128];
    if ((sd_storage_build_path(relpath, NULL, fullpath, sizeof(fullpath)) != ESP_OK) ||
        (sd_storage_build_path(relpath, ".tmp", tmppath, sizeof(tmppath)) != ESP_OK) ||
        (sd_storage_build_path(relpath, ".bak", bakpath, sizeof(bakpath)) != ESP_OK)) {
        sd_storage_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    FILE *in = fopen(fullpath, "r");
    if (in == NULL) {
        sd_storage_unlock();
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    FILE *out = fopen(tmppath, "w");
    if (out == NULL) {
        (void)fclose(in);
        sd_storage_unlock();
        return ESP_FAIL;
    }

    static char line[SD_STORAGE_LINE_MAX];
    size_t line_index = 0U;
    size_t dropped = 0U;
    size_t remaining = 0U;
    uint64_t dropped_bytes = 0U;
    esp_err_t err = ESP_OK;
    while (fgets(line, sizeof(line), in) != NULL) {
        const size_t len = strlen(line);
        const bool complete = ((len > 0U) && (line[len - 1U] == '\n')) || feof(in);
        if (!complete) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        const bool prefix = line_index < preserve_lines;
        const bool discard = !prefix && (dropped < max_drop);
        ++line_index;
        if (discard) {
            ++dropped;
            dropped_bytes += (uint64_t)len;
            continue;
        }
        if (!prefix) {
            ++remaining;
        }
        if (fwrite(line, 1U, len, out) != len) {
            err = ESP_FAIL;
            break;
        }
    }

    if (ferror(in) != 0) {
        err = ESP_FAIL;
    }
    (void)fclose(in);
    const int flush_err = fflush(out);
    const int close_err = fclose(out);
    if ((flush_err != 0) || (close_err != 0)) {
        err = ESP_FAIL;
    }
    out = NULL;

    if (err != ESP_OK) {
        (void)remove(tmppath);
        sd_storage_unlock();
        return err;
    }
    if (dropped == 0U) {
        (void)remove(tmppath);
    } else {
        (void)remove(bakpath);
        if (rename(fullpath, bakpath) != 0) {
            (void)remove(tmppath);
            sd_storage_unlock();
            return ESP_FAIL;
        }
        if (rename(tmppath, fullpath) != 0) {
            (void)rename(bakpath, fullpath);
            sd_storage_unlock();
            return ESP_FAIL;
        }
        (void)remove(bakpath);
    }

    if (out_dropped != NULL) {
        *out_dropped = dropped;
    }
    if (out_dropped_bytes != NULL) {
        *out_dropped_bytes = dropped_bytes;
    }
    if (out_remaining != NULL) {
        *out_remaining = remaining;
    }
    sd_storage_unlock();
    return ESP_OK;
}

esp_err_t sd_storage_remove_file(const char *relpath)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    char path[128];
    esp_err_t err = sd_storage_build_path(relpath, NULL, path, sizeof(path));
    if (err == ESP_OK) {
        if (!sd_storage_path_exists(path)) {
            err = ESP_ERR_NOT_FOUND;
        } else if (remove(path) != 0) {
            err = ESP_FAIL;
        }
    }
    sd_storage_unlock();
    return err;
}

esp_err_t sd_storage_get_file_size(const char *relpath, uint64_t *out_size)
{
    if (out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    char path[128];
    esp_err_t err = sd_storage_build_path(relpath, NULL, path, sizeof(path));
    struct stat st;
    if ((err == ESP_OK) && (stat(path, &st) != 0)) {
        err = (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (err == ESP_OK) {
        *out_size = (uint64_t)st.st_size;
    }
    sd_storage_unlock();
    return err;
}

esp_err_t sd_storage_ensure_dir(const char *relpath)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    char path[128];
    esp_err_t err = sd_storage_build_path(relpath, NULL, path, sizeof(path));
    if ((err == ESP_OK) && (mkdir(path, 0775) != 0) && (errno != EEXIST)) {
        err = ESP_FAIL;
    }
    sd_storage_unlock();
    return err;
}

esp_err_t sd_storage_get_usage(uint32_t *out_total_mb, uint32_t *out_free_mb)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_storage_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    // La unidad drive "0:" corresponde al primer volumen FAT montado por
    // esp_vfs_fat_sdspi_mount (mapeo por defecto de FATFS de ESP-IDF).
    FRESULT r = f_getfree("0:", &free_clusters, &fs);
    if ((r != FR_OK) || (fs == NULL)) {
        sd_storage_unlock();
        return ESP_FAIL;
    }
    const uint64_t sector_sz = 512ULL;  // FATFS sobre SD usa 512 B
    const uint64_t cluster_sz = (uint64_t)fs->csize * sector_sz;
    const uint64_t total_b = (uint64_t)(fs->n_fatent - 2U) * cluster_sz;
    const uint64_t free_b  = (uint64_t)free_clusters * cluster_sz;
    if (out_total_mb != NULL) {
        *out_total_mb = (uint32_t)(total_b / (1024ULL * 1024ULL));
    }
    if (out_free_mb != NULL) {
        *out_free_mb = (uint32_t)(free_b / (1024ULL * 1024ULL));
    }
    sd_storage_unlock();
    return ESP_OK;
}
