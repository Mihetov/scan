#include "api/json_rpc_server.h"
#include "application/modbus_service.h"
#include "application/wifi_service.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(app_wifi_service_init());
    ESP_ERROR_CHECK(app_wifi_start());

    ESP_ERROR_CHECK(app_service_init());
    ESP_ERROR_CHECK(json_rpc_server_start());

    ESP_LOGI(TAG, "ESP32 Modbus Config Backend started");
}
