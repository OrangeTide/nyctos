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

/*
 * The receive side. The 16550 raises its interrupt as soon as a byte is
 * in the holding register, so there is no trigger level to fall short of
 * and no timeout interrupt to go with it.
 */
#define UART0_RBR (*(volatile unsigned char *)(UART0_BASE + 0x00)) /* receive buffer */
#define UART0_IER (*(volatile unsigned char *)(UART0_BASE + 0x01)) /* interrupt enable */
#define UART_IER_RX   (1U << 0) /* received data available */
#define UART_LSR_DR   (1U << 0) /* data ready */

void
uart_rx_enable(void)
{
    UART0_IER |= UART_IER_RX;
}

int
uart_getc(void)
{
    if (!(UART0_LSR & UART_LSR_DR)) {
        return -1;
    }

    return (int)UART0_RBR;
}
