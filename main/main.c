#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#define PROV_AP_SSID             "ESP32C2_PROVISION"
#define PROV_AP_PASS             "12345678"
#define PROV_TCP_PORT            7000
#define UART_PORT                UART_NUM_0
#define UART_RX_BUF              1024
#define UART_TX_BUF              1024
#define WIFI_CONNECTED_BIT       BIT0
#define WIFI_FAIL_BIT            BIT1

#define BLE_APP_ID               0x55
#define BLE_SERVICE_HANDLE_NUM   8
#define BLE_MAX_VALUE_LEN        256
#define BLE_UUID_STR_LEN         32

static const char *TAG = "esp32_c2_gateway";
static EventGroupHandle_t wifi_event_group;
static int retry_num;
static int tcp_client_socket = -1;
static bool tcp_client_connected = false;
static bool ble_notify_enabled = false;
static uint16_t ble_conn_id = 0;
static esp_gatt_if_t ble_gatts_if = 0;
static uint16_t ble_service_handle = 0;
static uint16_t ble_char_handle = 0;
static uint16_t ble_cccd_handle = 0;

static const char *DEFAULT_SERVICE_UUID_STR = "6E400001B5A3F393E0A9E50E24DCCA9E";
static const char *DEFAULT_CHAR_UUID_STR = "6E400002B5A3F393E0A9E50E24DCCA9E";

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char tcp_host[64];
    uint16_t tcp_port;
    char ble_service_uuid[BLE_UUID_STR_LEN + 1];
    char ble_char_uuid[BLE_UUID_STR_LEN + 1];
    bool valid;
} app_config_t;

static app_config_t g_cfg;

static void tcp_client_task(void *arg);
static void provisioning_server_task(void *arg);
static void uart_at_task(void *arg);
static void start_sta_and_client(void);

static uint8_t ble_service_uuid128[16];
static uint8_t ble_char_uuid128[16];

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x20,
    .max_interval = 0x40,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 16,
    .p_service_uuid = ble_service_uuid128,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void ensure_ble_uuid_defaults(app_config_t *cfg)
{
    if (strlen(cfg->ble_service_uuid) != BLE_UUID_STR_LEN) {
        strlcpy(cfg->ble_service_uuid, DEFAULT_SERVICE_UUID_STR, sizeof(cfg->ble_service_uuid));
    }
    if (strlen(cfg->ble_char_uuid) != BLE_UUID_STR_LEN) {
        strlcpy(cfg->ble_char_uuid, DEFAULT_CHAR_UUID_STR, sizeof(cfg->ble_char_uuid));
    }
}

static esp_err_t save_config(const app_config_t *cfg)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("cfg", NVS_READWRITE, &nvs), TAG, "open nvs failed");
    ESP_RETURN_ON_ERROR(nvs_set_blob(nvs, "app", cfg, sizeof(*cfg)), TAG, "write cfg failed");
    ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, "valid", 1), TAG, "write valid failed");
    ESP_RETURN_ON_ERROR(nvs_commit(nvs), TAG, "commit failed");
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t load_config(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("cfg", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t valid = 0;
    err = nvs_get_u8(nvs, "valid", &valid);
    if (err != ESP_OK || valid == 0) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = sizeof(*cfg);
    err = nvs_get_blob(nvs, "app", cfg, &len);
    nvs_close(nvs);
    if (err == ESP_OK) {
        ensure_ble_uuid_defaults(cfg);
        cfg->valid = true;
    }
    return err;
}

static void factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset requested");
    nvs_flash_erase();
    esp_restart();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        tcp_client_connected = false;
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        if (retry_num < 10) {
            esp_wifi_connect();
            retry_num++;
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_softap(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid = PROV_AP_SSID,
            .ssid_len = strlen(PROV_AP_SSID),
            .channel = 1,
            .password = PROV_AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    if (strlen(PROV_AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    return ESP_OK;
}

static esp_err_t wifi_init_sta(const app_config_t *cfg)
{
    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, cfg->wifi_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, cfg->wifi_pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
    return ESP_OK;
}

static bool parse_provision_payload(const char *input, app_config_t *cfg)
{
    char ssid[33] = {0}, pass[65] = {0}, host[64] = {0};
    int port = 0;
    int n = sscanf(input, "SSID=%32[^;];PASS=%64[^;];HOST=%63[^;];PORT=%d", ssid, pass, host, &port);
    if (n != 4 || ssid[0] == '\0' || host[0] == '\0' || port <= 0 || port > 65535) {
        return false;
    }

    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->wifi_ssid, ssid, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_pass, pass, sizeof(cfg->wifi_pass));
    strlcpy(cfg->tcp_host, host, sizeof(cfg->tcp_host));
    cfg->tcp_port = (uint16_t)port;
    ensure_ble_uuid_defaults(cfg);
    cfg->valid = true;
    return true;
}

static int hex_to_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool uuid_str_to_bytes(const char *uuid_str, uint8_t out[16])
{
    if (strlen(uuid_str) != BLE_UUID_STR_LEN) {
        return false;
    }

    for (int i = 0; i < 16; ++i) {
        int hi = hex_to_nibble(uuid_str[i * 2]);
        int lo = hex_to_nibble(uuid_str[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void load_ble_uuid_from_cfg(void)
{
    if (!uuid_str_to_bytes(g_cfg.ble_service_uuid, ble_service_uuid128)) {
        uuid_str_to_bytes(DEFAULT_SERVICE_UUID_STR, ble_service_uuid128);
    }
    if (!uuid_str_to_bytes(g_cfg.ble_char_uuid, ble_char_uuid128)) {
        uuid_str_to_bytes(DEFAULT_CHAR_UUID_STR, ble_char_uuid128);
    }
}

static void handle_ble_to_tcp(const uint8_t *data, uint16_t len)
{
    if (tcp_client_connected && tcp_client_socket >= 0 && len > 0) {
        send(tcp_client_socket, data, len, 0);
    }
}

static void handle_tcp_to_ble(const uint8_t *data, size_t len)
{
    if (ble_notify_enabled && ble_char_handle != 0 && len > 0 && len <= BLE_MAX_VALUE_LEN) {
        esp_ble_gatts_send_indicate(ble_gatts_if, ble_conn_id, ble_char_handle,
                                    (uint16_t)len, (uint8_t *)data, false);
    }
}

static void tcp_client_task(void *arg)
{
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdFALSE, portMAX_DELAY);
        if (!(bits & WIFI_CONNECTED_BIT) || !g_cfg.valid) {
            continue;
        }

        struct sockaddr_in dest = {0};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(g_cfg.tcp_port);
        if (inet_pton(AF_INET, g_cfg.tcp_host, &dest.sin_addr) != 1) {
            struct hostent *host = gethostbyname(g_cfg.tcp_host);
            if (!host) {
                ESP_LOGE(TAG, "resolve host failed: %s", g_cfg.tcp_host);
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            memcpy(&dest.sin_addr, host->h_addr, host->h_length);
        }

        tcp_client_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (tcp_client_socket < 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (connect(tcp_client_socket, (struct sockaddr *)&dest, sizeof(dest)) == 0) {
            tcp_client_connected = true;
            ESP_LOGI(TAG, "TCP client connected to %s:%d", g_cfg.tcp_host, g_cfg.tcp_port);
            uint8_t rx[BLE_MAX_VALUE_LEN];
            while (1) {
                int len = recv(tcp_client_socket, rx, sizeof(rx), 0);
                if (len <= 0) {
                    break;
                }
                handle_tcp_to_ble(rx, (size_t)len);
            }
        }

        tcp_client_connected = false;
        if (tcp_client_socket >= 0) {
            close(tcp_client_socket);
            tcp_client_socket = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void start_sta_and_client(void)
{
    if (!g_cfg.valid) {
        return;
    }
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    wifi_init_sta(&g_cfg);
}

static void provisioning_server_task(void *arg)
{
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROV_TCP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_sock, 1) != 0) {
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Provisioning TCP server started on %d", PROV_TCP_PORT);

    while (1) {
        int client = accept(listen_sock, NULL, NULL);
        if (client < 0) {
            continue;
        }

        char rx[256] = {0};
        int len = recv(client, rx, sizeof(rx) - 1, 0);
        if (len > 0) {
            app_config_t new_cfg = g_cfg;
            if (parse_provision_payload(rx, &new_cfg)) {
                g_cfg = new_cfg;
                if (save_config(&g_cfg) == ESP_OK) {
                    send(client, "OK\n", 3, 0);
                    start_sta_and_client();
                } else {
                    send(client, "ERR:STORE\n", 10, 0);
                }
            } else {
                send(client, "ERR:FORMAT\n", 11, 0);
            }
        }
        close(client);
    }
}

static bool parse_ble_uuid_cmd(const char *line, char *service_uuid, size_t service_sz,
                               char *char_uuid, size_t char_sz)
{
    char service[BLE_UUID_STR_LEN + 1] = {0};
    char characteristic[BLE_UUID_STR_LEN + 1] = {0};
    int n = sscanf(line, "SERVICE=%32[0-9a-fA-F];CHAR=%32[0-9a-fA-F]", service, characteristic);
    if (n != 2) {
        return false;
    }
    strlcpy(service_uuid, service, service_sz);
    strlcpy(char_uuid, characteristic, char_sz);
    return true;
}

static void execute_at_command(const char *line)
{
    if (strncmp(line, "AT+SETCFG=", 10) == 0) {
        app_config_t new_cfg = g_cfg;
        if (parse_provision_payload(line + 10, &new_cfg)) {
            g_cfg = new_cfg;
            if (save_config(&g_cfg) == ESP_OK) {
                printf("OK\r\n");
                start_sta_and_client();
                return;
            }
        }
        printf("ERROR\r\n");
    } else if (strncmp(line, "AT+SETBLEUUID=", 14) == 0) {
        app_config_t new_cfg = g_cfg;
        if (parse_ble_uuid_cmd(line + 14, new_cfg.ble_service_uuid, sizeof(new_cfg.ble_service_uuid),
                               new_cfg.ble_char_uuid, sizeof(new_cfg.ble_char_uuid))) {
            uint8_t tmp_service[16], tmp_char[16];
            if (uuid_str_to_bytes(new_cfg.ble_service_uuid, tmp_service) &&
                uuid_str_to_bytes(new_cfg.ble_char_uuid, tmp_char)) {
                g_cfg = new_cfg;
                if (save_config(&g_cfg) == ESP_OK) {
                    printf("OK\r\n");
                    printf("+INFO:REBOOT_TO_APPLY\r\n");
                    return;
                }
            }
        }
        printf("ERROR\r\n");
    } else if (strcmp(line, "AT+CONNECT") == 0) {
        start_sta_and_client();
        printf("OK\r\n");
    } else if (strcmp(line, "AT+GETCFG?") == 0) {
        if (g_cfg.valid) {
            printf("+CFG:%s,%s,%s,%u\r\n", g_cfg.wifi_ssid, g_cfg.wifi_pass, g_cfg.tcp_host, g_cfg.tcp_port);
            printf("+BLEUUID:%s,%s\r\n", g_cfg.ble_service_uuid, g_cfg.ble_char_uuid);
            printf("OK\r\n");
        } else {
            printf("+CFG:EMPTY\r\nOK\r\n");
        }
    } else if (strcmp(line, "AT+FACTORY") == 0) {
        printf("OK\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        factory_reset();
    } else {
        printf("ERROR\r\n");
    }
}

static void uart_at_task(void *arg)
{
    uint8_t *buf = malloc(UART_RX_BUF);
    char line[256];
    size_t idx = 0;

    while (1) {
        int len = uart_read_bytes(UART_PORT, buf, UART_RX_BUF, pdMS_TO_TICKS(100));
        for (int i = 0; i < len; i++) {
            uint8_t c = buf[i];
            if (c == '\r' || c == '\n') {
                if (idx > 0) {
                    line[idx] = '\0';
                    execute_at_command(line);
                    idx = 0;
                }
            } else if (idx < sizeof(line) - 1) {
                line[idx++] = (char)c;
            }
        }
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) {
        esp_ble_gap_start_advertising(&s_adv_params);
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        esp_ble_gap_set_device_name("ESP32C2-BLE-UART");

        esp_gatt_srvc_id_t service_id = {
            .is_primary = true,
            .id.inst_id = 0x00,
            .id.uuid.len = ESP_UUID_LEN_128,
        };
        memcpy(service_id.id.uuid.uuid.uuid128, ble_service_uuid128, 16);
        esp_ble_gatts_create_service(gatts_if, &service_id, BLE_SERVICE_HANDLE_NUM);
        break;
    }
    case ESP_GATTS_CREATE_EVT: {
        ble_service_handle = param->create.service_handle;
        esp_ble_gatts_start_service(ble_service_handle);

        esp_bt_uuid_t char_uuid = {.len = ESP_UUID_LEN_128};
        memcpy(char_uuid.uuid.uuid128, ble_char_uuid128, 16);

        esp_attr_value_t char_val = {
            .attr_max_len = BLE_MAX_VALUE_LEN,
            .attr_len = 0,
            .attr_value = NULL,
        };

        esp_ble_gatts_add_char(ble_service_handle, &char_uuid,
                               ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                               ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE |
                                   ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                               &char_val, NULL);
        break;
    }
    case ESP_GATTS_ADD_CHAR_EVT: {
        ble_char_handle = param->add_char.attr_handle;
        esp_bt_uuid_t descr_uuid = {.len = ESP_UUID_LEN_16};
        descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
        esp_ble_gatts_add_char_descr(ble_service_handle, &descr_uuid,
                                     ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                     NULL, NULL);
        break;
    }
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        ble_cccd_handle = param->add_char_descr.attr_handle;
        esp_ble_gap_config_adv_data(&s_adv_data);
        break;
    case ESP_GATTS_CONNECT_EVT:
        ble_conn_id = param->connect.conn_id;
        ble_gatts_if = gatts_if;
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        ble_notify_enabled = false;
        esp_ble_gap_start_advertising(&s_adv_params);
        break;
    case ESP_GATTS_WRITE_EVT:
        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_OK, NULL);
        }
        if (param->write.handle == ble_cccd_handle && param->write.len == 2) {
            uint16_t cccd = param->write.value[0] | (param->write.value[1] << 8);
            ble_notify_enabled = (cccd & 0x0001) != 0;
        } else {
            handle_ble_to_tcp(param->write.value, param->write.len);
        }
        break;
    default:
        break;
    }
}

static void ble_init(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_profile_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(BLE_APP_ID));
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    memset(&g_cfg, 0, sizeof(g_cfg));
    ensure_ble_uuid_defaults(&g_cfg);

    wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(wifi_init_softap());
    ESP_ERROR_CHECK(esp_wifi_start());

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));

    if (load_config(&g_cfg) == ESP_OK && g_cfg.valid) {
        ESP_LOGI(TAG, "Loaded persisted config");
    }

    load_ble_uuid_from_cfg();
    ble_init();

    if (g_cfg.valid) {
        start_sta_and_client();
    }

    xTaskCreate(provisioning_server_task, "provision_srv", 4096, NULL, 5, NULL);
    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
    xTaskCreate(uart_at_task, "uart_at", 4096, NULL, 5, NULL);
}
