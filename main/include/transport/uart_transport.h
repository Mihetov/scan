#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool is_open;
    uart_port_t uart_port;
    int baud_rate;
    int tx_pin;
    int rx_pin;
    uint32_t timeout_ms;
} transport_status_t;

typedef struct {
    uart_port_t uart_port;
    int baud_rate;
    int tx_pin;
    int rx_pin;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_word_length_t data_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint32_t timeout_ms;
} transport_uart_config_t;

esp_err_t transport_init(void);
esp_err_t transport_open(const transport_uart_config_t *cfg);
esp_err_t transport_switch(const transport_uart_config_t *cfg);
esp_err_t transport_close(void);
esp_err_t transport_status_get(transport_status_t *out_status);
esp_err_t transport_send_receive(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_max, size_t *rx_len);

#ifdef __cplusplus
}
#endif
