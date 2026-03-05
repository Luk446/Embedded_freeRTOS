#include <stdio.h>
#include "driver/gpio.h"

// define pin nums
#define PIN_SYNC 0
#define PIN_IN_A 1
#define PIN_IN_B 3
#define PIN_IN_S 4
#define PIN_IN_MODE 5 

// configure gpio pin
void gpio_init(void)
{
    // build structure for input of pins
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
}


void app_main(void)
{
    printf("Hello world!\n");
}
