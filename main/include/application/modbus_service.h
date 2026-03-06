#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// API/Application-level limits and DTOs.
#define APP_MODBUS_MAX_REG_VALUES 125

typedef enum {
    APP_UART_PORT_0 = 0,
    APP_UART_PORT_1 = 1,
    APP_UART_PORT_2 = 2,
} app_uart_port_t;

typedef struct {
    app_uart_port_t uart_port;
    int baud_rate;
    int tx_pin;
    int rx_pin;
    uint32_t timeout_ms;
} app_transport_uart_config_t;

typedef struct {
    bool is_open;
    app_uart_port_t uart_port;
    int baud_rate;
    int tx_pin;
    int rx_pin;
    uint32_t timeout_ms;
} app_transport_status_t;

typedef struct {
    uint8_t slave_id;
    uint16_t address;
    uint16_t count;
} app_read_input_t;

typedef struct {
    uint8_t slave_id;
    uint16_t address;
    uint16_t value;
} app_write_input_t;

typedef struct {
    uint8_t slave_id;
    uint16_t address;
    uint16_t count;
    uint16_t values[APP_MODBUS_MAX_REG_VALUES];
} app_write_group_input_t;

typedef struct {
    uint8_t slave_id;
    uint16_t address;
    uint16_t count;
    uint8_t function;
    uint16_t values[APP_MODBUS_MAX_REG_VALUES];
    bool ok;
} app_read_response_t;

typedef struct {
    bool ok;
    uint8_t slave_id;
    uint16_t address;
    uint16_t count;
    uint8_t function;
} app_write_response_t;

esp_err_t app_service_init(void);

esp_err_t app_transport_open(const app_transport_uart_config_t *cfg);
esp_err_t app_transport_switch(const app_transport_uart_config_t *cfg);
esp_err_t app_transport_close(void);
esp_err_t app_transport_status(app_transport_status_t *status);

esp_err_t app_modbus_read(const app_read_input_t *input, app_read_response_t *out_response);
esp_err_t app_modbus_read_group(const app_read_input_t *input, app_read_response_t *out_response);
esp_err_t app_modbus_write(const app_write_input_t *input, app_write_response_t *out_response);
esp_err_t app_modbus_write_group(const app_write_group_input_t *input, app_write_response_t *out_response);

#ifdef __cplusplus
}
#endif
