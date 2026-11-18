#include "sim7080g_hal.h"

#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger.h"

static const char *TAG = "sim7080g_hal";
static sim7080g_hal_config_t s_cfg;
static SemaphoreHandle_t s_at_mutex;
static bool s_ready;
static int64_t s_last_rx_us;

/* rc6.4: comandos remotos. El URC +SMSUB llega cuando el broker lo entrega, no
 * cuando se pregunta. Antes se perdia en el uart_flush_input() de cada comando
 * AT o como renglon ignorado dentro de la respuesta de otro comando, y el reset
 * remoto no llego nunca (E8, 2026-11-11: 9 intentos sin reinicio). Ahora los
 * +SMSUB se guardan aqui hasta que sim7080g_hal_read_urc("+SMSUB:") los pida.
 * Todo acceso ocurre con s_at_mutex tomado. */
#define HAL_URC_SLOTS        4U
#define HAL_URC_LEN          192U
#define HAL_DRAIN_MAX_US     100000LL
static const char k_urc_smsub[] = "+SMSUB:";
static char s_urc[HAL_URC_SLOTS][HAL_URC_LEN];
static uint8_t s_urc_head;
static uint8_t s_urc_count;
static uint32_t s_urc_dropped;

static bool hal_urc_stash_locked(const char *line)
{
    if (strstr(line, k_urc_smsub) == NULL) {
        return false;
    }
    if (s_urc_count >= HAL_URC_SLOTS) {
        /* Cola llena: se descarta el mas viejo; el ultimo comando manda. */
        s_urc_head = (uint8_t)((s_urc_head + 1U) % HAL_URC_SLOTS);
        s_urc_count--;
        s_urc_dropped++;
        LOG_WARN(TAG, "URC +SMSUB descartado por cola llena (total %lu)",
                 (unsigned long)s_urc_dropped);
    }
    const uint8_t slot = (uint8_t)((s_urc_head + s_urc_count) % HAL_URC_SLOTS);
    (void)snprintf(s_urc[slot], HAL_URC_LEN, "%s", line);
    s_urc_count++;
    LOG_INFO(TAG, "URC guardado: %s", line);
    return true;
}

static bool hal_urc_pop_locked(char *out, size_t out_len)
{
    if (s_urc_count == 0U) {
        return false;
    }
    (void)snprintf(out, out_len, "%s", s_urc[s_urc_head]);
    s_urc_head = (uint8_t)((s_urc_head + 1U) % HAL_URC_SLOTS);
    s_urc_count--;
    return true;
}

/* Lee lo pendiente en el UART antes de un comando AT y guarda los +SMSUB.
 * Si queda un renglon a medias espera hasta HAL_DRAIN_MAX_US para completarlo. */
static void hal_drain_input_locked(void)
{
    size_t avail = 0U;
    if ((uart_get_buffered_data_len((uart_port_t)s_cfg.uart_num, &avail) != ESP_OK) ||
        (avail == 0U)) {
        return;
    }
    const int64_t limite_us = esp_timer_get_time() + HAL_DRAIN_MAX_US;
    char line[HAL_URC_LEN];
    size_t len = 0U;
    while (esp_timer_get_time() < limite_us) {
        uint8_t ch = 0U;
        const TickType_t espera = (len > 0U) ? pdMS_TO_TICKS(10) : 0;
        const int n = uart_read_bytes((uart_port_t)s_cfg.uart_num, &ch, 1, espera);
        if (n <= 0) {
            if (len == 0U) {
                break;
            }
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (len > 0U) {
                line[len] = '\0';
                (void)hal_urc_stash_locked(line);
                len = 0U;
            }
            continue;
        }
        if (len + 1U < sizeof(line)) {
            line[len++] = (char)ch;
        } else {
            len = 0U;
        }
    }
}

static esp_err_t sim7080g_hal_wait_response_locked(const char *expected,
                                                   char *response,
                                                   size_t response_len,
                                                   uint32_t timeout_ms)
{
    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = ((int64_t)timeout_ms) * 1000LL;
    char line[160];
    size_t line_len = 0U;
    size_t resp_len = 0U;
    size_t resp_line_start = 0U;

    if ((response != NULL) && (response_len > 0U)) {
        response[0] = '\0';
    }

    for (;;) {
        const int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (elapsed_us >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }

        uint8_t ch = 0U;
        const int n = uart_read_bytes((uart_port_t)s_cfg.uart_num,
                                      &ch,
                                      1,
                                      pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }

        if ((line_len == 0U) && (ch != '\r') && (ch != '\n')) {
            resp_line_start = resp_len;
        }

        if ((response != NULL) && (response_len > 1U) && (resp_len + 1U < response_len)) {
            response[resp_len++] = (char)ch;
            response[resp_len] = '\0';
            if ((expected != NULL) && (strstr(response, expected) != NULL)) {
                s_last_rx_us = esp_timer_get_time();
                return ESP_OK;
            }
        } else if ((expected != NULL) && (strlen(expected) == 1U) && ((char)ch == expected[0])) {
            // Prompt-style tokens (ej. '>') suelen no venir en una linea completa.
            s_last_rx_us = esp_timer_get_time();
            return ESP_OK;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            if (line_len == 0U) {
                continue;
            }
            line[line_len] = '\0';
            s_last_rx_us = esp_timer_get_time();
            LOG_DEBUG(TAG, "< %s", line);

            /* rc6.4: un +SMSUB dentro de la respuesta de otro comando se guarda
             * y se quita de esa respuesta, para no perderlo ni ensuciarla. */
            if (hal_urc_stash_locked(line)) {
                if ((response != NULL) && (response_len > 0U) && (resp_line_start < response_len)) {
                    resp_len = resp_line_start;
                    response[resp_len] = '\0';
                }
                line_len = 0U;
                continue;
            }

            if ((expected != NULL) && (strstr(line, expected) != NULL)) {
                return ESP_OK;
            }
            if (strcmp(line, "OK") == 0) {
                return (expected == NULL) ? ESP_OK : ESP_FAIL;
            }
            if (strstr(line, "ERROR") != NULL) {
                return ESP_FAIL;
            }

            line_len = 0U;
            continue;
        }

        if (line_len + 1U < sizeof(line)) {
            line[line_len++] = (char)ch;
        } else {
            line_len = 0U;
        }
    }
}

esp_err_t sim7080g_hal_read_urc(const char *prefix,
                                 char *out,
                                 size_t out_len,
                                 uint32_t timeout_ms)
{
    if (!s_ready || (prefix == NULL) || (out == NULL) || (out_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_at_mutex, pdMS_TO_TICKS(timeout_ms + 200U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* rc6.4: primero los +SMSUB que llegaron durante otros comandos. */
    const bool pide_smsub = (strcmp(prefix, k_urc_smsub) == 0);
    if (pide_smsub && hal_urc_pop_locked(out, out_len)) {
        (void)xSemaphoreGive(s_at_mutex);
        return ESP_OK;
    }

    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = ((int64_t)timeout_ms) * 1000LL;
    char line[160];
    size_t line_len = 0U;
    esp_err_t result = ESP_ERR_NOT_FOUND;

    while ((esp_timer_get_time() - start_us) < timeout_us) {
        uint8_t ch = 0U;
        const int n = uart_read_bytes((uart_port_t)s_cfg.uart_num,
                                      &ch, 1, pdMS_TO_TICKS(20));
        if (n <= 0) {
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (line_len == 0U) {
                continue;
            }
            line[line_len] = '\0';
            s_last_rx_us = esp_timer_get_time();
            if (strstr(line, prefix) != NULL) {
                (void)strncpy(out, line, out_len - 1U);
                out[out_len - 1U] = '\0';
                result = ESP_OK;
                break;
            }
            if (!pide_smsub) {
                /* Un +SMSUB mientras se espera otro URC (p. ej. +CDNSGIP). */
                (void)hal_urc_stash_locked(line);
            }
            line_len = 0U;
            continue;
        }
        if (line_len + 1U < sizeof(line)) {
            line[line_len++] = (char)ch;
        } else {
            line_len = 0U;
        }
    }

    (void)xSemaphoreGive(s_at_mutex);
    return result;
}

bool sim7080g_hal_is_ready(void)
{
    return s_ready;
}

int64_t sim7080g_hal_last_rx_us(void)
{
    return s_last_rx_us;
}

esp_err_t sim7080g_hal_init(const sim7080g_hal_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* If already initialized with same config, just flush and reuse. */
    if (s_ready &&
        (s_cfg.uart_num == cfg->uart_num) &&
        (s_cfg.tx_pin == cfg->tx_pin) &&
        (s_cfg.rx_pin == cfg->rx_pin) &&
        (s_cfg.baudrate == cfg->baudrate)) {
        (void)uart_flush_input((uart_port_t)s_cfg.uart_num);
        return ESP_OK;
    }

    /* If already initialized but baud rate changed, reconfigure without
       tearing down the UART driver (avoids ESP-IDF spinlock corruption). */
    if (s_ready && (s_cfg.uart_num == cfg->uart_num)) {
        s_cfg.baudrate = cfg->baudrate;
        (void)uart_flush_input((uart_port_t)s_cfg.uart_num);
        const esp_err_t baud_err = uart_set_baudrate((uart_port_t)s_cfg.uart_num,
                                                      s_cfg.baudrate);
        if (baud_err != ESP_OK) {
            LOG_WARN(TAG, "uart_set_baudrate failed: %s", esp_err_to_name(baud_err));
            return baud_err;
        }
        LOG_INFO(TAG, "HAL reconfigured baud=%lu", (unsigned long)s_cfg.baudrate);
        return ESP_OK;
    }

    /* Fresh initialization. */
    s_cfg = *cfg;

    if (s_at_mutex == NULL) {
        s_at_mutex = xSemaphoreCreateMutex();
        if (s_at_mutex == NULL) {
            LOG_ERROR(TAG, "mutex create failed");
            return ESP_FAIL;
        }
    }

    const uart_config_t uart_cfg = {
        .baud_rate = (int)s_cfg.baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if defined(UART_SCLK_DEFAULT)
        .source_clk = UART_SCLK_DEFAULT,
#elif defined(UART_SCLK_APB)
        .source_clk = UART_SCLK_APB,
#else
        .source_clk = UART_SCLK_XTAL,
#endif
    };

    esp_err_t err = uart_driver_install((uart_port_t)s_cfg.uart_num, 2048, 1024, 0, NULL, 0);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        LOG_ERROR(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config((uart_port_t)s_cfg.uart_num, &uart_cfg);
    if (err != ESP_OK) {
        LOG_ERROR(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        (void)uart_driver_delete((uart_port_t)s_cfg.uart_num);
        return err;
    }

    err = uart_set_pin((uart_port_t)s_cfg.uart_num,
                       s_cfg.tx_pin,
                       s_cfg.rx_pin,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        LOG_ERROR(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        (void)uart_driver_delete((uart_port_t)s_cfg.uart_num);
        return err;
    }

    s_ready = true;
    LOG_INFO(TAG,
             "HAL ready uart=%d tx=%d rx=%d baud=%lu",
             s_cfg.uart_num,
             s_cfg.tx_pin,
             s_cfg.rx_pin,
             (unsigned long)s_cfg.baudrate);
    return ESP_OK;
}

esp_err_t sim7080g_hal_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    (void)uart_driver_delete((uart_port_t)s_cfg.uart_num);
    if (s_at_mutex != NULL) {
        vSemaphoreDelete(s_at_mutex);
        s_at_mutex = NULL;
    }
    (void)memset(&s_cfg, 0, sizeof(s_cfg));
    s_ready = false;
    return ESP_OK;
}

esp_err_t sim7080g_hal_send_cmd(const char *cmd,
                                const char *expected,
                                char *response,
                                size_t response_len,
                                uint32_t timeout_ms,
                                uint8_t retries)
{
    if (!s_ready || (cmd == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (retries == 0U) {
        retries = 1U;
    }

    if (xSemaphoreTake(s_at_mutex, pdMS_TO_TICKS(timeout_ms + 500U)) != pdTRUE) {
        LOG_WARN(TAG, "mutex timeout for command");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t last_err = ESP_FAIL;
    for (uint8_t attempt = 0U; attempt < retries; ++attempt) {
        /* rc6.4: rescatar los +SMSUB pendientes antes de vaciar la entrada. */
        hal_drain_input_locked();
        (void)uart_flush_input((uart_port_t)s_cfg.uart_num);
        if ((response != NULL) && (response_len > 0U)) {
            response[0] = '\0';
        }

        LOG_DEBUG(TAG, "> %s", cmd);
        int n = uart_write_bytes((uart_port_t)s_cfg.uart_num, cmd, (int)strlen(cmd));
        if (n < 0) {
            last_err = ESP_FAIL;
            continue;
        }
        n = uart_write_bytes((uart_port_t)s_cfg.uart_num, "\r\n", 2);
        if (n < 0) {
            last_err = ESP_FAIL;
            continue;
        }

        last_err = sim7080g_hal_wait_response_locked(expected, response, response_len, timeout_ms);
        if (last_err == ESP_OK) {
            break;
        }

        if ((response != NULL) && (response_len > 0U) && (response[0] != '\0')) {
            LOG_WARN(TAG,
                     "AT retry %u/%u cmd=%s err=%s resp=%s",
                     (unsigned)(attempt + 1U),
                     (unsigned)retries,
                     cmd,
                     esp_err_to_name(last_err),
                     response);
        } else {
            LOG_WARN(TAG,
                     "AT retry %u/%u cmd=%s err=%s",
                     (unsigned)(attempt + 1U),
                     (unsigned)retries,
                     cmd,
                     esp_err_to_name(last_err));
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    (void)xSemaphoreGive(s_at_mutex);
    return last_err;
}

esp_err_t sim7080g_hal_send_payload(const char *payload,
                                    bool append_crlf,
                                    const char *expected,
                                    uint32_t timeout_ms)
{
    if (!s_ready || (payload == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_at_mutex, pdMS_TO_TICKS(timeout_ms + 500U)) != pdTRUE) {
        LOG_WARN(TAG, "mutex timeout for payload");
        return ESP_ERR_TIMEOUT;
    }

    int n = uart_write_bytes((uart_port_t)s_cfg.uart_num, payload, (int)strlen(payload));
    if (n < 0) {
        (void)xSemaphoreGive(s_at_mutex);
        return ESP_FAIL;
    }
    if (append_crlf) {
        n = uart_write_bytes((uart_port_t)s_cfg.uart_num, "\r\n", 2);
        if (n < 0) {
            (void)xSemaphoreGive(s_at_mutex);
            return ESP_FAIL;
        }
    }

    const esp_err_t err =
        sim7080g_hal_wait_response_locked(expected, NULL, 0U, timeout_ms);
    (void)xSemaphoreGive(s_at_mutex);
    return err;
}

