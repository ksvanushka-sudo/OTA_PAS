#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "lora.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h" 
#include "esp_wifi.h" 
#include "esp_event.h" 
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h" 
#include "cJSON.h" 
#include "esp_https_ota.h" 
#include "esp_ota_ops.h" 
#include "esp_partition.h" 
#include "esp_task_wdt.h"


#define WIFI_SSID       "Paras 2.4"
#define WIFI_PASS       "9890090011"
#define APP_VERSION     1
#define WIFI_CONNECTED_BIT BIT0
#define BUTTON_PIN      4
#define LONG_PRESS_TICKS 100  // 10 seconds (100 * 100ms)

static const char *TAG = "CLEAN_START";
static EventGroupHandle_t s_wifi_event_group; 

typedef struct {
    bool buttonPressed;
    char display[16];
} struct_message;

static struct_message myData;
static bool lastButtonState = false;

//static bool is_connected = false;

//void start_websever();

// --- 1. WIFI EVENT HANDLER ---
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying connection to AP...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Successfully Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        //is_connected = true;
        //start_webserver();

    }
}
/*
void start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESO_LOGI(TAG, "STARTING WEB SERVER ON PORT : '%d", config.server_port);
    if(httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_get);
    }
}
*/

// --- 2. OTA LOGIC (Only runs after 10s trigger) ---

void start_ota_update(const char *url) {
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = { .http_config = &config };

    ESP_LOGI(TAG, "Starting OTA update... disabling watchdog.");
    esp_task_wdt_delete(NULL); 
    
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Flash successful! Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Flash failed! Error: %d", ret);
        esp_task_wdt_add(NULL); 
    }
}

void check_for_updates() {
    ESP_LOGI(TAG, "Checking GitHub for updates...");
    esp_http_client_config_t config = {
        .url = "https://raw.githubusercontent.com/ksvanushka-sudo/OTA_PAS/main/project.json",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    char *buffer = malloc(1024);
    
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int len = esp_http_client_read(client, buffer, 1024);
        buffer[len] = '\0';

        cJSON *json = cJSON_Parse(buffer);
        if (json) {
            int cloud_version = cJSON_GetObjectItem(json, "version")->valueint;
            ESP_LOGI(TAG, "Cloud: %d | Local: %d", cloud_version, APP_VERSION);

            if (cloud_version > APP_VERSION) {
                ESP_LOGW(TAG, "New version found! Preparing to flash...");
                cJSON *url_item = cJSON_GetObjectItem(json, "file_url");
                if (url_item != NULL && cJSON_IsString(url_item)) {
                    start_ota_update(url_item->valuestring);
                }
            } else {
                ESP_LOGI(TAG, "Already up to date.");
            }
            cJSON_Delete(json);
        }
    }
    esp_http_client_cleanup(client);
    free(buffer);
}


void wifi_init_stable(void) {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// --- 4. MAIN APP ---

void app_main() 
{
    // Global Initializations (Called ONCE)
    ESP_LOGI(TAG, "Step 1: NVS Init...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "Step 2: Netif and Event Loop...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // GPIO Setup
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);

    // Watchdog
    esp_task_wdt_config_t twdt_config = { 
        .timeout_ms = 30000, 
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, 
        .trigger_panic = true };
        
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&twdt_config);
    if (wdt_err == ESP_ERR_INVALID_STATE) {
        // If reconfigure failed because it wasn't init yet, init it now
        ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
    }
        
    //ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
    esp_task_wdt_add(NULL); 

    // YOUR ORIGINAL LORA LOGIC (Untouched)
    ESP_LOGI(TAG, "Step 3: LoRa Init (Check wiring if it stops here!)...");
    lora_init();
    lora_set_frequency(865e6);
    lora_set_spreading_factor(11);
    lora_set_bandwidth(125e3);
    lora_set_coding_rate(5);
    lora_set_sync_word(0xF3);
    lora_set_tx_power(17);
    lora_enable_crc();
    lora_explicit_header_mode();

    // OTA Rollback Check
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback(); 
            ESP_LOGW(TAG, "Update confirmed and finalized.");
        }
    }
    ESP_LOGI(TAG, "Step 5: Entering Main Loop...");
    int press_ticks = 0;

    while (1) {
        bool currentButtonState = !gpio_get_level(BUTTON_PIN);
        esp_task_wdt_reset();

        if (currentButtonState) {
            press_ticks++;
            
            // If 15 seconds reached
            if (press_ticks == LONG_PRESS_TICKS) {
                ESP_LOGW(TAG, "10s reached! Checking for updates...");
                wifi_init_stable();
                
                // Wait for Wi-Fi connection
                EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
                if (bits & WIFI_CONNECTED_BIT) {
                    check_for_updates();
                }

                // If no update happened, turn Wi-Fi off and cleanup
                esp_wifi_stop();
                esp_wifi_deinit();
                vEventGroupDelete(s_wifi_event_group);
                press_ticks = 0; // Reset
            }
        } else {
            // Button was released - was it a normal LoRa press?
            if (press_ticks > 0 && press_ticks < 5) {
                // YOUR ORIGINAL LORA SEND LOGIC
                myData.buttonPressed = true;
                strcpy(myData.display, "A104");
                lora_send_packet((uint8_t *)&myData, sizeof(myData));
                ESP_LOGI(TAG, "Button Pressed - LoRa Data Sent");
            }
            press_ticks = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
