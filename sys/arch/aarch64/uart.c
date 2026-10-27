/* uart.c : AArch64 PL011 console output.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * uart_putc writes one byte to the PL011 data register at 0x09000000,
 * the UART0 of QEMU's virt board. It waits while the transmit FIFO is
 * full. This file has no receive path.
 *
 */

#include <sys/systm.h>

/* QEMU 'virt' machine: PL011 UART0 at 0x09000000. */
#define UART0_BASE 0x09000000UL

#define UART0_DR (*(volatile unsigned int *)(UART0_BASE + 0x00))
#define UART0_FR (*(volatile unsigned int *)(UART0_BASE + 0x18))
#define UART_FR_TXFF (1U << 5) /* transmit FIFO full */

void
uart_putc(char c)
{
    while (UART0_FR & UART_FR_TXFF) {
        /* wait for space in the transmit FIFO */
    }
    UART0_DR = (unsigned int)(unsigned char)c;
}

