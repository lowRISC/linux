// SPDX-License-Identifier: Apache-2.0
/*
 * lowRISC Opentitan UART driver.
 * Copyright (C) 2026 lowRISC Contributors.
 *
 * Based on:
 *   drivers/tty/sifive.c
 *   drivers/tty/liteuart.c
 */

#include <linux/console.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define LOWRISC_OT_SERIAL_NAME			"lowrisc-opentitan-serial"

#define LOWRISC_OT_SERIAL_TTY_PREFIX		"ttyOT"

#define LOWRISC_OT_SERIAL_MAX_PORTS		1

#define LOWRISC_OT_SERIAL_DEFAULT_BAUD		1000000

#ifdef CONFIG_SERIAL_LOWRISC_OPENTITAN_CONSOLE

/* Console ports. */
struct lowrisc_ot_serial_port *
	lowrisc_ot_serial_console_port[LOWRISC_OT_SERIAL_MAX_PORTS];

/* Console driver forward declaration. */
static struct console lowrisc_ot_serial_console;

#define LOWRISC_OT_SERIAL_CONSOLE		(&lowrisc_ot_serial_console)

#else /* !CONFIG_SERIAL_LOWRISC_OPENTITAN_CONSOLE */

#define LOWRISC_OT_SERIAL_CONSOLE		(NULL)

#endif /* CONFIG_SERIAL_LOWRISC_OPENTITAN_CONSOLE */

static struct uart_driver lowrisc_ot_serial_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= LOWRISC_OT_SERIAL_NAME,
	.dev_name	= LOWRISC_OT_SERIAL_TTY_PREFIX,
	.nr		= LOWRISC_OT_SERIAL_MAX_PORTS,
	.cons		= LOWRISC_OT_SERIAL_CONSOLE,
};

struct lowrisc_ot_serial_port {
	struct uart_port	port;
	struct device		*dev;
};

#define port_to_lowrisc_ot_serial_port(p) (container_of((p), \
							struct lowrisc_ot_serial_port, \
							port))

#define OT_UART_TX_FIFO_DEPTH	32
#define OT_UART_RX_FIFO_DEPTH	64

/*
 * Register offsets.
 */
#define OT_UART_REG_INTR_STATE		0
#define OT_UART_REG_INTR_ENABLE		1
#define OT_UART_REG_CTRL		4
#define OT_UART_REG_STATUS		5
#define OT_UART_REG_RDATA		6
#define OT_UART_REG_WDATA		7
#define OT_UART_REG_FIFO_CTRL		8
#define OT_UART_REG_FIFO_STATUS		9
#define OT_UART_REG_OVRD		10
#define OT_UART_REG_VAL			11
#define OT_UART_REG_TIMEOUT_CTRL	12

/*
 * INTR_* register fields.
 */
#define OT_UART_INTR_TX_WATERMARK	0x1
#define OT_UART_INTR_RX_WATERMARK	0x2
/* RW1C */
#define OT_UART_INTR_RX_TIMEOUT		0x40

/*
 * FIFO_CTRL register fields.
 */
#define OT_UART_FIFO_CTRL_RXRST		0x1
#define OT_UART_FIFO_CTRL_TXRST		0x2
#define OT_UART_FIFO_CTRL_RXILVL_SHIFT	2
#define OT_UART_FIFO_CTRL_RXILVL_1	0x0
#define OT_UART_FIFO_CTRL_RXILVL_2	0x1
#define OT_UART_FIFO_CTRL_RXILVL_4	0x2
#define OT_UART_FIFO_CTRL_RXILVL_8	0x3
#define OT_UART_FIFO_CTRL_RXILVL_16	0x4
#define OT_UART_FIFO_CTRL_RXILVL_32	0x5
#define OT_UART_FIFO_CTRL_RXILVL_62	0x6
#define OT_UART_FIFO_CTRL_TXILVL_SHIFT	5
#define OT_UART_FIFO_CTRL_TXILVL_1	0x0
#define OT_UART_FIFO_CTRL_TXILVL_2	0x1
#define OT_UART_FIFO_CTRL_TXILVL_4	0x2
#define OT_UART_FIFO_CTRL_TXILVL_8	0x3
#define OT_UART_FIFO_CTRL_TXILVL_16	0x4

/*
 * FIFO_STATUS register fields.
 */
#define OT_UART_FIFO_STATUS_TXLVL_SHIFT	0
#define OT_UART_FIFO_STATUS_TXLVL_MASK	0xff
#define OT_UART_FIFO_STATUS_RXLVL_SHIFT	16
#define OT_UART_FIFO_STATUS_RXLVL_MASK	0xff

/*
 * TIMEOUT_CTRL register fields.
 */
#define OT_UART_TIMEOUT_CTRL_VAL_SHIFT	0
#define OT_UART_TIMEOUT_CTRL_VAL_MASK	0xffffff
#define OT_UART_TIMEOUT_CTRL_EN		0x80000000

/*
 * Compile-time Configuration.
 */
/* RX Watermark level in characters. */
#define OT_UART_FIFO_CTRL_RXILVL	OT_UART_FIFO_CTRL_RXILVL_8
/* TX Watermark level in characters. */
#define OT_UART_FIFO_CTRL_TXILVL	OT_UART_FIFO_CTRL_TXILVL_16
/* RX Timeout interrupt threshold in bit times. */
#define OT_UART_RX_TIMEOUT_CTRL_VAL	(8 * 2)


#ifdef CONFIG_SERIAL_LOWRISC_OPENTITAN_CONSOLE

/*
 * Adding/removing console ports.
 */

static void __ot_serial_add_console_port(struct lowrisc_ot_serial_port *p)
{
	lowrisc_ot_serial_console_port[p->port.line] = p;
}

static void __ot_serial_remove_console_port(struct lowrisc_ot_serial_port *p)
{
	lowrisc_ot_serial_console_port[p->port.line] = NULL;
}

#else /* !CONFIG_SERIAL_LOWRISC_OPENTITAN_CONSOLE */

static void __ot_serial_add_console_port(struct lowrisc_ot_serial_port *p)
{}

static void __ot_serial_remove_console_port(struct lowrisc_ot_serial_port *p)
{}

#endif /* CONFIG_SERIAL_LOWRISC_OPENTITAN_CONSOLE */

/*
 * Register primitives.
 */

static void __ot_serial_write_reg(u32 v, u8 reg, struct lowrisc_ot_serial_port *p)
{
	writel_relaxed(v, p->port.membase + (4 * reg));
}

static u32 __ot_serial_read_reg(u8 reg, struct lowrisc_ot_serial_port *p)
{
	return readl_relaxed(p->port.membase + (4 * reg));
}

static void __ot_serial_enable_tx_watermark(struct lowrisc_ot_serial_port *p)
{
	u32 ie;
	ie = __ot_serial_read_reg(OT_UART_REG_INTR_ENABLE, p);
	ie |= OT_UART_INTR_TX_WATERMARK;
	__ot_serial_write_reg(ie, OT_UART_REG_INTR_ENABLE, p);
}

static void __ot_serial_enable_rx_watermark(struct lowrisc_ot_serial_port *p)
{
	u32 ie;
	ie = __ot_serial_read_reg(OT_UART_REG_INTR_ENABLE, p);
	ie |= OT_UART_INTR_RX_WATERMARK;
	__ot_serial_write_reg(ie, OT_UART_REG_INTR_ENABLE, p);
}

static void __ot_serial_enable_rx_timeout(struct lowrisc_ot_serial_port *p)
{
	u32 ie;
	ie = __ot_serial_read_reg(OT_UART_REG_INTR_ENABLE, p);
	ie |= OT_UART_INTR_RX_TIMEOUT;
	__ot_serial_write_reg(ie, OT_UART_REG_INTR_ENABLE, p);

}

static void __ot_serial_disable_tx_watermark(struct lowrisc_ot_serial_port *p)
{
	u32 ie;
	ie = __ot_serial_read_reg(OT_UART_REG_INTR_ENABLE, p);
	ie &= ~OT_UART_INTR_TX_WATERMARK;
	__ot_serial_write_reg(ie, OT_UART_REG_INTR_ENABLE, p);
}

static void __ot_serial_disable_rx_watermark(struct lowrisc_ot_serial_port *p)
{
	u32 ie;
	ie = __ot_serial_read_reg(OT_UART_REG_INTR_ENABLE, p);
	ie &= ~OT_UART_INTR_RX_WATERMARK;
	__ot_serial_write_reg(ie, OT_UART_REG_INTR_ENABLE, p);
}

static void __ot_serial_disable_rx_timeout(struct lowrisc_ot_serial_port *p)
{
	u32 ie;
	ie = __ot_serial_read_reg(OT_UART_REG_INTR_ENABLE, p);
	ie &= ~OT_UART_INTR_RX_TIMEOUT;
	__ot_serial_write_reg(ie, OT_UART_REG_INTR_ENABLE, p);

}

static void __ot_serial_init(struct lowrisc_ot_serial_port *p)
{
	u32 v;

	/* disable interrupts. */
	__ot_serial_write_reg(0, OT_UART_REG_INTR_ENABLE, p);

	v = OT_UART_FIFO_CTRL_RXRST | OT_UART_FIFO_CTRL_TXRST |
		(OT_UART_FIFO_CTRL_RXILVL << OT_UART_FIFO_CTRL_RXILVL_SHIFT) |
		(OT_UART_FIFO_CTRL_TXILVL << OT_UART_FIFO_CTRL_TXILVL_SHIFT);

	v = OT_UART_TIMEOUT_CTRL_EN |
		((OT_UART_RX_TIMEOUT_CTRL_VAL & OT_UART_TIMEOUT_CTRL_VAL_MASK)
			<< OT_UART_TIMEOUT_CTRL_VAL_SHIFT);

	__ot_serial_write_reg(v, OT_UART_REG_TIMEOUT_CTRL, p);

	__ot_serial_write_reg(v, OT_UART_REG_FIFO_CTRL, p);
}

static u8 __ot_serial_tx_fifo_level(struct lowrisc_ot_serial_port *p)
{
	u32 status;
	status = __ot_serial_read_reg(OT_UART_REG_FIFO_STATUS, p);
	return (u8)((status >> OT_UART_FIFO_STATUS_TXLVL_SHIFT)
		    & OT_UART_FIFO_STATUS_TXLVL_MASK);
}

static u8 __ot_serial_rx_fifo_level(struct lowrisc_ot_serial_port *p)
{
	u32 status;
	status = __ot_serial_read_reg(OT_UART_REG_FIFO_STATUS, p);
	return (u8)((status >> OT_UART_FIFO_STATUS_RXLVL_SHIFT)
		    & OT_UART_FIFO_STATUS_RXLVL_MASK);
}

static void __ot_serial_do_transmit_char(struct lowrisc_ot_serial_port *p, char ch)
{
	__ot_serial_write_reg(ch, OT_UART_REG_WDATA, p);
}

static char __ot_serial_do_receive_char(struct lowrisc_ot_serial_port *p, int *valid)
{
	/* if there is nothing in the RX FIFO, set valid to false and return. */
	if (__ot_serial_rx_fifo_level(p) == 0) {
		*valid = false;
		return 0;
	}

	/* a character was read. */
	*valid = true;
	return (char)(__ot_serial_read_reg(OT_UART_REG_RDATA, p) & 0xff);
}

static void __ot_serial_receive_chars(struct lowrisc_ot_serial_port *p)
{
	int valid;
	int c;
	u8 ch;

	for (c = 0; c < OT_UART_RX_FIFO_DEPTH; c++) {
		ch = __ot_serial_do_receive_char(p, &valid);
		if (!valid)
			break;

		if (!uart_prepare_sysrq_char(&p->port, ch))
			uart_insert_char(&p->port, 0, 0, ch, TTY_NORMAL);
	}

	p->port.icount.rx += c;

	tty_flip_buffer_push(&p->port.state->port);
}

static void __ot_serial_transmit_chars(struct lowrisc_ot_serial_port *p)
{
	u8 ch;

	uart_port_tx_limited(&p->port, ch, OT_UART_TX_FIFO_DEPTH,
		(__ot_serial_tx_fifo_level(p) < OT_UART_TX_FIFO_DEPTH),
		__ot_serial_do_transmit_char(p, ch),
		({}));
}

/*
 * core UART functions.
 */

static int lowrisc_ot_serial_startup(struct uart_port *port) {
	struct lowrisc_ot_serial_port *p = port_to_lowrisc_ot_serial_port(port);
	unsigned long flags;

	uart_port_lock_irqsave(&p->port, &flags);

	__ot_serial_enable_tx_watermark(p);
	__ot_serial_enable_rx_watermark(p);
	__ot_serial_enable_rx_timeout(p);

	uart_port_unlock_irqrestore(&p->port, flags);

	return 0;
}

static void lowrisc_ot_serial_shutdown(struct uart_port *port) {
	struct lowrisc_ot_serial_port *p = port_to_lowrisc_ot_serial_port(port);
	unsigned long flags;

	uart_port_lock_irqsave(&p->port, &flags);

	__ot_serial_disable_tx_watermark(p);
	__ot_serial_disable_rx_watermark(p);
	__ot_serial_disable_rx_timeout(p);

	uart_port_unlock_irqrestore(&p->port, flags);
}

static unsigned int lowrisc_ot_serial_tx_empty(struct uart_port *port)
{
	struct lowrisc_ot_serial_port *p = port_to_lowrisc_ot_serial_port(port);

	return __ot_serial_tx_fifo_level(p) == 0 ? TIOCSER_TEMT : 0;
}

static void lowrisc_ot_serial_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	/* Unsupported. */
}

static unsigned int lowrisc_ot_serial_get_mctrl(struct uart_port *port)
{
	/* Unsupported - always report these signals as set. */
	return TIOCM_CAR | TIOCM_CTS | TIOCM_DSR;
}

static void lowrisc_ot_serial_stop_tx(struct uart_port *port)
{
	struct lowrisc_ot_serial_port *p = port_to_lowrisc_ot_serial_port(port);

	__ot_serial_disable_tx_watermark(p);
}

static void lowrisc_ot_serial_start_tx(struct uart_port *port)
{
	struct lowrisc_ot_serial_port *p = port_to_lowrisc_ot_serial_port(port);

	__ot_serial_enable_tx_watermark(p);
}

static void lowrisc_ot_serial_stop_rx(struct uart_port *port)
{
	struct lowrisc_ot_serial_port *p = port_to_lowrisc_ot_serial_port(port);

	__ot_serial_disable_rx_watermark(p);
	__ot_serial_disable_rx_timeout(p);
}

static void lowrisc_ot_serial_start_rx(struct uart_port *port)
{
	struct lowrisc_ot_serial_port *p = port_to_lowrisc_ot_serial_port(port);

	__ot_serial_enable_rx_watermark(p);
	__ot_serial_enable_rx_timeout(p);

}

static void lowrisc_ot_serial_break_ctl(struct uart_port *port, int break_state)
{
	/* Unsupported. */
}

static void lowrisc_ot_serial_set_termios(struct uart_port *port,
					  struct ktermios *termios,
					  const struct ktermios *old)
{
	/* Unsupported. */
}

static void lowrisc_ot_serial_config_port(struct uart_port *port, int flags)
{
	port->type = PORT_LOWRISC_OT;
}

static const char *lowrisc_ot_serial_type(struct uart_port *port)
{
	return port->type == PORT_LOWRISC_OT ? "lowRISC Opentitan UART" : NULL;
}

static const struct uart_ops lowrisc_ot_serial_uops = {
	.tx_empty	= lowrisc_ot_serial_tx_empty,
	.set_mctrl	= lowrisc_ot_serial_set_mctrl,
	.get_mctrl	= lowrisc_ot_serial_get_mctrl,
	.stop_tx	= lowrisc_ot_serial_stop_tx,
	.start_tx	= lowrisc_ot_serial_start_tx,
	.stop_rx	= lowrisc_ot_serial_stop_rx,
	.start_rx	= lowrisc_ot_serial_start_rx,
	.break_ctl	= lowrisc_ot_serial_break_ctl,
	.startup	= lowrisc_ot_serial_startup,
	.shutdown	= lowrisc_ot_serial_shutdown,
	.set_termios	= lowrisc_ot_serial_set_termios,
	.type		= lowrisc_ot_serial_type,
	.config_port	= lowrisc_ot_serial_config_port,
};

#ifdef CONFIG_SERIAL_LOWRISC_OPENTITAN_CONSOLE

/*
 * console functions.
 */

static void lowrisc_ot_serial_console_device_lock(struct console *co, unsigned long *flags)
{
	struct uart_port *port = &lowrisc_ot_serial_console_port[co->index]->port;
	__uart_port_lock_irqsave(port, flags);
}

static void lowrisc_ot_serial_console_device_unlock(struct console *co, unsigned long flags)
{
	struct uart_port *port = &lowrisc_ot_serial_console_port[co->index]->port;
	__uart_port_unlock_irqrestore(port, flags);
}

static int lowrisc_ot_serial_console_setup(struct console *co, char *options)
{
	struct lowrisc_ot_serial_port *p;
	int baud = LOWRISC_OT_SERIAL_DEFAULT_BAUD;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	if (co->index < 0 || co->index >= LOWRISC_OT_SERIAL_MAX_PORTS)
		return -ENODEV;

	p = lowrisc_ot_serial_console_port[co->index];
	if (!p)
		return -ENODEV;

	if (options)
	 	uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(&p->port, co, baud, parity, bits, flow);
}

static void __ot_serial_polling_transmit_char(struct lowrisc_ot_serial_port *p, char ch)
{
	while (__ot_serial_tx_fifo_level(p) >= OT_UART_TX_FIFO_DEPTH)
		cpu_relax();

	__ot_serial_do_transmit_char(p, ch);
}

static void lowrisc_ot_serial_console_putchar(struct uart_port *port, unsigned char ch)
{
	struct lowrisc_ot_serial_port *p = port_to_lowrisc_ot_serial_port(port);

	__ot_serial_polling_transmit_char(p, ch);
}

static void lowrisc_ot_serial_console_write_atomic(struct console *co,
						   struct nbcon_write_context *wctxt)
{
	struct lowrisc_ot_serial_port *p = lowrisc_ot_serial_console_port[co->index];
	struct uart_port *port = &p->port;
	u32 ie;

	if (!p)
		return;

	if (!nbcon_enter_unsafe(wctxt))
		return;

	/* save and restore INTR_ENABLE */
	ie = __ot_serial_read_reg(OT_UART_REG_INTR_ENABLE, p);
	__ot_serial_write_reg(0, OT_UART_REG_INTR_ENABLE, p);

	uart_console_write(port, wctxt->outbuf, wctxt->len, lowrisc_ot_serial_console_putchar);

	__ot_serial_write_reg(ie, OT_UART_REG_INTR_ENABLE, p);

	nbcon_exit_unsafe(wctxt);
}

static void lowrisc_ot_serial_console_write_thread(struct console *co,
						   struct nbcon_write_context *wctxt)
{
	/* TODO: an implementation that doesn't just call write_atomic. */
	lowrisc_ot_serial_console_write_atomic(co, wctxt);

}

static struct console lowrisc_ot_serial_console = {
	.name		= LOWRISC_OT_SERIAL_TTY_PREFIX,
	.write_atomic	= lowrisc_ot_serial_console_write_atomic,
	.write_thread	= lowrisc_ot_serial_console_write_thread,
	.device_lock	= lowrisc_ot_serial_console_device_lock,
	.device_unlock	= lowrisc_ot_serial_console_device_unlock,
	.device		= uart_console_device,
	.setup		= lowrisc_ot_serial_console_setup,
	.flags		= CON_PRINTBUFFER | CON_NBCON,
	.index		= -1,
	.data		= &lowrisc_ot_serial_uart_driver,
};

static int __init lowrisc_ot_serial_console_init(void)
{
	register_console(&lowrisc_ot_serial_console);
	return 0;
}

console_initcall(lowrisc_ot_serial_console_init);

#endif /* CONFIG_SERIAL_LOWRISC_OPENTITAN_CONSOLE */

/*
 * Devicetree compatible strings.
 */
static const struct of_device_id lowrisc_ot_serial_of_match[] = {
	{ .compatible = "lowrisc,opentitan-uart-v2" },
	{},
};

MODULE_DEVICE_TABLE(of, lowrisc_ot_serial_of_match);

/*
 * IRQ Handler.
 */
static irqreturn_t lowrisc_ot_serial_irq(int irq, void *dev_id)
{
	struct lowrisc_ot_serial_port *p = dev_id;
	u32 ip;

	uart_port_lock(&p->port);

	ip = __ot_serial_read_reg(OT_UART_REG_INTR_STATE, p);
	if (!ip) {
		uart_port_unlock(&p->port);
		return IRQ_NONE;
	}

	/* handle RX interrupts first. */
	if (ip & OT_UART_INTR_RX_WATERMARK || ip & OT_UART_INTR_RX_TIMEOUT) {
		__ot_serial_receive_chars(p);
		/* RX timeout interrupt is RW1C */
		__ot_serial_write_reg(OT_UART_INTR_RX_TIMEOUT, OT_UART_REG_INTR_STATE, p);
	}
	if (ip & OT_UART_INTR_TX_WATERMARK)
		__ot_serial_transmit_chars(p);

	uart_unlock_and_check_sysrq(&p->port);

	return IRQ_HANDLED;
}

static int lowrisc_ot_serial_probe(struct platform_device *pdev)
{
	struct lowrisc_ot_serial_port *p;
	struct resource *mem;
	void __iomem *base;
	int irq, r;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return -EPROBE_DEFER;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &mem);
	if (IS_ERR(base))
		return PTR_ERR(base);

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->port.dev = &pdev->dev;
	p->port.type = PORT_LOWRISC_OT;
	p->port.iotype = UPIO_MEM;
	p->port.irq = irq;
	p->port.fifosize = OT_UART_TX_FIFO_DEPTH;
	p->port.ops = &lowrisc_ot_serial_uops;
	/* TODO: get an id from a serial DT alias instead. */
	p->port.line = 0;
	p->port.mapbase = mem->start;
	p->port.membase = base;
	/* TODO: get value from DT, perhaps by clock. */
	p->port.uartclk = LOWRISC_OT_SERIAL_DEFAULT_BAUD * 16;
	p->dev = &pdev->dev;

	platform_set_drvdata(pdev, p);

	__ot_serial_init(p);

	r = request_irq(p->port.irq, lowrisc_ot_serial_irq, p->port.irqflags,
			dev_name(&pdev->dev), p);
	if (r) {
		dev_err(&pdev->dev, "could not attach interrupt %d\n", r);
		goto probe_out1;
	}

	__ot_serial_add_console_port(p);

	r = uart_add_one_port(&lowrisc_ot_serial_uart_driver, &p->port);
	if (r != 0) {
		dev_err(&pdev->dev, "could not add uart: %d\n", r);
		goto probe_out2;
	}

	return 0;

probe_out2:
	__ot_serial_remove_console_port(p);
	free_irq(p->port.irq, p);
probe_out1:
	return r;
}

static void lowrisc_ot_serial_remove(struct platform_device *pdev)
{
	struct lowrisc_ot_serial_port *p = platform_get_drvdata(pdev);

	__ot_serial_remove_console_port(p);
	uart_remove_one_port(&lowrisc_ot_serial_uart_driver, &p->port);
	free_irq(p->port.irq, p);
}

static struct platform_driver lowrisc_ot_serial_platform_driver = {
	.probe		= lowrisc_ot_serial_probe,
	.remove		= lowrisc_ot_serial_remove,
	.driver		= {
		.name 	= LOWRISC_OT_SERIAL_NAME,
		.of_match_table = lowrisc_ot_serial_of_match,
	},
};

static int __init lowrisc_ot_serial_init(void)
{
	int r;

	r = uart_register_driver(&lowrisc_ot_serial_uart_driver);
	if (r)
		goto init_out1;

	r = platform_driver_register(&lowrisc_ot_serial_platform_driver);
	if (r)
		goto init_out2;

	return 0;

init_out2:
	uart_unregister_driver(&lowrisc_ot_serial_uart_driver);
init_out1:
	return r;
}

static void __exit lowrisc_ot_serial_exit(void)
{
	platform_driver_register(&lowrisc_ot_serial_platform_driver);
	uart_unregister_driver(&lowrisc_ot_serial_uart_driver);
}

module_init(lowrisc_ot_serial_init);
module_exit(lowrisc_ot_serial_exit);

MODULE_DESCRIPTION("lowRISC Opentitan UART driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alice Ziuziakowska <a.ziuziakowska@lowrisc.org>");
