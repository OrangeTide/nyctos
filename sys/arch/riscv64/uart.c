/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/systm.h>

/* QEMU 'virt' machine: 16550A-compatible UART at 0x10000000. */
#define UART0_BASE 0x10000000UL

#define UART0_THR (*(volatile unsigned char *)(UART0_BASE + 0x00)) /* transmit holding reg */
#define UART0_LSR (*(volatile unsigned char *)(UART0_BASE + 0x05)) /* line status reg */
#define UART_LSR_THRE (1U << 5) /* transmit holding register empty */

void
uart_putc(char c)
{
    while (!(UART0_LSR & UART_LSR_THRE)) {
        /* wait for the transmitter to be ready */
    }
    UART0_THR = (unsigned char)c;
}

