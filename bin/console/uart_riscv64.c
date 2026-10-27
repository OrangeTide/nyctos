/* uart_riscv64.c : 16550 UART driver for the RISC-V virt board.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Drives the 16550 on QEMU's virt machine. The register base is given at
 * run time through uart_attach, and every register access is an offset from
 * that base. The entry points are declared in uart.h.
 *
 */

#include "uart.h"

#define UART_SPAN  0x1000UL

#define UART0_RBR  (*(volatile unsigned char *)(base + 0x00))
#define UART0_THR  (*(volatile unsigned char *)(base + 0x00))
#define UART0_IER  (*(volatile unsigned char *)(base + 0x01))
#define UART0_LSR  (*(volatile unsigned char *)(base + 0x05))

#define IER_RX     (1U << 0)    /* received data available */
#define LSR_DR     (1U << 0)    /* data ready */
#define LSR_THRE   (1U << 5)    /* transmit holding register empty */

/*
 * Where the registers are, as the grant said. Zero until the driver has
 * been given them, which is a state that lasts for a few instructions at
 * startup and never comes back.
 */
static uintptr_t base;

int
uart_attach(uintptr_t at, uintptr_t len)
{
    if (at == 0 || len < UART_SPAN) {
        return -1;
    }

    base = at;

    return 0;
}

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
