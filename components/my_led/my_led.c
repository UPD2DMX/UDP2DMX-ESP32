#include "my_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" // WICHTIG: für SemaphoreHandle_t etc.
#include "driver/gpio.h"

static int led_gpio = 2;
static volatile bool wifi_connected = false;
static volatile bool dmx_error = false;
static volatile connection_type_t connection_type = CONNECTION_TYPE_NONE;
static SemaphoreHandle_t blink_mutex;
static int blink_count = 0;
static int blink_delay = 0;

// LED-Status-Task
static void led_status_task(void *arg)
{
    while (1)
    {
        // User action (blinking sequence) has priority
        if (blink_count > 0)
        {
            for (int i = 0; i < blink_count; i++)
            {
                gpio_set_level(led_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(blink_delay));
                gpio_set_level(led_gpio, 0);
                vTaskDelay(pdMS_TO_TICKS(blink_delay));
            }

            vTaskDelay(pdMS_TO_TICKS(700));
            blink_count = 0;
        }
        // DMX error has higher priority than connection status
        else if (dmx_error)
        {
            // Fast blinking: 100ms on / 100ms off
            gpio_set_level(led_gpio, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(led_gpio, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        // Connected (any type: WiFi or LAN): LED off
        else if (wifi_connected || connection_type == CONNECTION_TYPE_LAN)
        {
            gpio_set_level(led_gpio, 0);  // LED aus
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        // Not connected: Slow blinking
        else
        {
            // Slow blinking: 500ms on / 500ms off
            gpio_set_level(led_gpio, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(led_gpio, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

// Initialisierung
void my_led_init(int gpio)
{
    led_gpio = gpio;
    blink_mutex = xSemaphoreCreateMutex();

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << led_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io_conf);

    gpio_set_level(led_gpio, 1); // LED aus

    xTaskCreate(led_status_task, "led_status_task", 2048, NULL, 2, NULL);
}

// Benutzeraktion (z. B. WLAN-Wechsel) signalisieren
void my_led_blink(int count, int delay_ms)
{
    if (!blink_mutex)
    {
        blink_mutex = xSemaphoreCreateMutex();
    }

    if (xSemaphoreTake(blink_mutex, 0) == pdTRUE)
    {
        blink_count = count;
        blink_delay = delay_ms;
        xSemaphoreGive(blink_mutex);
    }
}

// Manuell setzen (optional)
void my_led_set(bool on)
{
    gpio_set_level(led_gpio, on ? 1 : 0);
}

// WLAN-Status aktualisieren
void my_led_set_wifi_status(bool connected)
{
    if (connected == true)
        my_led_blink(2, 50);
    wifi_connected = connected;
}

// Verbindungstyp aktualisieren (LAN/WiFi)
void my_led_set_connection_type(connection_type_t type)
{
    connection_type = type;
    
    // Signal die Verbindungsart: 3x blink für LAN, 2x für WiFi
    if (type == CONNECTION_TYPE_LAN)
    {
        my_led_blink(3, 80);  // 3 blinks für LAN
    }
    else if (type == CONNECTION_TYPE_WIFI)
    {
        my_led_blink(2, 80);  // 2 blinks für WiFi
    }
}

// DMX-Fehlerstatus aktualisieren
void my_led_set_dmx_error(bool error)
{
    dmx_error = error;
}
