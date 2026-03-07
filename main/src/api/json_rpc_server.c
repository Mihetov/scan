#include "api/json_rpc_server.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "application/modbus_service.h"
#include "application/wifi_service.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "protocol/modbus_protocol.h"

static const char *TAG = "json_rpc_api";

static void json_rpc_log_error_debug(const char *stage, esp_err_t err)
{
    ESP_LOGD(TAG, "%s failed: %s (0x%x)", stage, esp_err_to_name(err), (unsigned)err);
}

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
    // === CORS заголовки для всех ответов ===
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

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

static esp_err_t parse_transport_cfg(cJSON *params, app_transport_uart_config_t *cfg)
{
    if (params == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->timeout_ms = 500;

    cJSON *port = cJSON_GetObjectItemCaseSensitive(params, "uart_port");
    cJSON *baud = cJSON_GetObjectItemCaseSensitive(params, "baud_rate");
    cJSON *tx = cJSON_GetObjectItemCaseSensitive(params, "tx_pin");
    cJSON *rx = cJSON_GetObjectItemCaseSensitive(params, "rx_pin");

    if (!cJSON_IsNumber(port) || !cJSON_IsNumber(baud) || !cJSON_IsNumber(tx) || !cJSON_IsNumber(rx)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (port->valueint < APP_UART_PORT_0 || port->valueint > APP_UART_PORT_2 || baud->valueint <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *timeout_ms = cJSON_GetObjectItemCaseSensitive(params, "timeout_ms");

    cfg->uart_port = (app_uart_port_t)port->valueint;
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
        esp_err_t err = modbus_parse_address(addr->valuestring, out);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "parse_address_field: field=%s text=%s err=%s", field, addr->valuestring, esp_err_to_name(err));
        } else {
            ESP_LOGD(TAG, "parse_address_field: field=%s parsed=0x%04X", field, *out);
        }
        return err;
    }
    if (cJSON_IsNumber(addr)) {
        if (addr->valueint < 0 || addr->valueint > UINT16_MAX) {
            ESP_LOGD(TAG, "parse_address_field: field=%s number out of range=%d", field, addr->valueint);
            return ESP_ERR_INVALID_ARG;
        }
        *out = (uint16_t)addr->valueint;
        ESP_LOGD(TAG, "parse_address_field: field=%s parsed=0x%04X", field, *out);
        return ESP_OK;
    }
    ESP_LOGD(TAG, "parse_address_field: field=%s missing or invalid type", field);
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t parse_wifi_sta_config(cJSON *params, app_wifi_sta_config_t *out)
{
    if (params == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(params, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(params, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring == NULL || !cJSON_IsString(password) || password->valuestring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid->valuestring);
    size_t pass_len = strlen(password->valuestring);
    if (ssid_len == 0 || ssid_len >= sizeof(out->ssid) || pass_len >= sizeof(out->password)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->ssid, ssid->valuestring, ssid_len);
    memcpy(out->password, password->valuestring, pass_len);
    return ESP_OK;
}

static esp_err_t parse_wifi_ap_config(cJSON *params, app_wifi_ap_config_t *out)
{
    if (params == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(params, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(params, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring == NULL || !cJSON_IsString(password) || password->valuestring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid->valuestring);
    size_t pass_len = strlen(password->valuestring);
    if (ssid_len == 0 || ssid_len >= sizeof(out->ssid) || pass_len >= sizeof(out->password)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->ssid, ssid->valuestring, ssid_len);
    memcpy(out->password, password->valuestring, pass_len);

    cJSON *channel = cJSON_GetObjectItemCaseSensitive(params, "channel");
    cJSON *max_conn = cJSON_GetObjectItemCaseSensitive(params, "max_connection");
    out->channel = (cJSON_IsNumber(channel) && channel->valueint > 0 && channel->valueint <= 13) ? (uint8_t)channel->valueint : 1;
    out->max_connection = (cJSON_IsNumber(max_conn) && max_conn->valueint > 0 && max_conn->valueint <= 10) ? (uint8_t)max_conn->valueint : 4;

    return ESP_OK;
}

static cJSON *handle_method(cJSON *id, const char *method, cJSON *params)
{
    ESP_LOGD(TAG, "handle_method: method=%s", method);
    if (strcmp(method, "ping") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "ok");
        return json_rpc_ok(id, result);
    }

    if (strcmp(method, "transport.status") == 0) {
        app_transport_status_t status = {0};
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
        app_transport_uart_config_t cfg;
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

    if (strcmp(method, "wifi.status") == 0) {
        app_wifi_status_t status = {0};
        esp_err_t err = app_wifi_get_status(&status);
        if (err != ESP_OK) {
            return json_rpc_error(id, -32020, esp_err_to_name(err));
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "active_mode", status.active_mode);
        cJSON_AddBoolToObject(result, "sta_connected", status.sta_connected);
        cJSON_AddNumberToObject(result, "last_sta_disconnect_reason", status.last_sta_disconnect_reason);
        cJSON *sta = cJSON_AddObjectToObject(result, "sta");
        cJSON_AddStringToObject(sta, "ssid", status.sta.ssid);
        cJSON *ap = cJSON_AddObjectToObject(result, "ap");
        cJSON_AddStringToObject(ap, "ssid", status.ap.ssid);
        cJSON_AddNumberToObject(ap, "channel", status.ap.channel);
        cJSON_AddNumberToObject(ap, "max_connection", status.ap.max_connection);
        return json_rpc_ok(id, result);
    }

    if (strcmp(method, "wifi.set_sta") == 0) {
        app_wifi_sta_config_t cfg = {0};
        if (parse_wifi_sta_config(params, &cfg) != ESP_OK) {
            return json_rpc_error(id, -32602, "invalid wifi.set_sta parameters");
        }

        esp_err_t err = app_wifi_set_sta_config(&cfg);
        if (err != ESP_OK) {
            return json_rpc_error(id, -32021, esp_err_to_name(err));
        }

        return json_rpc_ok(id, cJSON_CreateTrue());
    }

    if (strcmp(method, "wifi.set_ap") == 0) {
        app_wifi_ap_config_t cfg = {0};
        if (parse_wifi_ap_config(params, &cfg) != ESP_OK) {
            return json_rpc_error(id, -32602, "invalid wifi.set_ap parameters");
        }

        esp_err_t err = app_wifi_set_ap_config(&cfg);
        if (err != ESP_OK) {
            return json_rpc_error(id, -32022, esp_err_to_name(err));
        }

        return json_rpc_ok(id, cJSON_CreateTrue());
    }

    if (strcmp(method, "wifi.apply") == 0) {
        esp_err_t err = app_wifi_apply();
        if (err != ESP_OK) {
            return json_rpc_error(id, -32023, esp_err_to_name(err));
        }
        return json_rpc_ok(id, cJSON_CreateTrue());
    }

    if (strcmp(method, "modbus.read") == 0 || strcmp(method, "modbus.read_group") == 0) {
        app_read_input_t input = {0};
        cJSON *slave_id = cJSON_GetObjectItemCaseSensitive(params, "slave_id");
        cJSON *count = cJSON_GetObjectItemCaseSensitive(params, "count");
        esp_err_t parse_addr_err = parse_address_field(params, "address", &input.address);
        if (!cJSON_IsNumber(slave_id) || !cJSON_IsNumber(count) || parse_addr_err != ESP_OK) {
            ESP_LOGD(TAG, "modbus.read invalid params: slave_id_is_num=%d count_is_num=%d parse_addr_err=%s",
                     cJSON_IsNumber(slave_id), cJSON_IsNumber(count), esp_err_to_name(parse_addr_err));
            return json_rpc_error(id, -32602, "invalid modbus.read parameters");
        }
        if (slave_id->valueint < 0 || slave_id->valueint > UINT8_MAX || count->valueint <= 0 || count->valueint > APP_MODBUS_MAX_REG_VALUES) {
            return json_rpc_error(id, -32602, "invalid slave_id/count range");
        }

        input.slave_id = (uint8_t)slave_id->valueint;
        input.count = (uint16_t)count->valueint;
      
        ESP_LOGD(TAG, "modbus.read request: slave_id=%u address=0x%04X count=%u",
                 input.slave_id, input.address, input.count);

        app_read_response_t response = {0};
        esp_err_t err = (strcmp(method, "modbus.read") == 0)
                            ? app_modbus_read(&input, &response)
                            : app_modbus_read_group(&input, &response);

        if (err != ESP_OK) {
            json_rpc_log_error_debug("app_modbus_read", err);
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
        if (slave_id->valueint < 0 || slave_id->valueint > UINT8_MAX || value->valueint < 0 || value->valueint > UINT16_MAX) {
            return json_rpc_error(id, -32602, "invalid slave_id/value range");
        }

        input.slave_id = (uint8_t)slave_id->valueint;
        input.value = (uint16_t)value->valueint;

        app_write_response_t response = {0};
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
        if (slave_id->valueint < 0 || slave_id->valueint > UINT8_MAX) {
            return json_rpc_error(id, -32602, "invalid slave_id range");
        }

        input.slave_id = (uint8_t)slave_id->valueint;
        input.count = (uint16_t)cJSON_GetArraySize(values);
        if (input.count == 0 || input.count > APP_MODBUS_MAX_REG_VALUES) {
            return json_rpc_error(id, -32602, "invalid values count");
        }

        for (uint16_t i = 0; i < input.count; i++) {
            cJSON *entry = cJSON_GetArrayItem(values, i);
            if (!cJSON_IsNumber(entry)) {
                return json_rpc_error(id, -32602, "values must contain only numbers");
            }
            if (entry->valueint < 0 || entry->valueint > UINT16_MAX) {
                return json_rpc_error(id, -32602, "values out of uint16 range");
            }
            input.values[i] = (uint16_t)entry->valueint;
        }

        app_write_response_t response = {0};
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

    size_t total = 0;
    while (total < len) {
        int received = httpd_req_recv(req, buf + total, len - total);
        if (received <= 0) {
            free(buf);
            return httpd_resp_send_500(req);
        }
        total += (size_t)received;
    }

    ESP_LOGD(TAG, "rpc_handler: request bytes=%u payload=%.*s", (unsigned)len, (int)len, buf);

    cJSON *root = cJSON_ParseWithLength(buf, len);
    free(buf);

    if (root == NULL) {
        return send_json(req, json_rpc_error(cJSON_CreateNull(), -32700, "parse error"));
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    bool id_is_local = false;
    cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");

    if (id == NULL) {
        id = cJSON_CreateNull();
        id_is_local = true;
    }
    if (!cJSON_IsString(method) || method->valuestring == NULL) {
        cJSON_Delete(root);
        cJSON *response = json_rpc_error(id, -32600, "invalid request");
        if (id_is_local) {
            cJSON_Delete(id);
        }
        return send_json(req, response);
    }
    cJSON *local_params = NULL;
    if (params == NULL) {
        local_params = cJSON_CreateObject();
        params = local_params;
    }

    cJSON *response = handle_method(id, method->valuestring, params);
    if (id_is_local) {
        cJSON_Delete(id);
    }
    cJSON_Delete(local_params);
    cJSON_Delete(root);
    return send_json(req, response);
}

static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");

    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t json_rpc_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8080;

    httpd_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&handle, &cfg), TAG, "httpd_start failed");

    // POST handler (уже был)
    httpd_uri_t uri_post = {
        .uri = "/rpc",
        .method = HTTP_POST,
        .handler = rpc_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(handle, &uri_post), TAG, "uri register failed");

    // === НОВОЕ: OPTIONS handler для CORS ===
    httpd_uri_t uri_options = {
    .uri = "/rpc",
    .method = HTTP_OPTIONS,
    .handler = options_handler,
    .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(handle, &uri_options), TAG, "uri options register failed");

    ESP_LOGI(TAG, "JSON-RPC server started on port %d", cfg.server_port);
    return ESP_OK;
}
