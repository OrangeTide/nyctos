/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The device, as this driver sees it.
 *
 * Section 19.2 gives a driver a grant covering its registers, and that is
 * now what it has: an address it was handed, at run time, by whatever
 * started it. The driver holds no address of its own and cannot reach a
 * device nobody gave it, which at Tier 2 is enforced and at Tier 0 is the
 * same statement without a mechanism behind it.
 *
 * uart_attach is called once, before anything else here, with the base of
 * the region the grant covers. Everything below is an offset from it.
 */

#ifndef _CONSOLE_UART_H_
#define _CONSOLE_UART_H_

#include <stdint.h>

/* The registers this driver was given. Called once, before any of the
 * rest, and the length is checked against what the device needs. */
int uart_attach(uintptr_t base, uintptr_t len);

/* Let the device raise an interrupt when a byte arrives. */
void uart_rx_enable(void);

/* One byte, or -1 when the receive FIFO is empty. */
int uart_getc(void);

/* Polled, at the priority of whoever asked. Section 22.4 leaves the
 * kernel writing to the same device for panics and for boot. */
void uart_putc(char c);

#endif /* _CONSOLE_UART_H_ */
