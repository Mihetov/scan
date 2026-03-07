#include "application/wifi_service.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define WIFI_CFG_NAMESPACE "wifi_cfg"
#define WIFI_CFG_STA_SSID_KEY "sta_ssid"
#define WIFI_CFG_STA_PASS_KEY "sta_pass"
#define WIFI_CFG_AP_SSID_KEY "ap_ssid"
#define WIFI_CFG_AP_PASS_KEY "ap_pass"

#define WIFI_CONNECT_RETRY_COUNT 5
#define WIFI_CONNECT_RETRY_INTERVAL_MS 30000

#define WIFI_EVENT_CONNECTED_BIT BIT0
#define WIFI_EVENT_DISCONNECTED_BIT BIT1

#define DEFAULT_STA_SSID "MCBackend"
#define DEFAULT_STA_PASSWORD "13@Rtyr13"
#define DEFAULT_AP_SSID "MCBackend"
#define DEFAULT_AP_PASSWORD "13@Rtyr13"
#define DEFAULT_AP_CHANNEL 1
#define DEFAULT_AP_MAX_CONNECTIONS 4

static const char *TAG = "wifi_service";

typedef struct {
    bool initialized;
    bool wifi_started;
    app_wifi_mode_t active_mode;
    bool sta_connected;
    int last_sta_disconnect_reason;
    app_wifi_sta_config_t sta_cfg;
    app_wifi_ap_config_t ap_cfg;
    SemaphoreHandle_t lock;
    EventGroupHandle_t events;
} app_wifi_ctx_t;

static app_wifi_ctx_t s_wifi;

static void app_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi.sta_connected = false;
        s_wifi.last_sta_disconnect_reason = disc ? disc->reason : -1;
        xEventGroupSetBits(s_wifi.events, WIFI_EVENT_DISCONNECTED_BIT);
        ESP_LOGD(TAG, "STA disconnected: reason=%d", s_wifi.last_sta_disconnect_reason);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        s_wifi.sta_connected = true;
        s_wifi.last_sta_disconnect_reason = 0;
        xEventGroupSetBits(s_wifi.events, WIFI_EVENT_CONNECTED_BIT);
        if (got_ip != NULL) {
            ESP_LOGI(TAG, "STA connected, IP=" IPSTR, IP2STR(&got_ip->ip_info.ip));
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started: ssid=%s", s_wifi.ap_cfg.ssid);
    }
}

static void app_wifi_apply_defaults(void)
{
    memset(&s_wifi.sta_cfg, 0, sizeof(s_wifi.sta_cfg));
    memset(&s_wifi.ap_cfg, 0, sizeof(s_wifi.ap_cfg));

    strncpy(s_wifi.sta_cfg.ssid, DEFAULT_STA_SSID, sizeof(s_wifi.sta_cfg.ssid) - 1);
    strncpy(s_wifi.sta_cfg.password, DEFAULT_STA_PASSWORD, sizeof(s_wifi.sta_cfg.password) - 1);

    strncpy(s_wifi.ap_cfg.ssid, DEFAULT_AP_SSID, sizeof(s_wifi.ap_cfg.ssid) - 1);
    strncpy(s_wifi.ap_cfg.password, DEFAULT_AP_PASSWORD, sizeof(s_wifi.ap_cfg.password) - 1);
    s_wifi.ap_cfg.channel = DEFAULT_AP_CHANNEL;
    s_wifi.ap_cfg.max_connection = DEFAULT_AP_MAX_CONNECTIONS;
}

static esp_err_t app_wifi_load_cfg_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(s_wifi.sta_cfg.ssid);
    nvs_get_str(handle, WIFI_CFG_STA_SSID_KEY, s_wifi.sta_cfg.ssid, &size);
    size = sizeof(s_wifi.sta_cfg.password);
    nvs_get_str(handle, WIFI_CFG_STA_PASS_KEY, s_wifi.sta_cfg.password, &size);
    size = sizeof(s_wifi.ap_cfg.ssid);
    nvs_get_str(handle, WIFI_CFG_AP_SSID_KEY, s_wifi.ap_cfg.ssid, &size);
    size = sizeof(s_wifi.ap_cfg.password);
    nvs_get_str(handle, WIFI_CFG_AP_PASS_KEY, s_wifi.ap_cfg.password, &size);

    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t app_wifi_save_cfg_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, WIFI_CFG_STA_SSID_KEY, s_wifi.sta_cfg.ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_CFG_STA_PASS_KEY, s_wifi.sta_cfg.password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_CFG_AP_SSID_KEY, s_wifi.ap_cfg.ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_CFG_AP_PASS_KEY, s_wifi.ap_cfg.password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save wifi cfg failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t app_wifi_start_sta_locked(void)
{
    wifi_config_t sta_cfg = {0};
    memcpy(sta_cfg.sta.ssid, s_wifi.sta_cfg.ssid, sizeof(sta_cfg.sta.ssid));
    memcpy(sta_cfg.sta.password, s_wifi.sta_cfg.password, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode sta failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set sta cfg failed");

    if (!s_wifi.wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
        s_wifi.wifi_started = true;
    }

    for (int attempt = 1; attempt <= WIFI_CONNECT_RETRY_COUNT; attempt++) {
        xEventGroupClearBits(s_wifi.events, WIFI_EVENT_CONNECTED_BIT | WIFI_EVENT_DISCONNECTED_BIT);
        ESP_LOGI(TAG, "STA connect attempt %d/%d to ssid=%s", attempt, WIFI_CONNECT_RETRY_COUNT, s_wifi.sta_cfg.ssid);

        esp_wifi_disconnect();
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "esp_wifi_connect failed");

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi.events,
            WIFI_EVENT_CONNECTED_BIT | WIFI_EVENT_DISCONNECTED_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_CONNECT_RETRY_INTERVAL_MS));

        if (bits & WIFI_EVENT_CONNECTED_BIT) {
            s_wifi.active_mode = APP_WIFI_MODE_STA;
            ESP_LOGI(TAG, "Connected to STA network");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "STA attempt %d failed (reason=%d)", attempt, s_wifi.last_sta_disconnect_reason);
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t app_wifi_start_ap_locked(void)
{
    wifi_config_t ap_cfg = {0};
    memcpy(ap_cfg.ap.ssid, s_wifi.ap_cfg.ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_wifi.ap_cfg.ssid);
    memcpy(ap_cfg.ap.password, s_wifi.ap_cfg.password, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = s_wifi.ap_cfg.channel;
    ap_cfg.ap.max_connection = s_wifi.ap_cfg.max_connection;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode ap failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set ap cfg failed");

    if (!s_wifi.wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
        s_wifi.wifi_started = true;
    }

    s_wifi.active_mode = APP_WIFI_MODE_AP;

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (ap_netif != NULL && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "AP IP=" IPSTR, IP2STR(&ip_info.ip));
    }

    return ESP_OK;
}

static esp_err_t app_wifi_restart_locked(void)
{
    if (s_wifi.wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "esp_wifi_stop failed");
        s_wifi.wifi_started = false;
        s_wifi.sta_connected = false;
        s_wifi.active_mode = APP_WIFI_MODE_NONE;
    }

    esp_err_t err = app_wifi_start_sta_locked();
    if (err == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA connect failed after retries, fallback to AP: %s", esp_err_to_name(err));
    return app_wifi_start_ap_locked();
}

static bool app_wifi_sta_cfg_valid(const app_wifi_sta_config_t *cfg)
{
    return cfg != NULL && cfg->ssid[0] != '\0';
}

static bool app_wifi_ap_cfg_valid(const app_wifi_ap_config_t *cfg)
{
    return cfg != NULL && cfg->ssid[0] != '\0';
}

esp_err_t app_wifi_service_init(void)
{
    if (s_wifi.initialized) {
        return ESP_OK;
    }

    memset(&s_wifi, 0, sizeof(s_wifi));

    s_wifi.lock = xSemaphoreCreateMutex();
    s_wifi.events = xEventGroupCreate();

    if (s_wifi.lock == NULL || s_wifi.events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    app_wifi_apply_defaults();
    app_wifi_load_cfg_nvs();

    // ИНИЦИАЛИЗАЦИЯ СЕТЕВОГО СТЕКА
    ESP_ERROR_CHECK(esp_netif_init());

    // СОЗДАНИЕ WIFI ИНТЕРФЕЙСОВ
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_RETURN_ON_ERROR(
        esp_wifi_init(&wifi_init_cfg),
        TAG,
        "esp_wifi_init failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_storage(WIFI_STORAGE_RAM),
        TAG,
        "esp_wifi_set_storage failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &app_wifi_event_handler,
            NULL),
        TAG,
        "register WIFI_EVENT failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &app_wifi_event_handler,
            NULL),
        TAG,
        "register IP_EVENT failed"
    );

    s_wifi.initialized = true;

    return ESP_OK;
}

esp_err_t app_wifi_start(void)
{
    if (!s_wifi.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    esp_err_t err = app_wifi_restart_locked();
    xSemaphoreGive(s_wifi.lock);
    return err;
}

esp_err_t app_wifi_apply(void)
{
    return app_wifi_start();
}

esp_err_t app_wifi_set_sta_config(const app_wifi_sta_config_t *cfg)
{
    if (!app_wifi_sta_cfg_valid(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    s_wifi.sta_cfg = *cfg;
    esp_err_t err = app_wifi_save_cfg_nvs();
    xSemaphoreGive(s_wifi.lock);
    return err;
}

esp_err_t app_wifi_set_ap_config(const app_wifi_ap_config_t *cfg)
{
    if (!app_wifi_ap_cfg_valid(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    s_wifi.ap_cfg = *cfg;
    if (s_wifi.ap_cfg.channel == 0) {
        s_wifi.ap_cfg.channel = DEFAULT_AP_CHANNEL;
    }
    if (s_wifi.ap_cfg.max_connection == 0) {
        s_wifi.ap_cfg.max_connection = DEFAULT_AP_MAX_CONNECTIONS;
    }

    esp_err_t err = app_wifi_save_cfg_nvs();
    xSemaphoreGive(s_wifi.lock);
    return err;
}

esp_err_t app_wifi_get_status(app_wifi_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
    out_status->active_mode = s_wifi.active_mode;
    out_status->sta_connected = s_wifi.sta_connected;
    out_status->last_sta_disconnect_reason = s_wifi.last_sta_disconnect_reason;
    out_status->sta = s_wifi.sta_cfg;
    out_status->ap = s_wifi.ap_cfg;
    xSemaphoreGive(s_wifi.lock);

    return ESP_OK;
}
