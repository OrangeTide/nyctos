/* uart.c : RISC-V 16550 UART console output.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * uart_putc writes one byte to the transmit holding register at
 * 0x10000000, the UART of QEMU's virt board. It waits for the transmit
 * holding register empty bit in the line status register first. This
 * file has no receive path.
 *
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

