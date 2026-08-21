/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/* The PL011 on QEMU's virt board. See uart.h for what this is allowed to
 * touch and why the address is compiled in. */

#include "uart.h"

#define UART0_BASE 0x09000000UL

#define UART0_DR   (*(volatile unsigned int *)(UART0_BASE + 0x00))
#define UART0_FR   (*(volatile unsigned int *)(UART0_BASE + 0x18))
#define UART0_IMSC (*(volatile unsigned int *)(UART0_BASE + 0x38))

#define FR_RXFE    (1U << 4)    /* receive FIFO empty */
#define FR_TXFF    (1U << 5)    /* transmit FIFO full */
#define INT_RX     (1U << 4)
#define INT_RT     (1U << 6)    /* receive timeout: a FIFO that stopped filling */

void
uart_rx_enable(void)
{
    UART0_IMSC |= INT_RX | INT_RT;
}

int
uart_getc(void)
{
    if (UART0_FR & FR_RXFE) {
        return -1;
    }

    return (int)(UART0_DR & 0xff);
}

void
uart_putc(char c)
{
    while (UART0_FR & FR_TXFF) {
        /* wait for space in the transmit FIFO */
    }
    UART0_DR = (unsigned int)(unsigned char)c;
}
