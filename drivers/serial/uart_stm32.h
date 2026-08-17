/*
 * Copyright (c) 2016 Open-RnD Sp. z o.o.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Driver for UART port on STM32 family processor.
 *
 */

#ifndef ZEPHYR_DRIVERS_SERIAL_UART_STM32_H_
#define ZEPHYR_DRIVERS_SERIAL_UART_STM32_H_

#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <stm32_ll_usart.h>

/*
 * What an interrupt-driven receiver has to keep the system out of.
 *
 * Such a receiver is delivered a byte at a time and has no transfer for a wake
 * claim to span, so between characters the system is free to enter a Stop
 * state. Whether that loses data depends on the baud rate and on how long the
 * wake takes, and the driver cannot work that out for itself: most SoC
 * devicetrees give power states a min-residency but no exit-latency, the L4
 * among them. So the board decides, through zephyr,disabling-power-states.
 */
enum uart_stm32_rx_irq_pm {
	/* Property absent: the question has not been considered for this port,
	 * so block every suspend-to-idle substate while the receiver is enabled.
	 * Costs power on a port that did not need it, which is the right way
	 * round - the alternative silently drops bytes.
	 */
	UART_STM32_RX_IRQ_PM_BLOCK_ALL,
	/* Property present but empty: considered, and nothing needs blocking.
	 * The receiver keeps waking per byte, which is measurably lossless at
	 * 9600 and below on an L4.
	 */
	UART_STM32_RX_IRQ_PM_BLOCK_NONE,
	/* Property lists states: block exactly those. */
	UART_STM32_RX_IRQ_PM_BLOCK_DECLARED,
};

/* device config */
struct uart_stm32_config {
	/* USART instance */
	USART_TypeDef *usart;
	/* clock device */
	const struct device *clock;
	/* Reset controller device configuration */
	const struct reset_dt_spec reset;
	/* clock subsystem driving this peripheral */
	const struct stm32_pclken *pclken;
	/* number of clock subsystems */
	size_t pclk_len;
	/* switch to enable single wire / half duplex feature */
	bool single_wire;
	/* enable tx/rx pin swap */
	bool tx_rx_swap;
	/* enable rx pin inversion */
	bool rx_invert;
	/* enable tx pin inversion */
	bool tx_invert;
	/* enable de signal */
	bool de_enable;
	/* what an interrupt-driven receiver must keep the system out of */
	enum uart_stm32_rx_irq_pm rx_irq_pm;
	/* de signal assertion time in 1/16 of a bit */
	uint8_t de_assert_time;
	/* de signal deassertion time in 1/16 of a bit */
	uint8_t de_deassert_time;
	/* enable de pin inversion */
	bool de_invert;
	/* enable fifo */
	bool fifo_enable;
	/* pin muxing */
	const struct pinctrl_dev_config *pcfg;
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_UART_ASYNC_API) || \
	defined(CONFIG_PM)
	uart_irq_config_func_t irq_config_func;
#endif
#if defined(CONFIG_PM)
	/* Device defined as wake-up source */
	bool wakeup_source;
	uint32_t wakeup_line;
#endif /* CONFIG_PM */
};

#ifdef CONFIG_UART_ASYNC_API
struct uart_dma_stream {
	const struct device *dma_dev;
	uint32_t dma_channel;
	struct dma_config dma_cfg;
	uint8_t priority;
	bool src_addr_increment;
	bool dst_addr_increment;
	uint8_t fifo_threshold;
	struct dma_block_config blk_cfg;
	uint8_t *buffer;
	size_t buffer_length;
	size_t offset;
	volatile size_t counter;
	int32_t timeout;
	struct k_work_delayable timeout_work;
	bool enabled;
};
#endif

/*
 * What is keeping the system awake, one bit per source. A source holds its own
 * bit for as long as it needs the states locked, so no source can release
 * another's claim, and taking or releasing the same one twice is harmless.
 *
 * The two transmit sources are mutually exclusive by construction:
 * uart_poll_out() only claims TX_POLL while no stream owns the transmitter, and
 * a stream claims TX_STREAM before releasing any outstanding TX_POLL. That is
 * what lets the transmit-complete arm disarm the interrupt when it sees TX_POLL.
 */
enum uart_stm32_pm_lock {
	UART_STM32_PM_LOCK_TX_POLL,
	UART_STM32_PM_LOCK_TX_STREAM,
	UART_STM32_PM_LOCK_RX,
	/* Held while an interrupt-driven receiver is enabled. What it keeps hold
	 * of depends on what the board declared - see uart_stm32_rx_irq_pm - but
	 * the bit itself only records that something is held, so that repeated
	 * uart_irq_rx_enable() calls, which are a no-op to the caller, take it
	 * once.
	 */
	UART_STM32_PM_LOCK_RX_IRQ,
	UART_STM32_PM_LOCK_COUNT,
};


/* driver data */
struct uart_stm32_data {
	/* uart config */
	struct uart_config *uart_cfg;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_user_data_t user_cb;
	void *user_data;
#endif

#ifdef CONFIG_UART_ASYNC_API
	const struct device *uart_dev;
	uart_callback_t async_cb;
	void *async_user_data;
	struct uart_dma_stream dma_rx;
	struct uart_dma_stream dma_tx;
	uint8_t *rx_next_buffer;
	size_t rx_next_buffer_len;
#endif
#ifdef CONFIG_PM
	ATOMIC_DEFINE(pm_lock, UART_STM32_PM_LOCK_COUNT);
#endif
};

#if defined(CONFIG_SOC_SERIES_STM32MP1X)
static inline uint32_t ll_usart_is_active_txe(USART_TypeDef *usart)
#else /* CONFIG_SOC_SERIES_STM32MP1X */
static inline uint32_t ll_usart_is_active_txe(const USART_TypeDef *usart)
#endif /* CONFIG_SOC_SERIES_STM32MP1X */
{
#if defined(CONFIG_STM32_HAL2)
	return LL_USART_IsActiveFlag_TXE_TXFNF(usart);
#else
	return LL_USART_IsActiveFlag_TXE(usart);
#endif /* CONFIG_STM32_HAL2 */
}

static inline void ll_usart_irq_rx_enable(USART_TypeDef *usart)
{
#if defined(CONFIG_STM32_HAL2)
	LL_USART_EnableIT_RXNE_RXFNE(usart);
#else
	LL_USART_EnableIT_RXNE(usart);
#endif
}

static inline void ll_usart_irq_rx_disable(USART_TypeDef *usart)
{
#if defined(CONFIG_STM32_HAL2)
	LL_USART_DisableIT_RXNE_RXFNE(usart);
#else
	LL_USART_DisableIT_RXNE(usart);
#endif
}

#if defined(CONFIG_SOC_SERIES_STM32MP1X)
static inline uint32_t ll_usart_is_active_rxne(USART_TypeDef *usart)
#else /* CONFIG_SOC_SERIES_STM32MP1X */
static inline uint32_t ll_usart_is_active_rxne(const USART_TypeDef *usart)
#endif /* CONFIG_SOC_SERIES_STM32MP1X */
{
#if defined(CONFIG_STM32_HAL2)
	return LL_USART_IsActiveFlag_RXNE_RXFNE(usart);
#else
	return LL_USART_IsActiveFlag_RXNE(usart);
#endif /* CONFIG_STM32_HAL2 */
}

#if defined(CONFIG_SOC_SERIES_STM32MP1X)
static inline uint32_t ll_usart_is_enabled_rxne(USART_TypeDef *usart)
#else /* CONFIG_SOC_SERIES_STM32MP1X */
static inline uint32_t ll_usart_is_enabled_rxne(const USART_TypeDef *usart)
#endif /* CONFIG_SOC_SERIES_STM32MP1X */
{
#if defined(CONFIG_STM32_HAL2)
	return LL_USART_IsEnabledIT_RXNE_RXFNE(usart);
#else
	return LL_USART_IsEnabledIT_RXNE(usart);
#endif /* CONFIG_STM32_HAL2 */
}

#endif	/* ZEPHYR_DRIVERS_SERIAL_UART_STM32_H_ */
