/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/* The 16550 on QEMU's virt board. See uart.h for what this is allowed to
 * touch and why the address is compiled in. */

#include "uart.h"

#define UART0_BASE 0x10000000UL

#define UART0_RBR  (*(volatile unsigned char *)(UART0_BASE + 0x00))
#define UART0_THR  (*(volatile unsigned char *)(UART0_BASE + 0x00))
#define UART0_IER  (*(volatile unsigned char *)(UART0_BASE + 0x01))
#define UART0_LSR  (*(volatile unsigned char *)(UART0_BASE + 0x05))

#define IER_RX     (1U << 0)    /* received data available */
#define LSR_DR     (1U << 0)    /* data ready */
#define LSR_THRE   (1U << 5)    /* transmit holding register empty */

void
uart_rx_enable(void)
{
    UART0_IER |= IER_RX;
}

int
uart_getc(void)
{
    if (!(UART0_LSR & LSR_DR)) {
        return -1;
    }

    return (int)UART0_RBR;
}

void
uart_putc(char c)
{
    while (!(UART0_LSR & LSR_THRE)) {
        /* wait for the transmitter to be ready */
    }
    UART0_THR = (unsigned char)c;
}
