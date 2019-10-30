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
#include <sys/module.h>

#include "hid.h"

int
hid_tlc_locate(const void *desc, hid_size_t size, int32_t u, enum hid_kind k,
    uint8_t tlc_index, uint8_t index, struct hid_location *loc,
    uint32_t *flags, uint8_t *id, struct hid_absinfo *ai)
{
	struct hid_data *d;
	struct hid_item h;

	d = hid_start_parse(desc, size, 1 << k);
	HID_TLC_FOREACH_ITEM(d, &h, tlc_index) {
		if (h.kind == k && !(h.flags & HIO_CONST) && h.usage == u) {
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

MODULE_DEPEND(hid, usb, 1, 1, 1);
MODULE_VERSION(hid, 1);
