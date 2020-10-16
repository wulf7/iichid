/* $FreeBSD$ */
/*-
 * SPDX-License-Identifier: BSD-2-Clause-NetBSD
 *
 * Copyright (c) 2008 Hans Petter Selasky. All rights reserved.
 * Copyright (c) 1998 The NetBSD Foundation, Inc. All rights reserved.
 * Copyright (c) 1998 Lennart Augustsson. All rights reserved.
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

#ifndef _HID_H_
#define	_HID_H_

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbhid.h>

/* Declare parent SYSCTL USB node. */
#ifdef SYSCTL_DECL
SYSCTL_DECL(_hw_hid);
#endif

#ifndef HUG_MULTIAXIS_CNTROLLER
#define	HUG_MULTIAXIS_CNTROLLER	0x0008
#endif

#ifndef HUG_D_PAD_UP
#define	HUG_D_PAD_UP	0x90
#define	HUG_D_PAD_DOWN	0x91
#define	HUG_D_PAD_RIGHT	0x92
#define	HUG_D_PAD_LEFT	0x93
#endif

#ifndef	HUD_SURFACE_SWITCH
#define	HUD_SURFACE_SWITCH	0x0057
#endif
#ifndef	HUD_BUTTONS_SWITCH
#define	HUD_BUTTONS_SWITCH	0x0058
#endif
#ifndef HUD_SEC_BARREL_SWITCH
#define	HUD_SEC_BARREL_SWITCH	0x5a
#endif

#ifndef	HUC_CONSUMER_CONTROL
#define	HUC_CONSUMER_CONTROL	0x0001
#endif
#ifndef	HUC_HEADPHONE
#define	HUC_HEADPHONE	0x05
#endif

#define	HID_INPUT_REPORT	0x1
#define	HID_OUTPUT_REPORT	0x2
#define	HID_FEATURE_REPORT	0x3

#define	HID_MAX_AUTO_QUIRK	8	/* maximum number of dynamic quirks */
#define	HID_PNP_ID_SIZE		20	/* includes null terminator */

#define	HID_IN_POLLING_MODE_FUNC() hid_in_polling_mode()
#define	HID_IN_POLLING_MODE_VALUE() (SCHEDULER_STOPPED() || kdb_active)

typedef usb_size_t hid_size_t;

struct hid_absinfo {
	int32_t min;
	int32_t max;
	int32_t res;
};

struct hid_device_info {
	char		name[80];
	char		serial[80];
	char		idPnP[HID_PNP_ID_SIZE];
	uint16_t	idBus;
	uint16_t	idVendor;
	uint16_t	idProduct;
	uint16_t	idVersion;
	hid_size_t	rdescsize;	/* Report descriptor size */
	uint8_t		autoQuirk[HID_MAX_AUTO_QUIRK];
};

struct hid_rdesc_info {
	void		*data;
	hid_size_t	len;
	hid_size_t	isize;
	hid_size_t	osize;
	hid_size_t	fsize;
	uint8_t		iid;
	uint8_t		oid;
	uint8_t		fid;
	/* Max sizes for HID requests supported by transport backend */
	hid_size_t	rdsize;
	hid_size_t	wrsize;
	hid_size_t	grsize;
	hid_size_t	srsize;
};

/* OpenBSD/NetBSD compat shim */
#define	HID_GET_USAGE(u) ((u) & 0xffff)
#define	HID_GET_USAGE_PAGE(u) (((u) >> 16) & 0xffff)

typedef void hid_intr_t(void *context, void *data, hid_size_t len);
typedef bool hid_test_quirk_t(const struct hid_device_info *dev_info,
    uint16_t quirk);

static __inline uint32_t
hid_get_udata(const uint8_t *buf, hid_size_t len, struct hid_location *loc)
{
	return (hid_get_data_unsigned(buf, len, loc));
}

extern hid_test_quirk_t *hid_test_quirk_p;

/*
 * hid_report_size_1 is a port of userland hid_report_size() from usbhid(3)
 * to kernel. XXX: to be renamed back to hid_report_size()
 */
int	hid_report_size_1(const void *buf, hid_size_t len, enum hid_kind k,
	    uint8_t id);
bool	hid_test_quirk(const struct hid_device_info *dev_info, uint16_t quirk);
int	hid_add_dynamic_quirk(struct hid_device_info *dev_info,
	    uint16_t quirk);
void	hidquirk_unload(void *arg);
int	hid_in_polling_mode(void);

int	hid_get_rdesc(device_t, void *, hid_size_t);
int	hid_read(device_t, void *, hid_size_t, hid_size_t *);
int	hid_write(device_t, const void *, hid_size_t);
int	hid_get_report(device_t, void *, hid_size_t, hid_size_t *, uint8_t,
	    uint8_t);
int	hid_set_report(device_t, const void *, hid_size_t, uint8_t, uint8_t);
int	hid_set_idle(device_t, uint16_t, uint8_t);
int	hid_set_protocol(device_t, uint16_t);

#endif					/* _HID_H_ */
