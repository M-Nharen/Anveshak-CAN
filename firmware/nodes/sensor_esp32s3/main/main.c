/* ESP32-S3 node template: wires acc_can_twai and anveshak_can_core together and sends
 * heartbeats and an example sensor frame (CLAUDE.md / prompts/A2, "ESP32-S3 node template").
 *
 * NODE_HB is already generic across every node type (anv_node_tick() sends it at the
 * configured rate; see anv_node.c's design note), so there is nothing extra to do for
 * heartbeats here. There is no science/auxiliary sensor message in defs/rover.dbc yet (only
 * drive and FPGA node types have one) -- adding one is a defs/ change for a future prompt, so
 * this template reuses FPGA_SENSORS_A's generic "four u16 values" layout the same way, with
 * the real CAN ID computed from this node's own ID via ids.h. Replace read_example_sensors()
 * with real ADC/I2C/SPI reads for an actual science payload.
 */
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "acc_can_twai.h"
#include "anv_node.h"
#include "board_config.h"
#include "ids.h"
#include "rover.h"

#define SENSOR_NODE_TYPE 3u /* NODE_HB's NODE_TYPE: 3 science (docs/architecture.md) */
#define SENSOR_PROTOCOL_VERSION 1u
#define SENSOR_TXQ_CAP_PER_PRIO 8u
#define SENSOR_HB_RATE_HZ 10u
#define SENSOR_FRAME_RATE_HZ 10u

static acc_can_twai_ctx_t s_can_ctx;
static anv_node_t s_node;
static acc_can_frame_t s_txq_storage[ACC_CAN_PRIO_COUNT * SENSOR_TXQ_CAP_PER_PRIO];
static anv_param_store_t s_param_store; /* empty: this node type has no tunable parameters yet */
static anv_rate_sched_t s_sensor_sched;

static uint32_t board_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void board_set_outputs_off(void *ctx)
{
    (void)ctx; /* no actuators on a sensor node */
}

static void board_feed_hw_watchdog(void *ctx)
{
    (void)ctx; /* TODO: esp_task_wdt_reset(), once this app registers with the task watchdog */
}

static uint8_t board_read_node_id(void *ctx)
{
    (void)ctx;
    uint8_t id = 0;
    if (gpio_get_level(SENSOR_NODE_ID_PIN0)) {
        id |= (1u << 0);
    }
    if (gpio_get_level(SENSOR_NODE_ID_PIN1)) {
        id |= (1u << 1);
    }
    if (gpio_get_level(SENSOR_NODE_ID_PIN2)) {
        id |= (1u << 2);
    }
    if (gpio_get_level(SENSOR_NODE_ID_PIN3)) {
        id |= (1u << 3);
    }
    if (gpio_get_level(SENSOR_NODE_ID_PIN4)) {
        id |= (1u << 4);
    }
    return id;
}

static const anv_board_hooks_t s_board_hooks = {
    .ctx = NULL,
    .read_node_id = board_read_node_id,
    .set_outputs_off = board_set_outputs_off,
    .feed_hw_watchdog = board_feed_hw_watchdog,
    .now_ms = board_now_ms,
    .enter_bootloader = NULL, /* OTA entry: out of scope (CLAUDE.md A2) */
    .request_reset = NULL,
    .storage_load = NULL, /* NVS-backed parameter storage: left for a later prompt */
    .storage_save = NULL,
};

static void board_gpio_init(void)
{
    gpio_config_t id_switch = {
        .pin_bit_mask = (1ULL << SENSOR_NODE_ID_PIN0) | (1ULL << SENSOR_NODE_ID_PIN1) |
                        (1ULL << SENSOR_NODE_ID_PIN2) | (1ULL << SENSOR_NODE_ID_PIN3) |
                        (1ULL << SENSOR_NODE_ID_PIN4),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&id_switch);

    gpio_config_t stb = {
        .pin_bit_mask = (1ULL << SENSOR_TCAN_STB_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&stb);
    gpio_set_level(SENSOR_TCAN_STB_GPIO, 0);
}

static void read_example_sensors(uint16_t *out4)
{
    static uint16_t counter;
    counter++;
    out4[0] = counter;
    out4[1] = (uint16_t)(counter * 2u);
    out4[2] = (uint16_t)(counter * 3u);
    out4[3] = (uint16_t)(counter * 4u);
}

static void send_example_sensor_frame(uint8_t node_id)
{
    uint16_t values[4];
    read_example_sensors(values);

    struct rover_fpga_sensors_a_t msg = {
        .sensor_1 = values[0],
        .sensor_2 = values[1],
        .sensor_3 = values[2],
        .sensor_4 = values[3],
    };
    acc_can_frame_t frame;
    frame.id = ROVER_CAN_ID(ROVER_CLASS_SENSOR, node_id, 0u);
    int packed = rover_fpga_sensors_a_pack(frame.data, &msg, sizeof(frame.data));
    if (packed >= 0) {
        frame.dlc = (uint8_t)packed;
        (void)anv_node_send(&s_node, ACC_CAN_PRIO_TELEMETRY, &frame);
    }
}

void app_main(void)
{
    board_gpio_init();
    uint8_t node_id = board_read_node_id(NULL);

    anv_param_store_init(&s_param_store, NULL, NULL, 0, &s_board_hooks);

    acc_can_driver_t can_driver = acc_can_twai_make_driver(&s_can_ctx, SENSOR_CAN_TX_GPIO, SENSOR_CAN_RX_GPIO);

    /* Two hardware mask filters (docs/architecture.md, "Hardware filters on nodes"): anything
     * addressed to this node (mask 0x0F8, the node field), plus all of class 0 (mask 0x700). */
    acc_can_filter_t filters[2];
    filters[0].id = (uint16_t)(node_id << 3);
    filters[0].mask = 0x0F8u;
    filters[1].id = 0x000u;
    filters[1].mask = 0x700u;
    (void)can_driver.init(can_driver.ctx, 500000u, filters, 2u);

    anv_node_info_t info = {0};
    uint32_t boot_ms = board_now_ms(NULL);
    anv_node_init(&s_node, node_id, SENSOR_NODE_TYPE, SENSOR_PROTOCOL_VERSION, &can_driver, s_txq_storage,
                  SENSOR_TXQ_CAP_PER_PRIO, &s_param_store, &info, &s_board_hooks, boot_ms);
    anv_node_boot_complete(&s_node);
    anv_node_set_hb_rate(&s_node, SENSOR_HB_RATE_HZ, boot_ms);
    anv_rate_sched_init(&s_sensor_sched, SENSOR_FRAME_RATE_HZ, boot_ms);

    for (;;) {
        acc_can_frame_t frame;
        while (can_driver.receive(can_driver.ctx, &frame)) {
            anv_node_on_rx_frame(&s_node, &frame, board_now_ms(NULL));
        }

        uint32_t now_ms = board_now_ms(NULL);
        anv_node_tick(&s_node, now_ms);
        if (anv_rate_sched_due(&s_sensor_sched, now_ms)) {
            send_example_sensor_frame(node_id);
        }
        (void)anv_node_drain_tx(&s_node);

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
