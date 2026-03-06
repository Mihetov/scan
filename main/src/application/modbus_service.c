#include "application/modbus_service.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(transport_send_receive(tx, tx_len, rx, sizeof(rx), &rx_len), TAG, "transport_send_receive failed");
    return modbus_parse_read_response(rx, rx_len, &request, out);
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
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(transport_send_receive(tx, tx_len, rx, sizeof(rx), &rx_len), TAG, "transport_send_receive failed");
    return modbus_parse_write_response(rx, rx_len, MODBUS_FUNCTION_WRITE_SINGLE, out);
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
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(transport_send_receive(tx, tx_len, rx, sizeof(rx), &rx_len), TAG, "transport_send_receive failed");
    return modbus_parse_write_response(rx, rx_len, MODBUS_FUNCTION_WRITE_MULTIPLE, out);
}

static void app_worker_task(void *arg)
{
    (void)arg;
    while (true) {
        app_job_t *job = NULL;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE || job == NULL) {
            continue;
        }

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

esp_err_t app_transport_open(const transport_uart_config_t *cfg)
{
    return transport_open(cfg);
}

esp_err_t app_transport_switch(const transport_uart_config_t *cfg)
{
    return transport_switch(cfg);
}

esp_err_t app_transport_close(void)
{
    return transport_close();
}

esp_err_t app_transport_status(transport_status_t *status)
{
    return transport_status_get(status);
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

    app_job_t *queued = job;
    if (xQueueSend(s_queue, &queued, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(job->done);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        vSemaphoreDelete(job->done);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(job->done);
    return job->result;
}

esp_err_t app_modbus_read(const app_read_input_t *input, modbus_read_response_t *out_response)
{
    if (input == NULL || out_response == NULL || input->count == 0 || input->count > MODBUS_MAX_REG_VALUES) {
        return ESP_ERR_INVALID_ARG;
    }

    app_job_t job = {
        .op = APP_OP_READ,
        .read = *input,
    };

    esp_err_t err = app_submit_job(&job);
    if (err == ESP_OK) {
        *out_response = job.read_response;
    }
    return err;
}

esp_err_t app_modbus_read_group(const app_read_input_t *input, modbus_read_response_t *out_response)
{
    return app_modbus_read(input, out_response);
}

esp_err_t app_modbus_write(const app_write_input_t *input, modbus_write_response_t *out_response)
{
    if (input == NULL || out_response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_job_t job = {
        .op = APP_OP_WRITE_SINGLE,
        .write = *input,
    };

    esp_err_t err = app_submit_job(&job);
    if (err == ESP_OK) {
        *out_response = job.write_response;
    }
    return err;
}

esp_err_t app_modbus_write_group(const app_write_group_input_t *input, modbus_write_response_t *out_response)
{
    if (input == NULL || out_response == NULL || input->count == 0 || input->count > MODBUS_MAX_REG_VALUES) {
        return ESP_ERR_INVALID_ARG;
    }

    app_job_t job = {
        .op = APP_OP_WRITE_GROUP,
        .write_group = *input,
    };

    esp_err_t err = app_submit_job(&job);
    if (err == ESP_OK) {
        *out_response = job.write_response;
    }
    return err;
}
