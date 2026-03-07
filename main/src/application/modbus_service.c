#include "application/modbus_service.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "protocol/modbus_protocol.h"
#include "transport/uart_transport.h"

typedef enum {
    APP_OP_READ = 0,
    APP_OP_WRITE_SINGLE,
    APP_OP_WRITE_GROUP,
} app_op_t;

typedef struct {
    app_op_t op;
    app_read_input_t read;
    app_write_input_t write;
    app_write_group_input_t write_group;
    SemaphoreHandle_t done;
    esp_err_t result;
    modbus_read_response_t read_response;
    modbus_write_response_t write_response;
} app_job_t;

static const char *TAG = "modbus_service";

static QueueHandle_t s_queue;


static const char *app_op_to_str(app_op_t op)
{
    switch (op) {
        case APP_OP_READ:
            return "read";
        case APP_OP_WRITE_SINGLE:
            return "write_single";
        case APP_OP_WRITE_GROUP:
            return "write_group";
        default:
            return "unknown";
    }
}
static void app_fill_read_response(const modbus_read_response_t *src, app_read_response_t *dst)
{
    dst->slave_id = src->slave_id;
    dst->address = src->address;
    dst->count = src->count;
    dst->function = (uint8_t)src->function;
    dst->ok = src->ok;
    memcpy(dst->values, src->values, sizeof(uint16_t) * src->count);
}

static void app_fill_write_response(const modbus_write_response_t *src, app_write_response_t *dst)
{
    dst->ok = src->ok;
    dst->slave_id = src->slave_id;
    dst->address = src->address;
    dst->count = src->count;
    dst->function = (uint8_t)src->function;
}

static esp_err_t app_convert_transport_cfg(const app_transport_uart_config_t *in, transport_uart_config_t *out)
{
    if (in == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (in->uart_port < APP_UART_PORT_0 || in->uart_port > APP_UART_PORT_2 || in->baud_rate <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out->uart_port = (uart_port_t)in->uart_port;
    out->baud_rate = in->baud_rate;
    out->tx_pin = in->tx_pin;
    out->rx_pin = in->rx_pin;
    out->timeout_ms = in->timeout_ms;
    out->parity = UART_PARITY_DISABLE;
    out->stop_bits = UART_STOP_BITS_1;
    out->data_bits = UART_DATA_8_BITS;
    out->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    return ESP_OK;
}

static esp_err_t app_execute_read(const app_read_input_t *input, modbus_read_response_t *out)
{
    modbus_read_request_t request = {
        .slave_id = input->slave_id,
        .address = input->address,
        .count = input->count,
        .function = MODBUS_FUNCTION_READ_HOLDING,
    };

    uint8_t tx[MODBUS_MAX_ADU_SIZE] = {0};
    uint8_t rx[MODBUS_MAX_ADU_SIZE] = {0};
    size_t tx_len = modbus_build_read_adu(&request, tx, sizeof(tx));
    size_t rx_len = 0;

    if (tx_len == 0) {
        ESP_LOGD(TAG, "app_execute_read: modbus_build_read_adu returned 0 slave=%u addr=0x%04X count=%u",
                 request.slave_id, request.address, request.count);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "app_execute_read: slave=%u addr=0x%04X count=%u tx_len=%u",
             request.slave_id, request.address, request.count, (unsigned)tx_len);

    esp_err_t err = transport_send_receive(tx, tx_len, rx, sizeof(rx), &rx_len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "app_execute_read: transport_send_receive failed: %s", esp_err_to_name(err));
        return err;
    }

    err = modbus_parse_read_response(rx, rx_len, &request, out);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "app_execute_read: modbus_parse_read_response failed: %s (rx_len=%u)",
                 esp_err_to_name(err), (unsigned)rx_len);
        return err;
    }

    return ESP_OK;
}

static esp_err_t app_execute_write_single(const app_write_input_t *input, modbus_write_response_t *out)
{
    modbus_write_request_t request = {
        .slave_id = input->slave_id,
        .address = input->address,
        .value = input->value,
    };

    uint8_t tx[MODBUS_MAX_ADU_SIZE] = {0};
    uint8_t rx[MODBUS_MAX_ADU_SIZE] = {0};
    size_t tx_len = modbus_build_write_single_adu(&request, tx, sizeof(tx));
    size_t rx_len = 0;

    if (tx_len == 0) {
        ESP_LOGD(TAG, "app_execute_write_single: modbus_build_write_single_adu returned 0 slave=%u addr=0x%04X",
                 request.slave_id, request.address);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = transport_send_receive(tx, tx_len, rx, sizeof(rx), &rx_len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "app_execute_write_single: transport_send_receive failed: %s", esp_err_to_name(err));
        return err;
    }

    err = modbus_parse_write_response(rx, rx_len, MODBUS_FUNCTION_WRITE_SINGLE, out);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "app_execute_write_single: modbus_parse_write_response failed: %s (rx_len=%u)",
                 esp_err_to_name(err), (unsigned)rx_len);
        return err;
    }

    return ESP_OK;
}

static esp_err_t app_execute_write_group(const app_write_group_input_t *input, modbus_write_response_t *out)
{
    modbus_write_group_request_t request = {
        .slave_id = input->slave_id,
        .address = input->address,
        .count = input->count,
        .values = input->values,
    };

    uint8_t tx[MODBUS_MAX_ADU_SIZE] = {0};
    uint8_t rx[MODBUS_MAX_ADU_SIZE] = {0};
    size_t tx_len = modbus_build_write_group_adu(&request, tx, sizeof(tx));
    size_t rx_len = 0;

    if (tx_len == 0) {
        ESP_LOGD(TAG, "app_execute_write_group: modbus_build_write_group_adu returned 0 slave=%u addr=0x%04X count=%u",
                 request.slave_id, request.address, request.count);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = transport_send_receive(tx, tx_len, rx, sizeof(rx), &rx_len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "app_execute_write_group: transport_send_receive failed: %s", esp_err_to_name(err));
        return err;
    }

    err = modbus_parse_write_response(rx, rx_len, MODBUS_FUNCTION_WRITE_MULTIPLE, out);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "app_execute_write_group: modbus_parse_write_response failed: %s (rx_len=%u)",
                 esp_err_to_name(err), (unsigned)rx_len);
        return err;
    }

    return ESP_OK;
}

static void app_worker_task(void *arg)
{
    (void)arg;
    while (true) {
        app_job_t *job = NULL;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE || job == NULL) {
            continue;
        }

        ESP_LOGD(TAG, "worker: dequeued op=%s", app_op_to_str(job->op));

        switch (job->op) {
            case APP_OP_READ:
                job->result = app_execute_read(&job->read, &job->read_response);
                break;
            case APP_OP_WRITE_SINGLE:
                job->result = app_execute_write_single(&job->write, &job->write_response);
                break;
            case APP_OP_WRITE_GROUP:
                job->result = app_execute_write_group(&job->write_group, &job->write_response);
                break;
            default:
                job->result = ESP_ERR_NOT_SUPPORTED;
                break;
        }

        ESP_LOGD(TAG, "worker: op=%s result=%s", app_op_to_str(job->op), esp_err_to_name(job->result));
        xSemaphoreGive(job->done);
    }
}

esp_err_t app_service_init(void)
{
    ESP_RETURN_ON_ERROR(transport_init(), TAG, "transport_init failed");

    s_queue = xQueueCreate(16, sizeof(app_job_t *));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreate(app_worker_task, "modbus_worker", 6144, NULL, 8, NULL);
    if (created != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t app_transport_open(const app_transport_uart_config_t *cfg)
{
    transport_uart_config_t transport_cfg;
    ESP_RETURN_ON_ERROR(app_convert_transport_cfg(cfg, &transport_cfg), TAG, "invalid app transport config");
    return transport_open(&transport_cfg);
}

esp_err_t app_transport_switch(const app_transport_uart_config_t *cfg)
{
    transport_uart_config_t transport_cfg;
    ESP_RETURN_ON_ERROR(app_convert_transport_cfg(cfg, &transport_cfg), TAG, "invalid app transport config");
    return transport_switch(&transport_cfg);
}

esp_err_t app_transport_close(void)
{
    return transport_close();
}

esp_err_t app_transport_status(app_transport_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    transport_status_t transport_status = {0};
    ESP_RETURN_ON_ERROR(transport_status_get(&transport_status), TAG, "transport status failed");

    status->is_open = transport_status.is_open;
    status->uart_port = (app_uart_port_t)transport_status.uart_port;
    status->baud_rate = transport_status.baud_rate;
    status->tx_pin = transport_status.tx_pin;
    status->rx_pin = transport_status.rx_pin;
    status->timeout_ms = transport_status.timeout_ms;

    return ESP_OK;
}

static esp_err_t app_submit_job(app_job_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    job->done = xSemaphoreCreateBinary();
    if (job->done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "app_submit_job: enqueue op=%s", app_op_to_str(job->op));
    app_job_t *queued = job;
    if (xQueueSend(s_queue, &queued, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(job->done);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        vSemaphoreDelete(job->done);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGD(TAG, "app_submit_job: op=%s completed with %s", app_op_to_str(job->op), esp_err_to_name(job->result));
    vSemaphoreDelete(job->done);
    return job->result;
}

esp_err_t app_modbus_read(const app_read_input_t *input, app_read_response_t *out_response)
{
    if (input == NULL || out_response == NULL || input->count == 0 || input->count > APP_MODBUS_MAX_REG_VALUES) {
        ESP_LOGD(TAG, "app_modbus_read invalid args: input=%p out=%p count=%u",
                 (void *)input, (void *)out_response, input ? input->count : 0);

        return ESP_ERR_INVALID_ARG;
    }

    app_job_t job = {
        .op = APP_OP_READ,
        .read = *input,
    };

    esp_err_t err = app_submit_job(&job);
    if (err == ESP_OK) {
        app_fill_read_response(&job.read_response, out_response);
    }
    return err;
}

esp_err_t app_modbus_read_group(const app_read_input_t *input, app_read_response_t *out_response)
{
    return app_modbus_read(input, out_response);
}

esp_err_t app_modbus_write(const app_write_input_t *input, app_write_response_t *out_response)
{
    if (input == NULL || out_response == NULL) {
        ESP_LOGD(TAG, "app_modbus_write invalid args: input=%p out=%p", (void *)input, (void *)out_response);
        return ESP_ERR_INVALID_ARG;
    }

    app_job_t job = {
        .op = APP_OP_WRITE_SINGLE,
        .write = *input,
    };

    esp_err_t err = app_submit_job(&job);
    if (err == ESP_OK) {
        app_fill_write_response(&job.write_response, out_response);
    }
    return err;
}

esp_err_t app_modbus_write_group(const app_write_group_input_t *input, app_write_response_t *out_response)
{
    if (input == NULL || out_response == NULL || input->count == 0 || input->count > APP_MODBUS_MAX_REG_VALUES) {
        ESP_LOGD(TAG, "app_modbus_write_group invalid args: input=%p out=%p count=%u",
                 (void *)input, (void *)out_response, input ? input->count : 0);
        return ESP_ERR_INVALID_ARG;
    }

    app_job_t job = {
        .op = APP_OP_WRITE_GROUP,
        .write_group = *input,
    };

    esp_err_t err = app_submit_job(&job);
    if (err == ESP_OK) {
        app_fill_write_response(&job.write_response, out_response);
    }
    return err;
}
