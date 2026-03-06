#include "api/json_rpc_server.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "application/modbus_service.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "json_rpc_api";

static cJSON *json_rpc_result_base(cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    return resp;
}

static cJSON *json_rpc_error(cJSON *id, int code, const char *message)
{
    cJSON *resp = json_rpc_result_base(id);
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return resp;
}

static cJSON *json_rpc_ok(cJSON *id, cJSON *result)
{
    cJSON *resp = json_rpc_result_base(id);
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    if (text == NULL) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    cJSON_Delete(json);
    return err;
}

static esp_err_t parse_transport_cfg(cJSON *params, transport_uart_config_t *cfg)
{
    if (params == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->parity = UART_PARITY_DISABLE;
    cfg->stop_bits = UART_STOP_BITS_1;
    cfg->data_bits = UART_DATA_8_BITS;
    cfg->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg->timeout_ms = 500;

    cJSON *port = cJSON_GetObjectItemCaseSensitive(params, "uart_port");
    cJSON *baud = cJSON_GetObjectItemCaseSensitive(params, "baud_rate");
    cJSON *tx = cJSON_GetObjectItemCaseSensitive(params, "tx_pin");
    cJSON *rx = cJSON_GetObjectItemCaseSensitive(params, "rx_pin");

    if (!cJSON_IsNumber(port) || !cJSON_IsNumber(baud) || !cJSON_IsNumber(tx) || !cJSON_IsNumber(rx)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *timeout_ms = cJSON_GetObjectItemCaseSensitive(params, "timeout_ms");

    cfg->uart_port = (uart_port_t)port->valueint;
    cfg->baud_rate = baud->valueint;
    cfg->tx_pin = tx->valueint;
    cfg->rx_pin = rx->valueint;
    if (cJSON_IsNumber(timeout_ms)) {
        cfg->timeout_ms = (uint32_t)timeout_ms->valueint;
    }

    return ESP_OK;
}

static esp_err_t parse_address_field(cJSON *params, const char *field, uint16_t *out)
{
    cJSON *addr = cJSON_GetObjectItemCaseSensitive(params, field);
    if (cJSON_IsString(addr) && addr->valuestring != NULL) {
        return modbus_parse_address(addr->valuestring, out);
    }
    if (cJSON_IsNumber(addr)) {
        if (addr->valueint < 0 || addr->valueint > UINT16_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        *out = (uint16_t)addr->valueint;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static cJSON *handle_method(cJSON *id, const char *method, cJSON *params)
{
    if (strcmp(method, "ping") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "ok");
        return json_rpc_ok(id, result);
    }

    if (strcmp(method, "transport.status") == 0) {
        transport_status_t status = {0};
        if (app_transport_status(&status) != ESP_OK) {
            return json_rpc_error(id, -32001, "transport status error");
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "is_open", status.is_open);
        cJSON_AddNumberToObject(result, "uart_port", status.uart_port);
        cJSON_AddNumberToObject(result, "baud_rate", status.baud_rate);
        cJSON_AddNumberToObject(result, "tx_pin", status.tx_pin);
        cJSON_AddNumberToObject(result, "rx_pin", status.rx_pin);
        cJSON_AddNumberToObject(result, "timeout_ms", status.timeout_ms);
        return json_rpc_ok(id, result);
    }

    if (strcmp(method, "transport.serial_ports") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON *ports = cJSON_AddArrayToObject(result, "ports");
        cJSON_AddItemToArray(ports, cJSON_CreateNumber(UART_NUM_0));
        cJSON_AddItemToArray(ports, cJSON_CreateNumber(UART_NUM_1));
        cJSON_AddItemToArray(ports, cJSON_CreateNumber(UART_NUM_2));
        return json_rpc_ok(id, result);
    }

    if (strcmp(method, "transport.open") == 0 || strcmp(method, "transport.switch") == 0) {
        transport_uart_config_t cfg;
        if (parse_transport_cfg(params, &cfg) != ESP_OK) {
            return json_rpc_error(id, -32602, "invalid transport parameters");
        }
        esp_err_t err = (strcmp(method, "transport.open") == 0) ? app_transport_open(&cfg) : app_transport_switch(&cfg);
        if (err != ESP_OK) {
            return json_rpc_error(id, -32002, esp_err_to_name(err));
        }
        return json_rpc_ok(id, cJSON_CreateTrue());
    }

    if (strcmp(method, "transport.close") == 0) {
        esp_err_t err = app_transport_close();
        if (err != ESP_OK) {
            return json_rpc_error(id, -32003, esp_err_to_name(err));
        }
        return json_rpc_ok(id, cJSON_CreateTrue());
    }

    if (strcmp(method, "modbus.read") == 0 || strcmp(method, "modbus.read_group") == 0) {
        app_read_input_t input = {0};
        cJSON *slave_id = cJSON_GetObjectItemCaseSensitive(params, "slave_id");
        cJSON *count = cJSON_GetObjectItemCaseSensitive(params, "count");
        if (!cJSON_IsNumber(slave_id) || !cJSON_IsNumber(count) || parse_address_field(params, "address", &input.address) != ESP_OK) {
            return json_rpc_error(id, -32602, "invalid modbus.read parameters");
        }

        input.slave_id = (uint8_t)slave_id->valueint;
        input.count = (uint16_t)count->valueint;

        modbus_read_response_t response = {0};
        esp_err_t err = (strcmp(method, "modbus.read") == 0)
                            ? app_modbus_read(&input, &response)
                            : app_modbus_read_group(&input, &response);

        if (err != ESP_OK) {
            return json_rpc_error(id, -32010, esp_err_to_name(err));
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "slave_id", response.slave_id);
        cJSON_AddNumberToObject(result, "address", response.address);
        cJSON_AddNumberToObject(result, "count", response.count);
        cJSON_AddNumberToObject(result, "function", response.function);
        cJSON_AddBoolToObject(result, "ok", response.ok);

        cJSON *values = cJSON_AddArrayToObject(result, "values");
        for (uint16_t i = 0; i < response.count; i++) {
            cJSON_AddItemToArray(values, cJSON_CreateNumber(response.values[i]));
        }

        return json_rpc_ok(id, result);
    }

    if (strcmp(method, "modbus.write") == 0) {
        app_write_input_t input = {0};
        cJSON *slave_id = cJSON_GetObjectItemCaseSensitive(params, "slave_id");
        cJSON *value = cJSON_GetObjectItemCaseSensitive(params, "value");
        if (!cJSON_IsNumber(slave_id) || !cJSON_IsNumber(value) || parse_address_field(params, "address", &input.address) != ESP_OK) {
            return json_rpc_error(id, -32602, "invalid modbus.write parameters");
        }

        input.slave_id = (uint8_t)slave_id->valueint;
        input.value = (uint16_t)value->valueint;

        modbus_write_response_t response = {0};
        esp_err_t err = app_modbus_write(&input, &response);
        if (err != ESP_OK) {
            return json_rpc_error(id, -32011, esp_err_to_name(err));
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "slave_id", response.slave_id);
        cJSON_AddNumberToObject(result, "address", response.address);
        cJSON_AddNumberToObject(result, "count", response.count);
        cJSON_AddNumberToObject(result, "function", response.function);
        cJSON_AddBoolToObject(result, "ok", response.ok);
        return json_rpc_ok(id, result);
    }

    if (strcmp(method, "modbus.write_group") == 0) {
        app_write_group_input_t input = {0};
        cJSON *slave_id = cJSON_GetObjectItemCaseSensitive(params, "slave_id");
        cJSON *values = cJSON_GetObjectItemCaseSensitive(params, "values");
        if (!cJSON_IsNumber(slave_id) || !cJSON_IsArray(values) || parse_address_field(params, "address", &input.address) != ESP_OK) {
            return json_rpc_error(id, -32602, "invalid modbus.write_group parameters");
        }

        input.slave_id = (uint8_t)slave_id->valueint;
        input.count = (uint16_t)cJSON_GetArraySize(values);
        if (input.count == 0 || input.count > MODBUS_MAX_REG_VALUES) {
            return json_rpc_error(id, -32602, "invalid values count");
        }

        for (uint16_t i = 0; i < input.count; i++) {
            cJSON *entry = cJSON_GetArrayItem(values, i);
            if (!cJSON_IsNumber(entry)) {
                return json_rpc_error(id, -32602, "values must contain only numbers");
            }
            input.values[i] = (uint16_t)entry->valueint;
        }

        modbus_write_response_t response = {0};
        esp_err_t err = app_modbus_write_group(&input, &response);
        if (err != ESP_OK) {
            return json_rpc_error(id, -32012, esp_err_to_name(err));
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "slave_id", response.slave_id);
        cJSON_AddNumberToObject(result, "address", response.address);
        cJSON_AddNumberToObject(result, "count", response.count);
        cJSON_AddNumberToObject(result, "function", response.function);
        cJSON_AddBoolToObject(result, "ok", response.ok);
        return json_rpc_ok(id, result);
    }

    return json_rpc_error(id, -32601, "method not found");
}

static esp_err_t rpc_handler(httpd_req_t *req)
{
    size_t len = req->content_len;
    char *buf = calloc(1, len + 1);
    if (buf == NULL) {
        return httpd_resp_send_500(req);
    }

    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) {
        free(buf);
        return httpd_resp_send_500(req);
    }

    cJSON *root = cJSON_ParseWithLength(buf, len);
    free(buf);

    if (root == NULL) {
        return send_json(req, json_rpc_error(cJSON_CreateNull(), -32700, "parse error"));
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");

    if (id == NULL) {
        id = cJSON_CreateNull();
    }
    if (!cJSON_IsString(method) || method->valuestring == NULL) {
        cJSON_Delete(root);
        return send_json(req, json_rpc_error(id, -32600, "invalid request"));
    }
    cJSON *local_params = NULL;
    if (params == NULL) {
        local_params = cJSON_CreateObject();
        params = local_params;
    }

    cJSON *response = handle_method(id, method->valuestring, params);
    cJSON_Delete(local_params);
    cJSON_Delete(root);
    return send_json(req, response);
}

esp_err_t json_rpc_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8080;

    httpd_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&handle, &cfg), TAG, "httpd_start failed");

    httpd_uri_t uri = {
        .uri = "/rpc",
        .method = HTTP_POST,
        .handler = rpc_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(handle, &uri), TAG, "uri register failed");

    ESP_LOGI(TAG, "JSON-RPC server started on port %d", cfg.server_port);
    return ESP_OK;
}
