/* Pin assignments for the ESP32-S3 node template. One file to edit when wiring this firmware
 * to a real PCB -- nothing else in main.c names a pin. Placeholder values; replace with the
 * real board's assignment. */
#ifndef SENSOR_BOARD_CONFIG_H
#define SENSOR_BOARD_CONFIG_H

#include "driver/gpio.h"

/* TWAI TX/RX pins. */
#define SENSOR_CAN_TX_GPIO GPIO_NUM_17
#define SENSOR_CAN_RX_GPIO GPIO_NUM_18

/* TCAN1044AV STB, driven low at start-up so the transceiver leaves standby
 * (docs/architecture.md "Physical layer"; CLAUDE.md / prompts/A2 "TCAN1044AV handling"). */
#define SENSOR_TCAN_STB_GPIO GPIO_NUM_8

/* 5-bit node ID switch. Science/auxiliary nodes are IDs 16-19 (docs/architecture.md,
 * "Identifier scheme"); an unreadable or zero switch falls back to 31 (unconfigured). */
#define SENSOR_NODE_ID_PIN0 GPIO_NUM_4
#define SENSOR_NODE_ID_PIN1 GPIO_NUM_5
#define SENSOR_NODE_ID_PIN2 GPIO_NUM_6
#define SENSOR_NODE_ID_PIN3 GPIO_NUM_7
#define SENSOR_NODE_ID_PIN4 GPIO_NUM_15

#endif /* SENSOR_BOARD_CONFIG_H */
