#include "api/json_rpc_server.h"
#include "application/modbus_service.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(app_service_init());
    ESP_ERROR_CHECK(json_rpc_server_start());

    ESP_LOGI(TAG, "ESP32 Modbus Config Backend started");
}
