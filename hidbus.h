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

struct hidbus_report_descr {
	void		*data;
	hid_size_t	len;
	hid_size_t	isize;
	hid_size_t	osize;
	hid_size_t	fsize;
	uint8_t		iid;
	uint8_t		oid;
	uint8_t		fid;
	bool		overloaded;
	/* Maximal sizes for HID requests supported by transport backend */
	hid_size_t	rdsize;
	hid_size_t	wrsize;
	hid_size_t	grsize;
	hid_size_t	srsize;
};

struct hidbus_ivars {
	device_t			child;
	int32_t				usage;
	uint8_t				index;
	uintptr_t			driver_info;	/* for internal use */
	hid_intr_t			*intr_handler;
	void				*intr_ctx;
	bool				open;
	STAILQ_ENTRY(hidbus_ivars)	link;
};

enum {
	HIDBUS_IVAR_USAGE,
	HIDBUS_IVAR_INDEX,
	HIDBUS_IVAR_DRIVER_INFO,
};

#define HIDBUS_ACCESSOR(A, B, T)					\
	__BUS_ACCESSOR(hidbus, A, HIDBUS, B, T)

HIDBUS_ACCESSOR(usage,		USAGE,		int32_t)
HIDBUS_ACCESSOR(index,		INDEX,		uint8_t)
HIDBUS_ACCESSOR(driver_info,	DRIVER_INFO,	uintptr_t)

/*
 * The following structure is used when looking up an HID driver for
 * an HID device. It is inspired by the structure called "usb_device_id".
 * which is originated in Linux and ported to FreeBSD.
 */
struct hid_device_id {

	/* Select which fields to match against */
	uint8_t
		match_flag_usage:1,
		match_flag_bus:1,
		match_flag_vendor:1,
		match_flag_product:1,
		match_flag_ver_lo:1,
		match_flag_ver_hi:1,
		match_flag_unused:2;

	/* Used for top level collection usage matches */
	int32_t usage;

	/* Used for product specific matches; the Version range is inclusive */
	uint16_t idBus;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t idVersion_lo;
	uint16_t idVersion_hi;

	/* Hook for driver specific information */
	uintptr_t driver_info;
};

#define HID_TLC(page,usg)			\
  .match_flag_usage = 1, .usage = HID_USAGE2((page),(usg))

#define HID_BUS(bus)				\
  .match_flag_bus = 1, .idBus = (bus)

#define HID_VENDOR(vend)			\
  .match_flag_vendor = 1, .idVendor = (vend)

#define HID_PRODUCT(prod)			\
  .match_flag_product = 1, .idProduct = (prod)

#define HID_VP(vend,prod)			\
  HID_VENDOR(vend), HID_PRODUCT(prod)

#define HID_BVP(bus,vend,prod)			\
  HID_BUS(bus), HID_VENDOR(vend), HID_PRODUCT(prod)

#define HID_BVPI(bus,vend,prod,info)		\
  HID_BUS(bus), HID_VENDOR(vend), HID_PRODUCT(prod), HID_DRIVER_INFO(info)

#define HID_VERSION_GTEQ(lo)	/* greater than or equal */	\
  .match_flag_ver_lo = 1, .idVersion_lo = (lo)

#define HID_VERSION_LTEQ(hi)	/* less than or equal */	\
  .match_flag_ver_hi = 1, .idVersion_hi = (hi)

#define HID_DRIVER_INFO(n)			\
  .driver_info = (n)

#define HID_GET_DRIVER_INFO(did)		\
  (did)->driver_info

/*
 * General purpose locking wrappers to ease supporting
 * HID polled mode:
 */
#define	HID_SYSCONS_MTX	(&Giant)
#ifdef INVARIANTS
#define	HID_MTX_ASSERT(_m, _t) do {		\
	if (!HID_IN_POLLING_MODE_FUNC())	\
		mtx_assert(_m, _t);		\
} while (0)
#else
#define	HID_MTX_ASSERT(_m, _t) do { } while (0)
#endif

#define	HID_MTX_LOCK(_m) do {			\
	if (!HID_IN_POLLING_MODE_FUNC())	\
		mtx_lock(_m);			\
} while (0)

#define	HID_MTX_UNLOCK(_m) do {			\
	if (!HID_IN_POLLING_MODE_FUNC())	\
		mtx_unlock(_m);			\
} while (0)

const struct hid_device_id *hidbus_lookup_id(device_t,
		    const struct hid_device_id *, size_t);
struct hidbus_report_descr *hidbus_get_report_descr(device_t);
int		hidbus_lookup_driver_info(device_t,
		    const struct hid_device_id *, size_t);
struct mtx *	hidbus_get_lock(device_t);
void		hidbus_set_intr(device_t, hid_intr_t*, void *);
int		hidbus_intr_start(device_t);
int		hidbus_intr_stop(device_t);
void		hidbus_intr_poll(device_t);
void		hidbus_set_desc(device_t, const char *);

/* hidbus HID interface */
int	hid_get_report_descr(device_t, void **, hid_size_t *);
int	hid_set_report_descr(device_t, const void *, hid_size_t);
int	hid_read(device_t, void *, hid_size_t, hid_size_t *);
int	hid_write(device_t, const void *, hid_size_t);
int	hid_get_report(device_t, void *, hid_size_t, hid_size_t *, uint8_t,
	    uint8_t);
int	hid_set_report(device_t, const void *, hid_size_t, uint8_t, uint8_t);
int	hid_set_idle(device_t, uint16_t, uint8_t);
int	hid_set_protocol(device_t, uint16_t);

const struct hid_device_info *hid_get_device_info(device_t);

extern devclass_t hidbus_devclass;

#endif	/* _HIDBUS_H_ */
