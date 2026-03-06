#include "api/json_rpc_server.h"
#include "application/modbus_service.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "app_main";

#define BACKEND_AP_SSID "MCBackend"
#define BACKEND_AP_PASSWORD "13@Rtyr13"
#define BACKEND_AP_MAX_CONNECTIONS 4

static esp_err_t app_wifi_ap_start(void)
{
    esp_netif_t *created_ap_netif = esp_netif_create_default_wifi_ap();
    if (created_ap_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "esp_wifi_set_mode failed");

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = BACKEND_AP_SSID,
            .ssid_len = strlen(BACKEND_AP_SSID),
            .channel = 1,
            .password = BACKEND_AP_PASSWORD,
            .max_connection = BACKEND_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (ap_netif != NULL && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP started: ssid=%s", BACKEND_AP_SSID);
        ESP_LOGI(TAG, "Backend JSON-RPC endpoint: http://" IPSTR ":8080/rpc", IP2STR(&ip_info.ip));
    }

    return ESP_OK;
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // Required for lwIP/tcpip stack used by esp_http_server.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(app_wifi_ap_start());

    ESP_ERROR_CHECK(app_service_init());
    ESP_ERROR_CHECK(json_rpc_server_start());

    ESP_LOGI(TAG, "ESP32 Modbus Config Backend started");
}
