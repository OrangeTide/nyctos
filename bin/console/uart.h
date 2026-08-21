/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The device, as this driver sees it.
 *
 * Section 19.2 gives a driver a grant covering its registers. There is no
 * grant object until milestone 4, and Tier 0 has no address space to grant
 * against, so what the driver has instead is the address compiled into it:
 * a region it may already reach, which is what section 6.1 says the weaker
 * tier means by permission. The authority that is a handle here is the
 * interrupt, and the register base is a deviation with a milestone against
 * it rather than a decision.
 */

#ifndef _CONSOLE_UART_H_
#define _CONSOLE_UART_H_

/* Let the device raise an interrupt when a byte arrives. */
void uart_rx_enable(void);

/* One byte, or -1 when the receive FIFO is empty. */
int uart_getc(void);

/* Polled, at the priority of whoever asked. Section 22.4 leaves the
 * kernel writing to the same device for panics and for boot. */
void uart_putc(char c);

#endif /* _CONSOLE_UART_H_ */
