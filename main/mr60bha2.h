#pragma once
/*
 * MR60BHA2 – 60 GHz mmWave Breathing & Heartbeat Module Pro (Seeed Studio)
 *
 * Protocol (UART 115200 8N1):
 *   [0x55][LEN_H][LEN_L][TYPE][CMD][DATA…][CRC]
 *   LEN   = number of bytes from TYPE through last DATA byte (big-endian)
 *   CRC   = XOR of all bytes from TYPE through last DATA byte
 *
 * Shares mr60_data_t / mr60_callback_t from mr60bha1.h so main.c only
 * needs to swap the include and function prefix.
 */
#include "mr60bha1.h"   /* re-uses mr60_data_t, mr60_callback_t, MR60_* constants */

esp_err_t mr60bha2_init(uart_port_t port, int tx_pin, int rx_pin,
                         mr60_callback_t cb);
void      mr60bha2_deinit(void);
void      mr60bha2_get(mr60_data_t *out);
bool      mr60bha2_ready(void);
