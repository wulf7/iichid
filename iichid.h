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

#define	I2C_HID_REPORT_TYPE_INPUT	0x1
#define	I2C_HID_REPORT_TYPE_OUTPUT	0x2
#define	I2C_HID_REPORT_TYPE_FEATURE	0x3

/* 7.2 */
enum {
        I2C_HID_CMD_DESCR       = 0x0,
        I2C_HID_CMD_RESET       = 0x1,
        I2C_HID_CMD_GET_REPORT  = 0x2,
        I2C_HID_CMD_SET_REPORT  = 0x3,
        I2C_HID_CMD_GET_IDLE    = 0x4,
        I2C_HID_CMD_SET_IDLE    = 0x5,
        I2C_HID_CMD_GET_PROTO   = 0x6,
        I2C_HID_CMD_SET_PROTO   = 0x7,
        I2C_HID_CMD_SET_POWER   = 0x8,

        /* pseudo commands */
        I2C_HID_REPORT_DESCR    = 0x100,
};
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

typedef void iichid_intr_t(void *context, void *buf, int len, uint8_t id);

struct iichid_hw {
	char		hid[16];
	uint8_t		device_addr;
	uint16_t	config_reg;
};

struct iichid_softc {
	device_t		dev;
	struct mtx		lock;

	bool			probe_done;
	int			probe_result;

	struct iichid_hw	hw;
	struct i2c_hid_desc	desc;

	iichid_intr_t		*intr_handler;
	void			*intr_context;

	uint8_t			*ibuf;
	int			isize;
	uint8_t			iid;

	int			irq_rid;
	struct resource		*irq_res;
	void			*irq_cookie;

	int			sampling_rate;
	struct callout		periodic_callout;
	bool			callout_setup;

	struct taskqueue	*taskqueue;
	struct task		event_task;

	bool			open;

	void			*hid_softc;
};

static inline void *
device_get_hid_softc(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);

	return (sc->hid_softc);
}

/* iichid soft context interface */
void	iichid_set_intr(device_t, iichid_intr_t, void *);
int	iichid_open(device_t);
int	iichid_close(device_t);

/* I2C bus interface */
int	iichid_set_power(device_t dev, bool sleep);
int	iichid_reset(device_t dev);
int	iichid_get_report_desc(device_t, void **, int *);
int	iichid_get_report(device_t, void *, int, uint8_t, uint8_t);

/* Newbus device method stubs */
device_identify_t	iichid_identify;
device_probe_t		iichid_probe;
device_attach_t		iichid_attach;
device_detach_t		iichid_detach;
device_suspend_t	iichid_suspend;
device_resume_t		iichid_resume;

#endif	/* _IICHID_H_ */
