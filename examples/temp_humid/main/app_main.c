/**
 * Brief
 * This example shows how to connect an ESP device to the Bytebeam cloud.
 * This is good starting point if you are looking to remotely update your ESP device
   or push a command to it or stream data from it and visualise it on the Bytebeam Cloud.
*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_tls.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "cJSON.h"

#include "driver/gpio.h"
#include "driver/i2c.h"

#include "bytebeam_sdk.h"

#include "sht31.h"

// this macro is used to specify the delay for 1 sec.
#define APP_DELAY_ONE_SEC 1000u

static int config_publish_period = APP_DELAY_ONE_SEC;

static float temperature = 0.0;
static float humidity = 90;

static char sht_stream[] = "sht_stream";

static bytebeam_client_t bytebeam_client;

static const char *TAG = "BYTEBEAM_TEMP_HUMID_EXAMPLE";

static void get_sht_values(void)
{
    int ret_val;

    ret_val = sht31_read_temp_humi(&temperature, &humidity);
    
    if(ret_val != 0)
    {
        ESP_LOGE(TAG, "Failed to read sht values.");
    }
    
    ESP_LOGI(TAG, "SHT3x Sensor : %.2f °C, %.2f %% \n",temperature, humidity);
}

static void sht_init(void)
{
    int ret_val;

    ret_val = sht31_init();

    if(ret_val != 0)
    {
        ESP_LOGE(TAG, "Failed to initialized sht.");
    }
}

static int publish_sht_values(bytebeam_client_t *bytebeam_client)
{
    struct timeval te;
    long long milliseconds = 0;
    static uint64_t sequence = 0;

    cJSON *device_shadow_json_list = NULL;
    cJSON *device_shadow_json = NULL;
    cJSON *sequence_json = NULL;
    cJSON *timestamp_json = NULL;
    cJSON *device_status_json = NULL;
    cJSON *temperature_json = NULL;
    cJSON *humidity_json = NULL;

    char *string_json = NULL;

    device_shadow_json_list = cJSON_CreateArray();

    if (device_shadow_json_list == NULL)
    {
        ESP_LOGE(TAG, "Json Init failed.");
        return -1;
    }

    device_shadow_json = cJSON_CreateObject();

    if (device_shadow_json == NULL)
    {
        ESP_LOGE(TAG, "Json add failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    // get current time
    gettimeofday(&te, NULL);
    milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000;

    timestamp_json = cJSON_CreateNumber(milliseconds);

    if (timestamp_json == NULL)
    {
        ESP_LOGE(TAG, "Json add time stamp failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    cJSON_AddItemToObject(device_shadow_json, "timestamp", timestamp_json);

    sequence++;
    sequence_json = cJSON_CreateNumber(sequence);

    if (sequence_json == NULL)
    {
        ESP_LOGE(TAG, "Json add sequence id failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    cJSON_AddItemToObject(device_shadow_json, "sequence", sequence_json);

    // get tempearture and humidity samples
    get_sht_values();

    temperature_json = cJSON_CreateNumber(temperature);

    if (temperature_json == NULL)
    {
        ESP_LOGE(TAG, "Json add temperature failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    
    cJSON_AddItemToObject(device_shadow_json, "temperature", temperature_json);
    
    humidity_json = cJSON_CreateNumber(humidity);

    if (humidity_json == NULL)
    {
        ESP_LOGE(TAG, "Json add humidity failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    
    cJSON_AddItemToObject(device_shadow_json, "humidity", humidity_json);

    cJSON_AddItemToArray(device_shadow_json_list, device_shadow_json);

    string_json = cJSON_Print(device_shadow_json_list);

    if(string_json == NULL)
    {
        ESP_LOGE(TAG, "Json string print failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    ESP_LOGI(TAG, "\nStatus to send:\n%s\n", string_json);

    // publish the json to sht stream
    int ret_val = bytebeam_publish_to_stream(bytebeam_client, sht_stream, string_json);

    cJSON_Delete(device_shadow_json_list);
    cJSON_free(string_json);

    return ret_val;
}

static void app_start(bytebeam_client_t *bytebeam_client)
{
    int ret_val = 0;

    while (1)
    {   
        // publish sht values
        ret_val = publish_sht_values(bytebeam_client);

        if (ret_val != 0)
        {
            ESP_LOGE(TAG, "Failed to publish sht values.");
        }

        vTaskDelay(config_publish_period / portTICK_PERIOD_MS);
    }
}

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);

#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif

    sntp_init();
}

static void sync_time_from_ntp(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    initialize_sntp();

    // wait for time to be set
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    time(&now);
    localtime_r(&now, &timeinfo);
}


void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */

    ESP_ERROR_CHECK(example_connect());

    // sync time from the ntp
    sync_time_from_ntp();

    // initialize the sht module
    sht_init();
    
    // setting up the device info i.e to be seen in the device shadow
    bytebeam_client.device_info.status           = "Device is Up!";
    bytebeam_client.device_info.software_type    = "temp-humid-app";
    bytebeam_client.device_info.software_version = "1.0.0";
    bytebeam_client.device_info.hardware_type    = "ESP32 DevKit V1";
    bytebeam_client.device_info.hardware_version = "rev1";

    // initialize the bytebeam client
    bytebeam_init(&bytebeam_client);

    // start the bytebeam client
    bytebeam_start(&bytebeam_client);

    //
    // start the main application
    //
    app_start(&bytebeam_client);
}
