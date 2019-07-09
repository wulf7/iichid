/*-
 * Copyright (c) 2018-2019 Marc Priggemeyer <marc.priggemeyer@gmail.com>
 * Copyright (c) 2019 Vladimir Kondratyev <wulf@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _IICHID_H_
#define _IICHID_H_

/*
 * Since interrupt resource acquisition is not always possible (in case of GPIO
 * interrupts) iichid now supports a sampling_mode.
 * Set dev.<name>.<unit>.sampling_rate to a value greater then 0 to activate
 * sampling. A value of 0 is possible but will not reset the callout and,
 * thereby, disable further report requests. Do not set the sampling_rate value
 * too high as it may result in periodical lags of cursor motion.
 */
#define	IICHID_DEFAULT_SAMPLING_RATE	60

/* 5.1.1 - HID Descriptor Format */
struct i2c_hid_desc {
	uint16_t wHIDDescLength;
	uint16_t bcdVersion;
	uint16_t wReportDescLength;
	uint16_t wReportDescRegister;
	uint16_t wInputRegister;
	uint16_t wMaxInputLength;
	uint16_t wOutputRegister;
	uint16_t wMaxOutputLength;
	uint16_t wCommandRegister;
	uint16_t wDataRegister;
	uint16_t wVendorID;
	uint16_t wProductID;
	uint16_t wVersionID;
	uint32_t reserved;
} __packed;

typedef void iichid_intr_t(void *context, uint8_t *buf, int len);

struct iichid_hw {
	char		hid[16];
	uint8_t		device_addr;
	uint16_t	config_reg;
	uint16_t	irq;
	uint16_t	gpio_pin;
	device_t	acpi_dev;
};

struct iichid {
	device_t		dev;
	struct mtx		lock;

	struct iichid_hw	hw;
	struct i2c_hid_desc	desc;

	iichid_intr_t		*intr_handler;
	void			*intr_sc;

	uint8_t			*input_buf;
	int			input_size;

	int			irq_rid;
	struct resource		*irq_res;
	void			*irq_cookie;

	int			sampling_rate;
	struct callout		periodic_callout;
	bool			callout_setup;

	struct taskqueue	*taskqueue;
	struct task		event_task;
};

int	iichid_fetch_report_descriptor(struct iichid*, uint8_t **, int *);
void	iichid_identify(driver_t *, device_t);
int	iichid_init(struct iichid *, device_t);
void	iichid_destroy(struct iichid *);
int	iichid_set_intr(struct iichid *, iichid_intr_t, void *);

#endif	/* _IICHID_H_ */
