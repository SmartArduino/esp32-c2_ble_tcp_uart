#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/uart.h"
#include "esp_vfs_dev.h"
#include "cJSON.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define STORAGE_NAMESPACE  "cfg"
#define PROV_AP_SSID       "ESP32C2_CFG"
#define PROV_AP_PASS       "12345678"
#define PROV_TCP_PORT      9000
#define UART_PORT          UART_NUM_0
#define UART_RX_BUF        256
#define MAX_FIELD          64
#define MAX_LINE           320
#define TCP_RX_BUF         256
#define DEFAULT_SVC_UUID16 0xFFF0
#define DEFAULT_CHR_UUID16 0xFFF1
#define AT_ESCAPE_SEQ      "+++"

static const char *TAG = "esp32c2_gateway";

struct app_config {
    char wifi_ssid[MAX_FIELD];
    char wifi_pass[MAX_FIELD];
    char server_ip[MAX_FIELD];
    uint16_t server_port;
    uint16_t ble_svc_uuid16;
    uint16_t ble_chr_uuid16;
    bool provisioned;
};

static struct app_config s_cfg = {
    .ble_svc_uuid16 = DEFAULT_SVC_UUID16,
    .ble_chr_uuid16 = DEFAULT_CHR_UUID16,
};

static EventGroupHandle_t s_wifi_event_group;
static esp_event_handler_instance_t s_wifi_evt_any;
static esp_event_handler_instance_t s_ip_evt_got_ip;
static int s_retry_num;
static int s_tcp_client_fd = -1;
static bool s_uart_data_mode;

static uint8_t s_ble_addr_type;
static uint16_t s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_ble_char_val_handle;
static ble_uuid16_t s_gatt_svc_uuid;
static ble_uuid16_t s_gatt_chr_uuid;

static struct ble_gatt_chr_def s_chr_defs[2];
static struct ble_gatt_svc_def s_gatt_svcs[2];

static int ble_gap_event(struct ble_gap_event *event, void *arg);

static void set_default_ble_uuid_if_needed(void)
{
    if (s_cfg.ble_svc_uuid16 == 0) {
        s_cfg.ble_svc_uuid16 = DEFAULT_SVC_UUID16;
    }
    if (s_cfg.ble_chr_uuid16 == 0) {
        s_cfg.ble_chr_uuid16 = DEFAULT_CHR_UUID16;
    }
}

static esp_err_t cfg_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs, "app_cfg", &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void cfg_load(void)
{
    nvs_handle_t nvs;
    size_t size = sizeof(s_cfg);
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        set_default_ble_uuid_if_needed();
        return;
    }

    err = nvs_get_blob(nvs, "app_cfg", &s_cfg, &size);
    if (err != ESP_OK || size != sizeof(s_cfg)) {
        memset(&s_cfg, 0, sizeof(s_cfg));
    }
    nvs_close(nvs);
    set_default_ble_uuid_if_needed();
}

static void cfg_factory_reset(void)
{
    nvs_handle_t nvs;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, "app_cfg");
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    set_default_ble_uuid_if_needed();
}

static void tcp_client_close(void)
{
    if (s_tcp_client_fd >= 0) {
        shutdown(s_tcp_client_fd, SHUT_RDWR);
        close(s_tcp_client_fd);
        s_tcp_client_fd = -1;
    }
}

static bool validate_server(const char *ip, int port)
{
    struct in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1 && port > 0 && port <= 65535;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        tcp_client_close();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_retry_num < 10) {
            s_retry_num++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start_ap(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .channel = 1,
        },
    };

    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", PROV_AP_SSID);
    ap_config.ap.ssid_len = strlen(PROV_AP_SSID);
    snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", PROV_AP_PASS);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "set AP config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "Provision AP started: %s:%d", PROV_AP_SSID, PROV_TCP_PORT);
    return ESP_OK;
}

static esp_err_t wifi_start_sta(void)
{
    if (!s_cfg.provisioned) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t sta = {0};
    snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", s_cfg.wifi_ssid);
    snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", s_cfg.wifi_pass);

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "set sta cfg failed");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "sta connect failed");
    return ESP_OK;
}

static void tcp_client_connect(void)
{
    if (!s_cfg.provisioned || !validate_server(s_cfg.server_ip, s_cfg.server_port)) {
        return;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(s_cfg.server_port),
    };
    inet_pton(AF_INET, s_cfg.server_ip, &dest.sin_addr);

    tcp_client_close();
    s_tcp_client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_tcp_client_fd < 0) {
        ESP_LOGE(TAG, "socket create failed errno=%d", errno);
        return;
    }

    struct timeval tv = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    setsockopt(s_tcp_client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(s_tcp_client_fd, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "connect %s:%u failed errno=%d", s_cfg.server_ip, s_cfg.server_port, errno);
        tcp_client_close();
        return;
    }

    ESP_LOGI(TAG, "TCP client connected to %s:%u", s_cfg.server_ip, s_cfg.server_port);
}

static bool parse_and_apply_json_cfg(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        return false;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "wifi_pass");
    cJSON *srv = cJSON_GetObjectItem(root, "server_ip");
    cJSON *port = cJSON_GetObjectItem(root, "server_port");

    bool ok = cJSON_IsString(ssid) && cJSON_IsString(pass) && cJSON_IsString(srv) && cJSON_IsNumber(port);
    if (ok && validate_server(srv->valuestring, port->valueint)) {
        snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "%s", ssid->valuestring);
        snprintf(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "%s", pass->valuestring);
        snprintf(s_cfg.server_ip, sizeof(s_cfg.server_ip), "%s", srv->valuestring);
        s_cfg.server_port = (uint16_t)port->valueint;
        s_cfg.provisioned = true;
        ok = (cfg_save() == ESP_OK);
    } else {
        ok = false;
    }

    cJSON_Delete(root);
    return ok;
}

static void provisioning_server_task(void *arg)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "provision socket create failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PROV_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "provision bind failed errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 2) != 0) {
        ESP_LOGE(TAG, "provision listen failed errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int sock = accept(listen_fd, (struct sockaddr *)&src, &slen);
        if (sock < 0) {
            continue;
        }

        char rx[MAX_LINE] = {0};
        int len = recv(sock, rx, sizeof(rx) - 1, 0);
        if (len > 0 && parse_and_apply_json_cfg(rx)) {
            send(sock, "OK\n", 3, 0);
            wifi_start_sta();
        } else {
            send(sock, "ERR\n", 4, 0);
        }

        close(sock);
    }
}

static void ble_restart_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(s_ble_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_ble_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "BLE connected handle=%u", s_ble_conn_handle);
            } else {
                s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                ble_restart_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "BLE disconnected");
            ble_restart_advertising();
            return 0;

        default:
            return 0;
    }
}

static int ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > MAX_LINE) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        char *buf = calloc(1, len + 1);
        if (buf == NULL) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        os_mbuf_copydata(ctxt->om, 0, len, buf);
        if (s_uart_data_mode) {
            uart_write_bytes(UART_PORT, buf, len);
        }
        free(buf);
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *status = s_cfg.provisioned ? "READY" : "UNPROV";
        os_mbuf_append(ctxt->om, status, strlen(status));
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static void ble_setup_gatt_dynamic_uuid(void)
{
    s_gatt_svc_uuid = BLE_UUID16_INIT(s_cfg.ble_svc_uuid16);
    s_gatt_chr_uuid = BLE_UUID16_INIT(s_cfg.ble_chr_uuid16);

    memset(s_chr_defs, 0, sizeof(s_chr_defs));
    memset(s_gatt_svcs, 0, sizeof(s_gatt_svcs));

    s_chr_defs[0].uuid = &s_gatt_chr_uuid.u;
    s_chr_defs[0].access_cb = ble_gatt_access;
    s_chr_defs[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY;
    s_chr_defs[0].val_handle = &s_ble_char_val_handle;

    s_gatt_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_gatt_svcs[0].uuid = &s_gatt_svc_uuid.u;
    s_gatt_svcs[0].characteristics = s_chr_defs;
}

static void ble_app_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    const char *name = "ESP32C2-AT-BLE";

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);

    ble_restart_advertising();
}

static void ble_on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_ble_addr_type);
    ble_app_advertise();
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_init(void)
{
    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_setup_gatt_dynamic_uuid();
    ble_gatts_count_cfg(s_gatt_svcs);
    ble_gatts_add_svcs(s_gatt_svcs);

    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
}

static bool parse_ble_uuid_cmd(const char *line, uint16_t *svc, uint16_t *chr)
{
    unsigned int svc_tmp = 0;
    unsigned int chr_tmp = 0;

    if (sscanf(line, "AT+BLEUUID=%x,%x", &svc_tmp, &chr_tmp) != 2) {
        return false;
    }

    if (svc_tmp == 0 || svc_tmp > 0xFFFF || chr_tmp == 0 || chr_tmp > 0xFFFF) {
        return false;
    }

    *svc = (uint16_t)svc_tmp;
    *chr = (uint16_t)chr_tmp;
    return true;
}

static bool parse_wifi_cfg_cmd(const char *line, char *ssid, char *pass)
{
    return sscanf(line, "AT+WIFICFG=%63[^,],%63s", ssid, pass) == 2;
}

static bool parse_server_cfg_cmd(const char *line, char *ip, uint16_t *port)
{
    int port_tmp = 0;
    if (sscanf(line, "AT+SRVCFG=%63[^,],%d", ip, &port_tmp) != 2) {
        return false;
    }
    if (!validate_server(ip, port_tmp)) {
        return false;
    }
    *port = (uint16_t)port_tmp;
    return true;
}

static bool cfg_is_ready(void)
{
    return strlen(s_cfg.wifi_ssid) > 0 && strlen(s_cfg.wifi_pass) > 0 && validate_server(s_cfg.server_ip, s_cfg.server_port);
}

static void at_handle_line(const char *line)
{
    if (strcmp(line, "AT") == 0) {
        printf("OK\r\n");
        return;
    }

    if (strncmp(line, "AT+SETCFG=", 10) == 0) {
        if (parse_and_apply_json_cfg(line + 10)) {
            wifi_start_sta();
            printf("OK\r\n");
        } else {
            printf("ERR\r\n");
        }
        return;
    }

    if (strcmp(line, "AT+CONNECT") == 0) {
        if ((xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0) {
            tcp_client_connect();
            printf("OK\r\n");
        } else {
            printf("ERR\r\n");
        }
        return;
    }

    if (strncmp(line, "AT+WIFICFG=", 11) == 0) {
        char ssid[MAX_FIELD] = {0};
        char pass[MAX_FIELD] = {0};
        if (!parse_wifi_cfg_cmd(line, ssid, pass)) {
            printf("ERR\r\n");
            return;
        }

        snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "%s", ssid);
        snprintf(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "%s", pass);
        s_cfg.provisioned = cfg_is_ready();
        printf(cfg_save() == ESP_OK ? "OK\r\n" : "ERR\r\n");
        return;
    }

    if (strncmp(line, "AT+SRVCFG=", 10) == 0) {
        char ip[MAX_FIELD] = {0};
        uint16_t port = 0;
        if (!parse_server_cfg_cmd(line, ip, &port)) {
            printf("ERR\r\n");
            return;
        }

        snprintf(s_cfg.server_ip, sizeof(s_cfg.server_ip), "%s", ip);
        s_cfg.server_port = port;
        s_cfg.provisioned = cfg_is_ready();
        printf(cfg_save() == ESP_OK ? "OK\r\n" : "ERR\r\n");
        return;
    }

    if (strncmp(line, "AT+BLEUUID=", 11) == 0) {
        uint16_t svc, chr;
        if (parse_ble_uuid_cmd(line, &svc, &chr)) {
            s_cfg.ble_svc_uuid16 = svc;
            s_cfg.ble_chr_uuid16 = chr;
            if (cfg_save() == ESP_OK) {
                printf("OK\r\n");
            } else {
                printf("ERR\r\n");
            }
        } else {
            printf("ERR\r\n");
        }
        return;
    }

    if (strcmp(line, "AT+BLEUUID?") == 0) {
        printf("+BLEUUID:%04X,%04X\r\nOK\r\n", s_cfg.ble_svc_uuid16, s_cfg.ble_chr_uuid16);
        return;
    }

    if (strcmp(line, "AT+SAVE") == 0) {
        printf(cfg_save() == ESP_OK ? "OK\r\n" : "ERR\r\n");
        return;
    }

    if (strcmp(line, "AT+FACTORY") == 0) {
        tcp_client_close();
        cfg_factory_reset();
        s_uart_data_mode = false;
        esp_wifi_disconnect();
        printf("OK\r\n");
        return;
    }

    if (strcmp(line, "AT+RST") == 0) {
        printf("OK\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    if (strcmp(line, "AT+ENTM") == 0) {
        s_uart_data_mode = true;
        printf("OK\r\n");
        return;
    }

    if (strcmp(line, "AT+EXIT") == 0) {
        s_uart_data_mode = false;
        printf("OK\r\n");
        return;
    }

    if (strcmp(line, "AT+STATUS?") == 0) {
        printf("+STATUS:%s,%s,%u,TCP=%s\r\nOK\r\n",
               s_cfg.provisioned ? s_cfg.wifi_ssid : "UNPROV",
               s_cfg.provisioned ? s_cfg.server_ip : "0.0.0.0",
               s_cfg.provisioned ? s_cfg.server_port : 0,
               s_tcp_client_fd >= 0 ? "UP" : "DOWN");
        return;
    }

    printf("ERR\r\n");
}


static void tcp_rx_task(void *arg)
{
    uint8_t rx[TCP_RX_BUF];

    while (1) {
        if (s_tcp_client_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        int len = recv(s_tcp_client_fd, rx, sizeof(rx), 0);
        if (len > 0) {
            if (s_uart_data_mode) {
                uart_write_bytes(UART_PORT, (const char *)rx, len);
            }
            continue;
        }

        if (len == 0) {
            ESP_LOGW(TAG, "TCP peer closed");
            tcp_client_close();
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
        }

        ESP_LOGW(TAG, "TCP recv failed errno=%d", errno);
        tcp_client_close();
    }
}

static void uart_at_task(void *arg)
{
    uint8_t data[UART_RX_BUF];
    char line[MAX_LINE] = {0};
    size_t pos = 0;
    size_t plus_count = 0;

    while (1) {
        int len = uart_read_bytes(UART_PORT, data, sizeof(data), pdMS_TO_TICKS(100));
        for (int i = 0; i < len; i++) {
            if (s_uart_data_mode) {
                if (data[i] == '+') {
                    plus_count++;
                    if (plus_count == strlen(AT_ESCAPE_SEQ)) {
                        s_uart_data_mode = false;
                        plus_count = 0;
                        printf("\r\nOK\r\n");
                    }
                    continue;
                }

                if (plus_count > 0) {
                    for (size_t j = 0; j < plus_count; j++) {
                        if (s_tcp_client_fd >= 0) {
                            send(s_tcp_client_fd, "+", 1, 0);
                        }
                        if (s_ble_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                            struct os_mbuf *om = ble_hs_mbuf_from_flat("+", 1);
                            if (om != NULL) {
                                ble_gatts_notify_custom(s_ble_conn_handle, s_ble_char_val_handle, om);
                            }
                        }
                    }
                    plus_count = 0;
                }

                if (s_tcp_client_fd >= 0) {
                    send(s_tcp_client_fd, &data[i], 1, 0);
                }

                if (s_ble_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                    struct os_mbuf *om = ble_hs_mbuf_from_flat(&data[i], 1);
                    if (om != NULL) {
                        ble_gatts_notify_custom(s_ble_conn_handle, s_ble_char_val_handle, om);
                    }
                }
                continue;
            }

            if (data[i] == '\r' || data[i] == '\n') {
                if (pos > 0) {
                    line[pos] = '\0';
                    at_handle_line(line);
                    pos = 0;
                }
            } else if (pos < sizeof(line) - 1) {
                line[pos++] = (char)data[i];
            }
        }

        if ((xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) && s_tcp_client_fd < 0) {
            tcp_client_connect();
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_evt_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_ip_evt_got_ip));

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    esp_vfs_dev_uart_use_driver(UART_PORT);

    cfg_load();
    ESP_ERROR_CHECK(wifi_start_ap());
    if (s_cfg.provisioned) {
        wifi_start_sta();
    }

    ble_init();

    xTaskCreate(provisioning_server_task, "prov_srv", 4096, NULL, 5, NULL);
    xTaskCreate(tcp_rx_task, "tcp_rx", 4096, NULL, 5, NULL);
    xTaskCreate(uart_at_task, "uart_at", 4096, NULL, 5, NULL);
}
