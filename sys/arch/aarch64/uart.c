/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
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

/*
 * The receive side. RXIM is the FIFO reaching its trigger level and RTIM
 * is the timeout that covers a FIFO which has bytes in it and has stopped
 * filling, which is what a person typing looks like. Without the second, a
 * line shorter than the trigger level arrives nowhere.
 */
#define UART0_IMSC (*(volatile unsigned int *)(UART0_BASE + 0x38))
#define UART0_ICR  (*(volatile unsigned int *)(UART0_BASE + 0x44))
#define UART_FR_RXFE (1U << 4)  /* receive FIFO empty */
#define UART_INT_RX  (1U << 4)  /* receive */
#define UART_INT_RT  (1U << 6)  /* receive timeout */

void
uart_rx_enable(void)
{
    /*
     * Unmasked without clearing first. The interrupt is latched when
     * the byte arrives rather than recomputed from the FIFO, so a byte
     * that arrived before anyone was listening is delivered by the
     * unmasking and lost by a clear.
     */
    UART0_IMSC |= UART_INT_RX | UART_INT_RT;
}

int
uart_getc(void)
{
    if (UART0_FR & UART_FR_RXFE) {
        return -1;
    }

    return (int)(UART0_DR & 0xff);
}
