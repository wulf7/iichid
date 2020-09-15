/* $FreeBSD$ */
/*	$NetBSD: hid.c,v 1.17 2001/11/13 06:24:53 lukem Exp $	*/
/*-
 * SPDX-License-Identifier: BSD-2-Clause-NetBSD
 *
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) at
 * Carlstedt Research & Technology.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/proc.h>

#include "hid.h"
#include "hidquirk.h"

static hid_test_quirk_t hid_test_quirk_w;
hid_test_quirk_t *hid_test_quirk_p = &hid_test_quirk_w;

int
hid_report_size_1(const void *buf, hid_size_t len, enum hid_kind k, uint8_t id)
{
	struct hid_data *d;
	struct hid_item h;
	uint32_t temp;
	uint32_t hpos;
	uint32_t lpos;
	int report_id = 0;

	hpos = 0;
	lpos = 0xFFFFFFFF;

	for (d = hid_start_parse(buf, len, 1 << k); hid_get_item(d, &h);) {
		if (h.kind == k && h.report_ID == id) {
			/* compute minimum */
			if (lpos > h.loc.pos)
				lpos = h.loc.pos;
			/* compute end position */
			temp = h.loc.pos + (h.loc.size * h.loc.count);
			/* compute maximum */
			if (hpos < temp)
				hpos = temp;
			if (h.report_ID != 0)
				report_id = 1;
		}
	}
	hid_end_parse(d);

	/* safety check - can happen in case of currupt descriptors */
	if (lpos > hpos)
		temp = 0;
	else
		temp = hpos - lpos;

	/* return length in bytes rounded up */
	return ((temp + 7) / 8 + report_id);
}

int
hid_tlc_locate(const void *desc, hid_size_t size, int32_t u, enum hid_kind k,
    uint8_t tlc_index, uint8_t index, struct hid_location *loc,
    uint32_t *flags, uint8_t *id, struct hid_absinfo *ai)
{
	struct hid_data *d;
	struct hid_item h;

	d = hid_start_parse(desc, size, 1 << k);
	HID_TLC_FOREACH_ITEM(d, &h, tlc_index) {
		if (h.kind == k && h.usage == u) {
			if (index--)
				continue;
			if (loc != NULL)
				*loc = h.loc;
			if (flags != NULL)
				*flags = h.flags;
			if (id != NULL)
				*id = h.report_ID;
			if (ai != NULL && (h.flags & HIO_RELATIVE) == 0)
				*ai = (struct hid_absinfo) {
					.max = h.logical_maximum,
					.min = h.logical_minimum,
					.res = hid_item_resolution(&h),
				};
			hid_end_parse(d);
			return (1);
		}
	}
	if (loc != NULL)
		loc->size = 0;
	if (flags != NULL)
		*flags = 0;
	if (id != NULL)
		*id = 0;
	hid_end_parse(d);
	return (0);
}

/*------------------------------------------------------------------------*
 *	hid_test_quirk - test a device for a given quirk
 *
 * Return values:
 * false: The HID device does not have the given quirk.
 * true: The HID device has the given quirk.
 *------------------------------------------------------------------------*/
bool
hid_test_quirk(const struct hid_device_info *dev_info, uint16_t quirk)
{
	bool found;
	uint8_t x;

	if (quirk == HQ_NONE)
		return (false);

	/* search the automatic per device quirks first */
	for (x = 0; x != HID_MAX_AUTO_QUIRK; x++) {
		if (dev_info->autoQuirk[x] == quirk)
			return (true);
	}

	/* search global quirk table, if any */
	found = (hid_test_quirk_p) (dev_info, quirk);

	return (found);
}

static bool
hid_test_quirk_w(const struct hid_device_info *dev_info, uint16_t quirk)
{
	return (false);			/* no match */
}

int
hid_add_dynamic_quirk(struct hid_device_info *dev_info, uint16_t quirk)
{
	uint8_t x;

	for (x = 0; x != HID_MAX_AUTO_QUIRK; x++) {
		if (dev_info->autoQuirk[x] == 0 ||
		    dev_info->autoQuirk[x] == quirk) {
			dev_info->autoQuirk[x] = quirk;
			return (0);     /* success */
		}
	}
	return (ENOSPC);
}

void
hidquirk_unload(void *arg)
{
	/* reset function pointer */
	hid_test_quirk_p = &hid_test_quirk_w;
#ifdef NOT_YET
	hidquirk_ioctl_p = &hidquirk_ioctl_w;
#endif

	/* wait for CPU to exit the loaded functions, if any */

	/* XXX this is a tradeoff */

	pause("WAIT", hz);
}

int
hid_in_polling_mode(void)
{
	return (HID_IN_POLLING_MODE_VALUE());
}

MODULE_DEPEND(hid, usb, 1, 1, 1);
MODULE_VERSION(hid, 1);
