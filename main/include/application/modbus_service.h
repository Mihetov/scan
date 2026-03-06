#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol/modbus_protocol.h"
#include "transport/uart_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    uint16_t values[MODBUS_MAX_REG_VALUES];
} app_write_group_input_t;

esp_err_t app_service_init(void);

esp_err_t app_transport_open(const transport_uart_config_t *cfg);
esp_err_t app_transport_switch(const transport_uart_config_t *cfg);
esp_err_t app_transport_close(void);
esp_err_t app_transport_status(transport_status_t *status);

esp_err_t app_modbus_read(const app_read_input_t *input, modbus_read_response_t *out_response);
esp_err_t app_modbus_read_group(const app_read_input_t *input, modbus_read_response_t *out_response);
esp_err_t app_modbus_write(const app_write_input_t *input, modbus_write_response_t *out_response);
esp_err_t app_modbus_write_group(const app_write_group_input_t *input, modbus_write_response_t *out_response);

#ifdef __cplusplus
}
#endif
