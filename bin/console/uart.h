/* uart.h : interface of the console UART driver.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares the four entry points of the console UART driver: attach to a
 * register region, enable receive interrupts, read one byte, and write one
 * byte. The register region is supplied at run time by whatever started the
 * driver, so the driver holds no device address of its own.
 *
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
