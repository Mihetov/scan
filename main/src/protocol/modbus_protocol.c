#include "protocol/modbus_protocol.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

esp_err_t modbus_parse_address(const char *address_text, uint16_t *out_address)
{
    if (address_text == NULL || out_address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int base = 10;
    if (strlen(address_text) > 2 && address_text[0] == '0' && (address_text[1] == 'x' || address_text[1] == 'X')) {
        base = 16;
    }

    char *end = NULL;
    long value = strtol(address_text, &end, base);
    if (*end != '\0' || value < 0 || value > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_address = (uint16_t)value;
    return ESP_OK;
}

static size_t modbus_finalize_adu(uint8_t *adu, size_t payload_len)
{
    uint16_t crc = modbus_crc16(adu, payload_len);
    adu[payload_len] = (uint8_t)(crc & 0xFF);
    adu[payload_len + 1] = (uint8_t)(crc >> 8);
    return payload_len + 2;
}

size_t modbus_build_read_adu(const modbus_read_request_t *request, uint8_t *out_adu, size_t out_size)
{
    if (request == NULL || out_adu == NULL || out_size < 8) {
        return 0;
    }

    out_adu[0] = request->slave_id;
    out_adu[1] = (uint8_t)request->function;
    out_adu[2] = (uint8_t)(request->address >> 8);
    out_adu[3] = (uint8_t)(request->address & 0xFF);
    out_adu[4] = (uint8_t)(request->count >> 8);
    out_adu[5] = (uint8_t)(request->count & 0xFF);
    return modbus_finalize_adu(out_adu, 6);
}

size_t modbus_build_write_single_adu(const modbus_write_request_t *request, uint8_t *out_adu, size_t out_size)
{
    if (request == NULL || out_adu == NULL || out_size < 8) {
        return 0;
    }

    out_adu[0] = request->slave_id;
    out_adu[1] = MODBUS_FUNCTION_WRITE_SINGLE;
    out_adu[2] = (uint8_t)(request->address >> 8);
    out_adu[3] = (uint8_t)(request->address & 0xFF);
    out_adu[4] = (uint8_t)(request->value >> 8);
    out_adu[5] = (uint8_t)(request->value & 0xFF);
    return modbus_finalize_adu(out_adu, 6);
}

size_t modbus_build_write_group_adu(const modbus_write_group_request_t *request, uint8_t *out_adu, size_t out_size)
{
    if (request == NULL || out_adu == NULL || request->values == NULL || request->count == 0 || request->count > MODBUS_MAX_REG_VALUES) {
        return 0;
    }

    size_t payload_len = 7 + ((size_t)request->count * 2);
    if (out_size < payload_len + 2) {
        return 0;
    }

    out_adu[0] = request->slave_id;
    out_adu[1] = MODBUS_FUNCTION_WRITE_MULTIPLE;
    out_adu[2] = (uint8_t)(request->address >> 8);
    out_adu[3] = (uint8_t)(request->address & 0xFF);
    out_adu[4] = (uint8_t)(request->count >> 8);
    out_adu[5] = (uint8_t)(request->count & 0xFF);
    out_adu[6] = (uint8_t)(request->count * 2);

    for (uint16_t i = 0; i < request->count; i++) {
        out_adu[7 + (i * 2)] = (uint8_t)(request->values[i] >> 8);
        out_adu[8 + (i * 2)] = (uint8_t)(request->values[i] & 0xFF);
    }

    return modbus_finalize_adu(out_adu, payload_len);
}

static bool modbus_crc_valid(const uint8_t *adu, size_t adu_len)
{
    if (adu_len < 4) {
        return false;
    }
    uint16_t expected = modbus_crc16(adu, adu_len - 2);
    uint16_t got = (uint16_t)adu[adu_len - 2] | ((uint16_t)adu[adu_len - 1] << 8);
    return expected == got;
}

esp_err_t modbus_parse_read_response(const uint8_t *adu, size_t adu_len, const modbus_read_request_t *request, modbus_read_response_t *out_response)
{
    if (adu == NULL || request == NULL || out_response == NULL || adu_len < 5) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_response, 0, sizeof(*out_response));
    out_response->slave_id = request->slave_id;
    out_response->address = request->address;
    out_response->count = request->count;
    out_response->function = request->function;

    if (!modbus_crc_valid(adu, adu_len)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (adu[0] != request->slave_id || adu[1] != (uint8_t)request->function) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t byte_count = adu[2];
    if (byte_count != request->count * 2 || adu_len < (size_t)(byte_count + 5)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint16_t i = 0; i < request->count; i++) {
        out_response->values[i] = ((uint16_t)adu[3 + (i * 2)] << 8) | adu[4 + (i * 2)];
    }
    out_response->ok = true;
    return ESP_OK;
}

esp_err_t modbus_parse_write_response(const uint8_t *adu, size_t adu_len, modbus_function_t function, modbus_write_response_t *out_response)
{
    if (adu == NULL || out_response == NULL || adu_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!modbus_crc_valid(adu, adu_len)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out_response, 0, sizeof(*out_response));
    out_response->slave_id = adu[0];
    out_response->function = (modbus_function_t)adu[1];
    out_response->address = ((uint16_t)adu[2] << 8) | adu[3];
    out_response->count = ((uint16_t)adu[4] << 8) | adu[5];
    out_response->ok = out_response->function == function;

    return out_response->ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
