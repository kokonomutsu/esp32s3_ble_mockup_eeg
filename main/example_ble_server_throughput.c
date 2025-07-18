/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/****************************************************************************
*
* This is the demo to test the BLE throughput. It should be used together with throughput_client demo.
*
****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gatt_common_api.h"
#include "esp_timer.h"

#include "sdkconfig.h"

/**********************************************************
 * Thread/Task reference
 **********************************************************/
#ifdef CONFIG_BLUEDROID_PINNED_TO_CORE
#define BLUETOOTH_TASK_PINNED_TO_CORE              (CONFIG_BLUEDROID_PINNED_TO_CORE < CONFIG_FREERTOS_NUMBER_OF_CORES ? CONFIG_BLUEDROID_PINNED_TO_CORE : tskNO_AFFINITY)
#else
#define BLUETOOTH_TASK_PINNED_TO_CORE              (0)
#endif

#define SECOND_TO_USECOND          1000000

#define GATTS_TAG "GATTS_DEMO"

#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
#define GATTS_NOTIFY_LEN    160
static SemaphoreHandle_t gatts_semaphore;
static bool can_send_notify = false;
static uint8_t indicate_data[GATTS_NOTIFY_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a};

/* Auto-start timer variables */
static bool auto_start_enabled = false;
static TickType_t connection_time = 0;

/* Package format variables for performance testing */
static uint8_t bSPPZipSingleFrameFull[GATTS_NOTIFY_LEN];
static uint16_t uPayloadSize = 0;
static uint16_t uMsgIdx = 0;
static uint16_t tempIndex = 0;
static uint16_t current_mtu = 23;  // Default MTU, will be updated on MTU exchange

/* Throughput calculation variables for NOTIFY mode */
static uint64_t notify_start_time = 0;
static uint64_t notify_current_time = 0;
static uint64_t notify_sent_packages = 0;
static uint64_t notify_sent_bytes = 0;
static bool notify_throughput_started = false;

/* Bitrate control variables */
#define TARGET_BITRATE_KBPS 20  // Target bitrate in kbps
#define TARGET_BITRATE_BPS (TARGET_BITRATE_KBPS * 1000 / 8)  // Convert to bytes per second
static uint64_t last_send_time = 0;

#endif /* #if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT) */

#if (CONFIG_EXAMPLE_GATTC_WRITE_THROUGHPUT)
static bool start = false;
static uint64_t write_len = 0;
static uint64_t start_time = 0;
static uint64_t current_time = 0;
#endif /* #if (CONFIG_EXAMPLE_GATTC_WRITE_THROUGHPUT) */

static bool is_connect = false;

/* Auto-start timer task */
void auto_start_timer_task(void *param) {
    while (1) {
        if (is_connect && !auto_start_enabled) {
            // Calculate time since connection
            TickType_t current_time = xTaskGetTickCount();
            TickType_t elapsed_time = current_time - connection_time;
            
            // Check if 2 seconds have passed since connection
            if (elapsed_time >= (2000 / portTICK_PERIOD_MS)) {
                // Check if still connected and not already enabled
                if (is_connect && !can_send_notify) {
                    ESP_LOGI(GATTS_TAG, "Auto-starting data transmission after 2s...");
                    can_send_notify = true;
                    auto_start_enabled = true;
                    xSemaphoreGive(gatts_semaphore);
                }
            }
        } else if (!is_connect) {
            // Reset auto-start flag when disconnected
            auto_start_enabled = false;
        }
        
        vTaskDelay(100 / portTICK_PERIOD_MS);  // Check every 100ms
    }
}

///Declare the static function
static void gatts_profile_a_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

#define REMOTE_SERVICE_UUID        0xFFF0
#define REMOTE_NOTIFY_CHAR_UUID    0xFFF1
#define GATTS_DESCR_UUID_TEST_A     0x3333
#define GATTS_NUM_HANDLE_TEST_A     4

#define GATTS_SERVICE_UUID_TEST_B   0x00EE
#define GATTS_CHAR_UUID_TEST_B      0xEE01
#define GATTS_DESCR_UUID_TEST_B     0x2222
#define GATTS_NUM_HANDLE_TEST_B     4

#define TEST_MANUFACTURER_DATA_LEN  17

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 0x40

#define PREPARE_BUF_MAX_SIZE 1024

static char test_device_name[ESP_BLE_ADV_NAME_LEN_MAX] = "NeuroNap-LE-0AF2-B";
static uint8_t char1_str[] = {0x11,0x22,0x33};
static esp_gatt_char_prop_t a_property = 0;

static esp_attr_value_t gatts_demo_char1_val =
{
    .attr_max_len = GATTS_DEMO_CHAR_VAL_LEN_MAX,
    .attr_len     = sizeof(char1_str),
    .attr_value   = char1_str,
};

static uint8_t adv_config_done = 0;
#define adv_config_flag      (1 << 0)
#define scan_rsp_config_flag (1 << 1)

#ifdef CONFIG_EXAMPLE_SET_RAW_ADV_DATA
static uint8_t raw_adv_data[] = {
    /* Flags */
    0x02, ESP_BLE_AD_TYPE_FLAG, 0x06,
    /* TX Power Level */
    0x02, ESP_BLE_AD_TYPE_TX_PWR, 0xEB,
    /* Service UUID */
    0x03, ESP_BLE_AD_TYPE_16SRV_CMPL, 0xAB, 0xCD
};

static uint8_t raw_scan_rsp_data[] = {
    /* Complete Local Name */
    0x0F, ESP_BLE_AD_TYPE_NAME_CMPL, 'E', 'S', 'P', '_', 'G', 'A', 'T', 'T', 'S', '_', 'D', 'E', 'M', 'O'
};
#else

static uint8_t adv_service_uuid128[32] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    //first uuid, 16bit, [12],[13] is the value
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xEE, 0x00, 0x00, 0x00,
    //second uuid, 32bit, [12], [13], [14], [15] is the value
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

// The length of adv data must be less than 31 bytes
//static uint8_t test_manufacturer[TEST_MANUFACTURER_DATA_LEN] =  {0x12, 0x23, 0x45, 0x56};
//adv data
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006, //slave connection min interval, Time = min_interval * 1.25 msec
    .max_interval = 0x000C, //slave connection max interval, Time = max_interval * 1.25 msec
    .appearance = 0x00,
    .manufacturer_len = 0, //TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data =  NULL, //&test_manufacturer[0],
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 32,
    .p_service_uuid = adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};
// scan response data
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x000C,
    .appearance = 0x00,
    .manufacturer_len = 0, //TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data =  NULL, //&test_manufacturer[0],
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 32,
    .p_service_uuid = adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

#endif /* CONFIG_EXAMPLE_SET_RAW_ADV_DATA */

static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    //.peer_addr            =
    //.peer_addr_type       =
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

#define PROFILE_NUM 1
#define PROFILE_A_APP_ID 0

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

/* One gatt-based profile one app_id and one gatts_if, this array will store the gatts_if returned by ESP_GATTS_REG_EVT */
static struct gatts_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {
        .gatts_cb = gatts_profile_a_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
};

typedef struct {
    uint8_t                 *prepare_buf;
    int                     prepare_len;
} prepare_type_env_t;

static prepare_type_env_t a_prepare_write_env;

void example_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param);
void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param);

static uint8_t check_sum(uint8_t *addr, uint16_t count)
{
    uint32_t sum = 0;

    if (addr == NULL || count == 0) {
        return 0;
    }

    for(int i = 0; i < count; i++) {
        sum = sum + addr[i];
    }

    while (sum >> 8) {
        sum = (sum & 0xff) + (sum >> 8);
    }

    return (uint8_t)~sum;
}


static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
#ifdef CONFIG_EXAMPLE_SET_RAW_ADV_DATA
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done==0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done==0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
#else
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done == 0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done == 0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
#endif
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        //advertising start complete event to indicate advertising start successfully or failed
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTS_TAG, "Advertising start failed, status %d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(GATTS_TAG, "Advertising start successfully");
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTS_TAG, "Advertising stop failed, status %d", param->adv_stop_cmpl.status);
        }
        else {
            ESP_LOGI(GATTS_TAG, "Advertising stop successfully");
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
         ESP_LOGI(GATTS_TAG, "Connection params update, status %d, conn_int %d, latency %d, timeout %d",
                  param->update_conn_params.status,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}

void example_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    esp_gatt_status_t status = ESP_GATT_OK;
    if (param->write.need_rsp) {
        if (param->write.is_prep) {
            if (param->write.offset > PREPARE_BUF_MAX_SIZE) {
                status = ESP_GATT_INVALID_OFFSET;
            } else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE) {
                status = ESP_GATT_INVALID_ATTR_LEN;
            }

            if (status == ESP_GATT_OK && prepare_write_env->prepare_buf == NULL) {
                prepare_write_env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE * sizeof(uint8_t));
                prepare_write_env->prepare_len = 0;
                if (prepare_write_env->prepare_buf == NULL) {
                    ESP_LOGE(GATTS_TAG, "Gatt_server prep no mem");
                    status = ESP_GATT_NO_RESOURCES;
                }
            }

            esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
            if (gatt_rsp) {
                gatt_rsp->attr_value.len = param->write.len;
                gatt_rsp->attr_value.handle = param->write.handle;
                gatt_rsp->attr_value.offset = param->write.offset;
                gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
                memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);
                esp_err_t response_err = esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, gatt_rsp);

                if (response_err != ESP_OK) {
                    ESP_LOGE(GATTS_TAG, "Send response error\n");
                }
                free(gatt_rsp);
            } else {
                ESP_LOGE(GATTS_TAG, "malloc failed, no resource to send response error\n");
                status = ESP_GATT_NO_RESOURCES;
            }

            if (status != ESP_GATT_OK) {
                return;
            }
            memcpy(prepare_write_env->prepare_buf + param->write.offset,
                   param->write.value,
                   param->write.len);
            prepare_write_env->prepare_len += param->write.len;

        }else {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, NULL);
        }
    }
}

void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    if (param->exec_write.exec_write_flag != ESP_GATT_PREP_WRITE_EXEC){
        ESP_LOGI(GATTS_TAG,"Prepare write cancel");
    }
    if (prepare_write_env->prepare_buf) {
        free(prepare_write_env->prepare_buf);
        prepare_write_env->prepare_buf = NULL;
    }
    prepare_write_env->prepare_len = 0;
}

static void gatts_profile_a_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(GATTS_TAG, "GATT server register, status %d, app_id %d, gatts_if %d", param->reg.status, param->reg.app_id, gatts_if);
        gl_profile_tab[PROFILE_A_APP_ID].service_id.is_primary = true;
        gl_profile_tab[PROFILE_A_APP_ID].service_id.id.inst_id = 0x00;
        gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.uuid.uuid16 = REMOTE_SERVICE_UUID;
        gl_profile_tab[PROFILE_A_APP_ID].gatts_if = gatts_if;
        esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(test_device_name);
        if (set_dev_name_ret){
            ESP_LOGE(GATTS_TAG, "set device name failed, error code = %x", set_dev_name_ret);
        }
#ifdef CONFIG_EXAMPLE_SET_RAW_ADV_DATA
        esp_err_t raw_adv_ret = esp_ble_gap_config_adv_data_raw(raw_adv_data, sizeof(raw_adv_data));
        if (raw_adv_ret){
            ESP_LOGE(GATTS_TAG, "config raw adv data failed, error code = %x ", raw_adv_ret);
        }
        adv_config_done |= adv_config_flag;
        esp_err_t raw_scan_ret = esp_ble_gap_config_scan_rsp_data_raw(raw_scan_rsp_data, sizeof(raw_scan_rsp_data));
        if (raw_scan_ret){
            ESP_LOGE(GATTS_TAG, "config raw scan rsp data failed, error code = %x", raw_scan_ret);
        }
        adv_config_done |= scan_rsp_config_flag;
#else
        //config adv data
        esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret){
            ESP_LOGE(GATTS_TAG, "config adv data failed, error code = %x", ret);
        }
        adv_config_done |= adv_config_flag;
        //config scan response data
        ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
        if (ret){
            ESP_LOGE(GATTS_TAG, "config scan response data failed, error code = %x", ret);
        }
        adv_config_done |= scan_rsp_config_flag;

#endif
        esp_ble_gatts_create_service(gatts_if, &gl_profile_tab[PROFILE_A_APP_ID].service_id, GATTS_NUM_HANDLE_TEST_A);
        break;
    case ESP_GATTS_READ_EVT: {
        ESP_LOGI(GATTS_TAG, "Characteristic read, conn_id %d, trans_id %" PRIu32 ", handle %d", param->read.conn_id, param->read.trans_id, param->read.handle);
        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = 4;
        rsp.attr_value.value[0] = 0xde;
        rsp.attr_value.value[1] = 0xed;
        rsp.attr_value.value[2] = 0xbe;
        rsp.attr_value.value[3] = 0xef;
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                    ESP_GATT_OK, &rsp);
        break;
    }
    case ESP_GATTS_WRITE_EVT: {
#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
        if (!param->write.is_prep){
            if (gl_profile_tab[PROFILE_A_APP_ID].descr_handle == param->write.handle && param->write.len == 2){
                uint16_t descr_value = param->write.value[1]<<8 | param->write.value[0];
                if (descr_value == 0x0001){
                    if (a_property & ESP_GATT_CHAR_PROP_BIT_NOTIFY){

                        ESP_LOGI(GATTS_TAG, "Notification enable");
                        can_send_notify = true;
                        xSemaphoreGive(gatts_semaphore);
                    }
                }else if (descr_value == 0x0002){
                    if (a_property & ESP_GATT_CHAR_PROP_BIT_INDICATE){
                        ESP_LOGI(GATTS_TAG, "Indication enable");
                        uint8_t indicate_data[600];
                        for (int i = 0; i < sizeof(indicate_data); ++i)
                        {
                            indicate_data[i] = i%0xff;
                        }

                        for (int j = 0; j < 1000; j++) {
                        //the size of indicate_data[] need less than MTU size
                        esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                sizeof(indicate_data), indicate_data, true);
                        }
                    }
                }
                else if (descr_value == 0x0000){
                    can_send_notify = false;
                    a_property = 0;
                    ESP_LOGI(GATTS_TAG, "Notification/Indication disable");
                }else{
                    ESP_LOGE(GATTS_TAG, "unknown descr value");
                    ESP_LOG_BUFFER_HEX(GATTS_TAG, param->write.value, param->write.len);
                }

            }
        }
#endif /* #if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT) */
        example_write_event_env(gatts_if, &a_prepare_write_env, param);
#if (CONFIG_EXAMPLE_GATTC_WRITE_THROUGHPUT)
        if (param->write.handle == gl_profile_tab[PROFILE_A_APP_ID].char_handle) {
            // The last value byte is the checksum data, should used to check the data is received corrected or not.
            if (param->write.value[param->write.len - 1] ==
                check_sum(param->write.value, param->write.len - 1)) {
                write_len += param->write.len;
            }

            if (start == false) {
                start_time = esp_timer_get_time();
                start = true;
                break;
            }
        }
#endif /* #if (CONFIG_EXAMPLE_GATTC_WRITE_THROUGHPUT) */

        break;
    }
    case ESP_GATTS_EXEC_WRITE_EVT:
        ESP_LOGI(GATTS_TAG,"Execute write");
#if (CONFIG_EXAMPLE_GATTC_WRITE_THROUGHPUT)
        if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_CANCEL) {
            if (write_len > a_prepare_write_env.prepare_len) {
                write_len -= a_prepare_write_env.prepare_len;
            } else {
                write_len = 0;
            }
        }
#endif /* #if (CONFIG_EXAMPLE_GATTC_WRITE_THROUGHPUT) */
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        example_exec_write_event_env(&a_prepare_write_env, param);
        break;
    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(GATTS_TAG, "MTU exchange, MTU %d", param->mtu.mtu);
        current_mtu = param->mtu.mtu;  // Update current MTU
        ESP_LOGI(GATTS_TAG, "Current package size: %d bytes, MTU: %d bytes", GATTS_NOTIFY_LEN, current_mtu);
        if (GATTS_NOTIFY_LEN > current_mtu - 3) {  // 3 bytes overhead for notification
            ESP_LOGW(GATTS_TAG, "Package size (%d) too large for MTU (%d)! Will use fragmentation.", GATTS_NOTIFY_LEN, current_mtu);
        } else {
            ESP_LOGI(GATTS_TAG, "Package size fits in MTU - no fragmentation needed");
        }
        
#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
        ESP_LOGI(GATTS_TAG, "NOTIFY throughput benchmark starting - Package size: %d bytes", GATTS_NOTIFY_LEN);
        ESP_LOGI(GATTS_TAG, "Expected bitrate for 400 packages/s: 512 kbps (64 KB/s)");
#endif
        break;
    case ESP_GATTS_UNREG_EVT:
        break;
    case ESP_GATTS_CREATE_EVT:
        ESP_LOGI(GATTS_TAG, "Service create, status %d, service_handle %d", param->create.status, param->create.service_handle);
        gl_profile_tab[PROFILE_A_APP_ID].service_handle = param->create.service_handle;
        gl_profile_tab[PROFILE_A_APP_ID].char_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_A_APP_ID].char_uuid.uuid.uuid16 = REMOTE_NOTIFY_CHAR_UUID;

        esp_ble_gatts_start_service(gl_profile_tab[PROFILE_A_APP_ID].service_handle);
        a_property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        esp_err_t add_char_ret = esp_ble_gatts_add_char(gl_profile_tab[PROFILE_A_APP_ID].service_handle, &gl_profile_tab[PROFILE_A_APP_ID].char_uuid,
                                                        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                                        a_property,
                                                        &gatts_demo_char1_val, NULL);
        if (add_char_ret){
            ESP_LOGE(GATTS_TAG, "add char failed, error code =%x",add_char_ret);
        }
        break;
    case ESP_GATTS_ADD_INCL_SRVC_EVT:
        break;
    case ESP_GATTS_ADD_CHAR_EVT: {
        uint16_t length = 0;
        const uint8_t *prf_char;

        ESP_LOGI(GATTS_TAG, "Characteristic add, status %d, attr_handle %d, service_handle %d",
                param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle);
        gl_profile_tab[PROFILE_A_APP_ID].char_handle = param->add_char.attr_handle;
        gl_profile_tab[PROFILE_A_APP_ID].descr_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_A_APP_ID].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
        esp_err_t get_attr_ret = esp_ble_gatts_get_attr_value(param->add_char.attr_handle,  &length, &prf_char);
        if (get_attr_ret == ESP_FAIL){
            ESP_LOGE(GATTS_TAG, "ILLEGAL HANDLE");
        }

        ESP_LOGI(GATTS_TAG, "the gatts demo char length = %x", length);
        for(int i = 0; i < length; i++){
            ESP_LOGI(GATTS_TAG, "prf_char[%x] =%x",i,prf_char[i]);
        }
        esp_err_t add_descr_ret = esp_ble_gatts_add_char_descr(gl_profile_tab[PROFILE_A_APP_ID].service_handle, &gl_profile_tab[PROFILE_A_APP_ID].descr_uuid,
                                                                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, NULL, NULL);
        if (add_descr_ret){
            ESP_LOGE(GATTS_TAG, "add char descr failed, error code =%x", add_descr_ret);
        }
        break;
    }
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        gl_profile_tab[PROFILE_A_APP_ID].descr_handle = param->add_char_descr.attr_handle;
        ESP_LOGI(GATTS_TAG, "Descriptor add, status %d, attr_handle %d, service_handle %d",
                 param->add_char_descr.status, param->add_char_descr.attr_handle, param->add_char_descr.service_handle);
        break;
    case ESP_GATTS_DELETE_EVT:
        break;
    case ESP_GATTS_START_EVT:
        ESP_LOGI(GATTS_TAG, "Service start, status %d, service_handle %d",
                 param->start.status, param->start.service_handle);
        break;
    case ESP_GATTS_STOP_EVT:
        break;
    case ESP_GATTS_CONNECT_EVT: {
        ESP_LOGI(GATTS_TAG, "Connected, conn_id %d, remote "ESP_BD_ADDR_STR"",
                 param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        gl_profile_tab[PROFILE_A_APP_ID].conn_id = param->connect.conn_id;
        is_connect = true;  // Set connection flag immediately upon connect
        connection_time = xTaskGetTickCount(); // Store connection time
        
#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
        ESP_LOGI(GATTS_TAG, "Connection established - ready to send data when notification enabled");
#endif
        break;
    }
    case ESP_GATTS_DISCONNECT_EVT:
        is_connect = false;
        ESP_LOGI(GATTS_TAG, "Disconnected, remote "ESP_BD_ADDR_STR", reason 0x%02x",
                 ESP_BD_ADDR_HEX(param->disconnect.remote_bda), param->disconnect.reason);
        
#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
        /* Reset notify throughput variables for next connection */
        notify_throughput_started = false;
        notify_start_time = 0;
        notify_sent_packages = 0;
        notify_sent_bytes = 0;
        uMsgIdx = 0;
        can_send_notify = false;  // Reset notification flag
        auto_start_enabled = false;  // Reset auto-start flag
        ESP_LOGI(GATTS_TAG, "NOTIFY throughput variables reset for next connection");
#endif
        
        esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GATTS_CONF_EVT:
        break;
    case ESP_GATTS_OPEN_EVT:
    case ESP_GATTS_CANCEL_OPEN_EVT:
    case ESP_GATTS_CLOSE_EVT:
    case ESP_GATTS_LISTEN_EVT:
        break;
    case ESP_GATTS_CONGEST_EVT:
#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
        if (param->congest.congested) {
            can_send_notify = false;
        } else {
            can_send_notify = true;
            xSemaphoreGive(gatts_semaphore);
        }
#endif /* #if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT) */
        break;
    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        } else {
            ESP_LOGI(GATTS_TAG, "Reg app failed, app_id %04x, status %d",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }

    /* If the gatts_if equal to profile A, call profile A cb handler,
     * so here call each profile's callback */
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            if (gatts_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
                    gatts_if == gl_profile_tab[idx].gatts_if) {
                if (gl_profile_tab[idx].gatts_cb) {
                    gl_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}

#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
void throughput_server_task(void *param)
{
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    ESP_LOGI(GATTS_TAG, "Throughput server task started");

    while(1) {
        if (!can_send_notify) {
            ESP_LOGI(GATTS_TAG, "Waiting for notification enable...");
            int res = xSemaphoreTake(gatts_semaphore, portMAX_DELAY);
            assert(res == pdTRUE);
            ESP_LOGI(GATTS_TAG, "Notification enabled, starting to send data");
        } else {
            if (is_connect) {
                int free_buff_num = esp_ble_get_cur_sendable_packets_num(gl_profile_tab[PROFILE_A_APP_ID].conn_id);
                ESP_LOGI(GATTS_TAG, "is_connect=true, free_buff_num=%d", free_buff_num);
                if(free_buff_num > 0) {
                    for( ; free_buff_num > 0; free_buff_num--) {
                        /* Bitrate control - limit transmission rate to target bitrate */
                        uint64_t current_time_us = esp_timer_get_time();
                        if (last_send_time != 0) {
                            uint64_t time_since_last = current_time_us - last_send_time;
                            uint64_t min_interval_us = (uint64_t)GATTS_NOTIFY_LEN * SECOND_TO_USECOND / TARGET_BITRATE_BPS;
                            
                            if (time_since_last < min_interval_us) {
                                uint64_t delay_us = min_interval_us - time_since_last;
                                vTaskDelay(delay_us / (portTICK_PERIOD_MS * 1000));
                            }
                        }
                        
                        /* Add sample package */
                        memset(bSPPZipSingleFrameFull, 0, sizeof(bSPPZipSingleFrameFull));
                        uMsgIdx++;
                        
                        // Wrap index from 0000 to 9999, then back to 0000
                        if(uMsgIdx > 9999) {
                            uMsgIdx = 0;
                        }
                        
                        uPayloadSize = sizeof("---012345678901234567890123456789012345678901234567890123456789 We start test thoughput mode perfomance missing package happen or not? 987654321098765432109876543210987654321098765432109876543210---\r\n") - 1; // -1 to exclude null terminator
                        
                        // Copy index as ASCII characters "0000" to "9999"
                        sprintf((char*)&bSPPZipSingleFrameFull[0], "%04d", uMsgIdx);
                        
                        memcpy(&bSPPZipSingleFrameFull[4], "---012345678901234567890123456789012345678901234567890123456789 We start test thoughput mode perfomance missing package happen or not? 987654321098765432109876543210987654321098765432109876543210---\r\n", uPayloadSize);
                        tempIndex = 4 + uPayloadSize; // 4 bytes for index + payload size
                        
                        /* Start throughput measurement on first package */
                        if (!notify_throughput_started) {
                            notify_start_time = esp_timer_get_time();
                            notify_throughput_started = true;
                            ESP_LOGI(GATTS_TAG, "NOTIFY throughput measurement started");
                        }
                        
                        /* Check if fragmentation is needed */
                        uint16_t max_payload = current_mtu - 3; // 3 bytes overhead
                        if (tempIndex <= max_payload) {
                            // Send as single package
                            esp_ble_gatts_send_indicate(gl_profile_tab[PROFILE_A_APP_ID].gatts_if, gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                        gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                        tempIndex, bSPPZipSingleFrameFull, false);
                            notify_sent_packages++;
                            notify_sent_bytes += tempIndex;
                        } else {
                            // Fragment the package
                            uint16_t remaining = tempIndex;
                            uint16_t offset = 0;
                            uint16_t fragment_count = (tempIndex + max_payload - 1) / max_payload; // Round up division
                            
                            for (uint16_t frag = 0; frag < fragment_count; frag++) {
                                uint16_t frag_size = (remaining > max_payload) ? max_payload : remaining;
                                
                                esp_ble_gatts_send_indicate(gl_profile_tab[PROFILE_A_APP_ID].gatts_if, gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                            gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                            frag_size, &bSPPZipSingleFrameFull[offset], false);
                                
                                offset += frag_size;
                                remaining -= frag_size;
                                notify_sent_bytes += frag_size;
                            }
                            notify_sent_packages++; // Count as one logical package
                        }
                        
                        /* Update last send time for bitrate control */
                        last_send_time = esp_timer_get_time();
                        
                        /* Log every 100 packages for debugging */
                        if (notify_sent_packages % 100 == 0) {
                            ESP_LOGI(GATTS_TAG, "Sent package #%" PRIu64 ", index: %d, total bytes: %" PRIu64, 
                                     notify_sent_packages, uMsgIdx, notify_sent_bytes);
                        }
                    }
                } else { //Add the vTaskDelay to prevent this task from consuming the CPU all the time, causing low-priority tasks to not be executed at all.
                    vTaskDelay( 10 / portTICK_PERIOD_MS );
                }
            } else {
                ESP_LOGI(GATTS_TAG, "is_connect=false, waiting for connection...");
                 vTaskDelay(300 / portTICK_PERIOD_MS);
            }
        }

    }
}
#endif

#if (CONFIG_EXAMPLE_GATTC_WRITE_THROUGHPUT)
void throughput_cal_task(void *param)
{
    while (1)
    {
        uint32_t bit_rate = 0;
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        if (is_connect && start_time) {
            current_time = esp_timer_get_time();
            bit_rate = write_len * SECOND_TO_USECOND / (current_time - start_time);
            ESP_LOGI(GATTS_TAG, "GATTC write Bit rate = %" PRIu32 " Byte/s, = %" PRIu32 " bit/s, time %d",
                     bit_rate, bit_rate<<3, (int)((current_time - start_time) / SECOND_TO_USECOND));
        }

    }

}
#endif /* #if (CONFIG_EXAMPLE_GATTC_WRITE_THROUGHPUT) */

#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
void notify_bitrate_calc_task(void *param)
{
    while (1)
    {
        uint32_t bit_rate = 0;
        uint32_t package_rate = 0;
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        
        if (is_connect && notify_throughput_started && notify_start_time) {
            notify_current_time = esp_timer_get_time();
            uint64_t elapsed_time = notify_current_time - notify_start_time;
            
            if (elapsed_time > 0) {
                // Calculate byte rate (Bytes/s)
                bit_rate = notify_sent_bytes * SECOND_TO_USECOND / elapsed_time;
                
                // Calculate package rate (packages/s)
                package_rate = notify_sent_packages * SECOND_TO_USECOND / elapsed_time;
                
                ESP_LOGI(GATTS_TAG, "NOTIFY Throughput: %" PRIu32 " Bytes/s, %" PRIu32 " bits/s (%.2f kbps), %" PRIu32 " packages/s [TARGET: %d kbps]", 
                         bit_rate, bit_rate * 8, (float)(bit_rate * 8) / 1000.0, package_rate, TARGET_BITRATE_KBPS);
                ESP_LOGI(GATTS_TAG, "Total sent: %" PRIu64 " packages, %" PRIu64 " bytes, time: %.2f seconds", 
                         notify_sent_packages, notify_sent_bytes, (float)elapsed_time / SECOND_TO_USECOND);
                
                /* Missing package detection info */
                if (notify_sent_packages != uMsgIdx) {
                    ESP_LOGW(GATTS_TAG, "Package count mismatch! Sent: %" PRIu64 ", Index: %d", 
                             notify_sent_packages, uMsgIdx);
                }
            }
        }
    }
}
#endif /* #if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT) */

void app_main(void)
{
    esp_err_t ret;

    // Initialize NVS.
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    #if CONFIG_EXAMPLE_CI_PIPELINE_ID
    // Keep custom device name - comment out to prevent overriding
    // memcpy(test_device_name, esp_bluedroid_get_example_name(), ESP_BLE_ADV_NAME_LEN_MAX);
    #endif

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s initialize controller failed", __func__);
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s enable controller failed", __func__);
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s init bluetooth failed", __func__);
        return;
    }
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s enable bluetooth failed", __func__);
        return;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gatts register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gap register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gatts_app_register(PROFILE_A_APP_ID);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gatts app register error, error code = %x", ret);
        return;
    }

    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(517);
    if (local_mtu_ret){
        ESP_LOGE(GATTS_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
    }

#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
    // The task is only created on the CPU core that Bluetooth is working on,
    // preventing the sending task from using the un-updated Bluetooth state on another CPU.
    xTaskCreatePinnedToCore(&throughput_server_task, "throughput_server_task", 4096, NULL, 15, NULL, BLUETOOTH_TASK_PINNED_TO_CORE);
    xTaskCreatePinnedToCore(&notify_bitrate_calc_task, "notify_bitrate_calc_task", 4096, NULL, 14, NULL, BLUETOOTH_TASK_PINNED_TO_CORE);
    xTaskCreatePinnedToCore(&auto_start_timer_task, "auto_start_timer_task", 2048, NULL, 13, NULL, BLUETOOTH_TASK_PINNED_TO_CORE);
#endif

#if (CONFIG_EXAMPLE_GATTC_WRITE_THROUGHPUT)
    xTaskCreatePinnedToCore(&throughput_cal_task, "throughput_cal_task", 4096, NULL, 14, NULL, BLUETOOTH_TASK_PINNED_TO_CORE);
#endif

#if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT)
    gatts_semaphore = xSemaphoreCreateBinary();
    if (!gatts_semaphore) {
        ESP_LOGE(GATTS_TAG, "%s, init fail, the gatts semaphore create fail.", __func__);
        return;
    }
#endif /* #if (CONFIG_EXAMPLE_GATTS_NOTIFY_THROUGHPUT) */
    return;
}
