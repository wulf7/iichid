/*-
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

#ifndef _HIDBUS_H_
#define _HIDBUS_H_

#define	HID_INPUT_REPORT	0x1
#define	HID_OUTPUT_REPORT	0x2
#define	HID_FEATURE_REPORT	0x3

typedef void hid_intr_t(void *context, void *data, uint16_t len);

struct hid_hw {
	char		hid[16];
	uint16_t	idVendor;
	uint16_t	idProduct;
	uint16_t	idVersion;
};

/* hidbus interrupts interface */
void	hid_intr_setup(device_t, struct mtx *mtx, hid_intr_t, void *);
void	hid_intr_unsetup(device_t);
int	hid_intr_start(device_t);
int	hid_intr_stop(device_t);

/* hidbus HID interface */
int	hid_get_report_descr(device_t, void **, uint16_t *);
int	hid_get_input_report(device_t, void *, uint16_t);
int	hid_set_output_report(device_t, void *, uint16_t);
int	hid_get_report(device_t, void *, uint16_t, uint8_t, uint8_t);
int	hid_set_report(device_t, void *, uint16_t, uint8_t, uint8_t);

#endif	/* _HIDBUS_H_ */
