#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODBUS_MAX_REG_VALUES 125
#define MODBUS_MAX_ADU_SIZE 256

typedef enum {
    MODBUS_FUNCTION_READ_HOLDING = 0x03,
    MODBUS_FUNCTION_WRITE_SINGLE = 0x06,
    MODBUS_FUNCTION_WRITE_MULTIPLE = 0x10,
} modbus_function_t;

typedef struct {
    uint8_t slave_id;
    uint16_t address;
    uint16_t count;
    modbus_function_t function;
} modbus_read_request_t;

typedef struct {
    uint8_t slave_id;
    uint16_t address;
    uint16_t value;
} modbus_write_request_t;

typedef struct {
    uint8_t slave_id;
    uint16_t address;
    uint16_t count;
    const uint16_t *values;
} modbus_write_group_request_t;

typedef struct {
    uint8_t slave_id;
    uint16_t address;
    uint16_t count;
    modbus_function_t function;
    uint16_t values[MODBUS_MAX_REG_VALUES];
    bool ok;
} modbus_read_response_t;

typedef struct {
    bool ok;
    uint8_t slave_id;
    uint16_t address;
    uint16_t count;
    modbus_function_t function;
} modbus_write_response_t;

esp_err_t modbus_parse_address(const char *address_text, uint16_t *out_address);
size_t modbus_build_read_adu(const modbus_read_request_t *request, uint8_t *out_adu, size_t out_size);
size_t modbus_build_write_single_adu(const modbus_write_request_t *request, uint8_t *out_adu, size_t out_size);
size_t modbus_build_write_group_adu(const modbus_write_group_request_t *request, uint8_t *out_adu, size_t out_size);
esp_err_t modbus_parse_read_response(const uint8_t *adu, size_t adu_len, const modbus_read_request_t *request, modbus_read_response_t *out_response);
esp_err_t modbus_parse_write_response(const uint8_t *adu, size_t adu_len, modbus_function_t function, modbus_write_response_t *out_response);

#ifdef __cplusplus
}
#endif
