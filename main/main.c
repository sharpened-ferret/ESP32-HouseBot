#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "discord.h"
#include "discord/session.h"
#include "discord/message.h"
#include "esp_sntp.h"
#include "esp_sleep.h"
#include <time.h>

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* WiFi config settings */
#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PWD CONFIG_WIFI_PASSWORD
#define MAXIMUM_RETRY CONFIG_MAXIMUM_RETRY

// Discord connection settings
#define CHANNEL_ID CONFIG_CHANNEL_ID
// #define CONNECTION_MSG_ENABLED CONFIG_CONNECTION_MESSAGE_ENABLED
#define CONNECTION_MSG CONFIG_CONNECTION_MESSAGE
#define REMINDER_MSG CONFIG_REMINDER_MESSAGE

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

static bool BOT_CONNECTED = false;
static bool CONNECTION_MSG_ENABLED = false;
//static bool RESTART_FLAG = false;

static discord_handle_t bot;
static discord_message_t reminder_msg = {
    .content = REMINDER_MSG,
    .channel_id = CHANNEL_ID
};

static const char *TAG = "Server";

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// WiFi Initialisation
void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PWD,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PWD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PWD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

static void time_check() {
    time_t now;
    char strftime_buf[64];
    struct tm timeinfo;

    time(&now);
    // Set timezone to China Standard Time
    setenv("GB", "UTC+0", 1);
    tzset();

    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in the UK is: %s", strftime_buf);
    struct tm *tm_time = localtime(&now);
    int curr_day = tm_time->tm_wday;
    unsigned long long int sleep_delay;
    ESP_LOGI(TAG, "Current Day: %d", curr_day);
    if (curr_day == 2) {
        if (tm_time->tm_hour < 18) {
            sleep_delay = (18ULL - tm_time->tm_hour) * 60 * 60 * 1000 * 1000;
            esp_sleep_enable_timer_wakeup(sleep_delay);
            ESP_LOGI(TAG, "Sleeping for %llu seconds", (sleep_delay/1000000));
            esp_deep_sleep_start();
        } else {
            ESP_LOGI(TAG, "Sending Reminder");
            discord_message_send(bot, &reminder_msg, NULL);
            sleep_delay = 12ULL * 60 * 60 * 1000 * 1000;
            ESP_LOGI(TAG, "Sleeping for %llu seconds", (sleep_delay/1000000));
            esp_deep_sleep_start();
        }
    } else {
        sleep_delay = 12ULL * 60 * 60 * 1000 * 1000;
        ESP_LOGI(TAG, "Sleeping for %llu seconds", (sleep_delay/1000000));
        esp_sleep_enable_timer_wakeup(sleep_delay);
    }
}

// Basic Bot Handler
static void bot_discord_event_handler(void* handler_arg, esp_event_base_t base, int32_t event_id, void* event_data) {
    discord_event_data_t* data = (discord_event_data_t*) event_data;

    switch (event_id) {
        case DISCORD_EVENT_CONNECTED: {
                discord_session_t* session = (discord_session_t*) data->ptr;
                ESP_LOGI("BOT", "Bot %s#%s connected", session->user->username, session->user->discriminator);
                BOT_CONNECTED = true;
                if (CONNECTION_MSG_ENABLED) {
                    discord_message_t connectionMsg = {
                        .content = CONNECTION_MSG,
                        .channel_id = CHANNEL_ID
                    };
                    discord_message_send(bot, &connectionMsg, NULL);
                }
                time_check();
            }
            break;

        case DISCORD_EVENT_DISCONNECTED: {
                BOT_CONNECTED = false;
                ESP_LOGI("BOT", "Bot Disconnected");
            }
            break;
    }
}

void app_main(void)
{
    //Initialise NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    printf("Boot Triggered\n");

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    discord_config_t cfg = {
        .intents = DISCORD_INTENT_GUILD_MESSAGES
    };

    bot = discord_create(&cfg);
    discord_register_events(bot, DISCORD_EVENT_ANY, bot_discord_event_handler, NULL);
    discord_login(bot);
    printf("Bot created!\n");
    
    // if (BOT_CONNECTED) {
        
    // }
}