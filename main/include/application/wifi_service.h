#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WIFI_MODE_NONE = 0,
    APP_WIFI_MODE_STA,
    APP_WIFI_MODE_AP,
} app_wifi_mode_t;

typedef struct {
    char ssid[33];
    char password[65];
} app_wifi_sta_config_t;

typedef struct {
    char ssid[33];
    char password[65];
    uint8_t channel;
    uint8_t max_connection;
} app_wifi_ap_config_t;

typedef struct {
    app_wifi_mode_t active_mode;
    bool sta_connected;
    int last_sta_disconnect_reason;
    app_wifi_sta_config_t sta;
    app_wifi_ap_config_t ap;
} app_wifi_status_t;

esp_err_t app_wifi_service_init(void);
esp_err_t app_wifi_start(void);
esp_err_t app_wifi_apply(void);
esp_err_t app_wifi_set_sta_config(const app_wifi_sta_config_t *cfg);
esp_err_t app_wifi_set_ap_config(const app_wifi_ap_config_t *cfg);
esp_err_t app_wifi_get_status(app_wifi_status_t *out_status);

#ifdef __cplusplus
}
#endif
