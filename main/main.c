#include <stdio.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// define pin nums
#define PIN_SYNC 13
#define PIN_IN_A 14
#define PIN_IN_B 25
#define PIN_IN_S 26
#define PIN_IN_MODE 27 

static const char *TAG = "main";

// global state flags
static bool mode = true; // pin in mode bool 

// configure gpio pin
void gpio_init(void)
{
    // build structure for input of pins (plain first)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_SYNC) | 
                        (1ULL << PIN_IN_A) | 
                        (1ULL << PIN_IN_B) | 
                        (1ULL << PIN_IN_S) | 
                        (1ULL << PIN_IN_MODE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   // or ENABLE if needed
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,      // No interrupts initially
    };

    gpio_config(&io_conf);

    // per pin settings
    gpio_set_pull_mode(PIN_IN_S, GPIO_PULLDOWN_ENABLE);
    gpio_set_pull_mode(PIN_IN_MODE, GPIO_PULLDOWN_ENABLE);
    gpio_set_pull_mode(PIN_IN_A, GPIO_FLOATING);
    gpio_set_pull_mode(PIN_IN_B, GPIO_FLOATING);
}

static void check_mode(void)
{
    static uint8_t lastReading = 0;
    static bool toggled = false;

    uint8_t reading = (uint8_t)gpio_get_level(PIN_IN_MODE);

    if (lastReading == 0 && reading == 1) { // if last AND read is
        toggled = !toggled;                 // flip state
        mode = toggled;                     // update output state
        ESP_LOGI(TAG, "Button1 is: %s", toggled ? "State_A" : "State_B");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    lastReading = reading; // update var
    printf("Mode toggled: %u\n", reading);
}

static void check_a(void)
{
    int in_a_level = gpio_get_level(PIN_IN_A);

    // display gpio val
    printf("IN_A: %d\n", in_a_level);
        
    vTaskDelay(pdMS_TO_TICKS(1));  // Delay 1 tick
}

static void check_sporadic(void)
{
    return;
}

void app_main(void)
{
    // run gpio init and start logic
    ESP_LOGI(TAG, "Starting");
    gpio_init();

    printf("GPIO Input Monitor Started\n");

    while(true) {
        check_mode();
        check_sporadic();
        check_a();
    }
}
