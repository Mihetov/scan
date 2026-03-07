#include "transport/uart_transport.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define UART_TRANSPORT_RX_BUF_SIZE 1024
#define UART_TRANSPORT_TX_BUF_SIZE 1024

static const char *TAG = "uart_transport";

static void transport_log_hex_debug(const char *label, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        ESP_LOGD(TAG, "%s: <empty>", label);
        return;
    }

    ESP_LOGD(TAG, "%s: len=%u", label, (unsigned)len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_DEBUG);
}

typedef struct {
    transport_status_t status;
    SemaphoreHandle_t lock;
} transport_ctx_t;

static transport_ctx_t s_transport;

esp_err_t transport_init(void)
{
    if (s_transport.lock != NULL) {
        ESP_LOGD(TAG, "transport_init: already initialized");
        return ESP_OK;
    }

    memset(&s_transport, 0, sizeof(s_transport));
    s_transport.lock = xSemaphoreCreateMutex();
    if (s_transport.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "transport_init: mutex created");
    return ESP_OK;
}

static esp_err_t transport_open_locked(const transport_uart_config_t *cfg)
{
    ESP_LOGD(TAG,
             "open_locked: port=%d baud=%d tx=%d rx=%d parity=%d stop=%d data=%d flow=%d timeout_ms=%u",
             (int)cfg->uart_port,
             cfg->baud_rate,
             cfg->tx_pin,
             cfg->rx_pin,
             (int)cfg->parity,
             (int)cfg->stop_bits,
             (int)cfg->data_bits,
             (int)cfg->flow_ctrl,
             (unsigned)cfg->timeout_ms);

    uart_config_t uart_cfg = {
        .baud_rate = cfg->baud_rate,
        .data_bits = cfg->data_bits,
        .parity = cfg->parity,
        .stop_bits = cfg->stop_bits,
        .flow_ctrl = cfg->flow_ctrl,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(cfg->uart_port, &uart_cfg), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(cfg->uart_port, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(cfg->uart_port, UART_TRANSPORT_RX_BUF_SIZE, UART_TRANSPORT_TX_BUF_SIZE, 0, NULL, 0), TAG, "uart_driver_install failed");

    s_transport.status.is_open = true;
    s_transport.status.uart_port = cfg->uart_port;
    s_transport.status.baud_rate = cfg->baud_rate;
    s_transport.status.tx_pin = cfg->tx_pin;
    s_transport.status.rx_pin = cfg->rx_pin;
    s_transport.status.timeout_ms = cfg->timeout_ms;

    ESP_LOGI(TAG, "UART opened: port=%d baud=%d", (int)cfg->uart_port, cfg->baud_rate);
    return ESP_OK;
}

esp_err_t transport_open(const transport_uart_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_transport.lock, portMAX_DELAY);
    if (s_transport.status.is_open) {
        ESP_LOGD(TAG, "transport_open: already open on port=%d", (int)s_transport.status.uart_port);
        xSemaphoreGive(s_transport.lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = transport_open_locked(cfg);
    xSemaphoreGive(s_transport.lock);
    return err;
}

esp_err_t transport_switch(const transport_uart_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_transport.lock, portMAX_DELAY);
    if (s_transport.status.is_open) {
        ESP_LOGD(TAG, "transport_switch: closing current port=%d", (int)s_transport.status.uart_port);
        uart_driver_delete(s_transport.status.uart_port);
        s_transport.status.is_open = false;
    }

    esp_err_t err = transport_open_locked(cfg);
    xSemaphoreGive(s_transport.lock);
    return err;
}

esp_err_t transport_close(void)
{
    xSemaphoreTake(s_transport.lock, portMAX_DELAY);
    if (!s_transport.status.is_open) {
        ESP_LOGD(TAG, "transport_close: transport already closed");
        xSemaphoreGive(s_transport.lock);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "transport_close: closing port=%d", (int)s_transport.status.uart_port);

    esp_err_t err = uart_driver_delete(s_transport.status.uart_port);
    if (err != ESP_OK) {
        xSemaphoreGive(s_transport.lock);
        return err;
    }

    memset(&s_transport.status, 0, sizeof(s_transport.status));
    xSemaphoreGive(s_transport.lock);
    return ESP_OK;
}

esp_err_t transport_status_get(transport_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_transport.lock, portMAX_DELAY);
    *out_status = s_transport.status;
    ESP_LOGD(TAG,
             "status_get: is_open=%d port=%d baud=%d tx=%d rx=%d timeout_ms=%u",
             (int)out_status->is_open,
             (int)out_status->uart_port,
             out_status->baud_rate,
             out_status->tx_pin,
             out_status->rx_pin,
             (unsigned)out_status->timeout_ms);
    xSemaphoreGive(s_transport.lock);
    return ESP_OK;
}

esp_err_t transport_send_receive(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_max, size_t *rx_len)
{
    if (tx == NULL || tx_len == 0 || rx == NULL || rx_max == 0 || rx_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_transport.lock, portMAX_DELAY);
    if (!s_transport.status.is_open) {
        xSemaphoreGive(s_transport.lock);
        return ESP_ERR_INVALID_STATE;
    }

    uart_port_t port = s_transport.status.uart_port;
    uint32_t timeout_ms = s_transport.status.timeout_ms > 0 ? s_transport.status.timeout_ms : 500;
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    ESP_LOGD(TAG,
             "send_receive: port=%d tx_len=%u rx_max=%u timeout_ms=%u",
             (int)port,
             (unsigned)tx_len,
             (unsigned)rx_max,
             (unsigned)timeout_ms);
    transport_log_hex_debug("tx", tx, tx_len);

    uart_flush_input(port);
    int written = uart_write_bytes(port, tx, tx_len);
    if (written != (int)tx_len) {
        xSemaphoreGive(s_transport.lock);
        return ESP_FAIL;
    }

    if (uart_wait_tx_done(port, timeout) != ESP_OK) {
        xSemaphoreGive(s_transport.lock);
        return ESP_ERR_TIMEOUT;
    }

    int read = uart_read_bytes(port, rx, rx_max, timeout);
    if (read < 0) {
        xSemaphoreGive(s_transport.lock);
        return ESP_FAIL;
    }

    *rx_len = (size_t)read;
    transport_log_hex_debug("rx", rx, *rx_len);

    xSemaphoreGive(s_transport.lock);
    return ESP_OK;
}
