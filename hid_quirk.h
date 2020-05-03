/* $FreeBSD$ */
/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2008 Hans Petter Selasky. All rights reserved.
 * Copyright (c) 2020 Vladimir Kondratyev <wulf@FreeBSD.org>
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

#ifndef _HID_QUIRK_H_
#define	_HID_QUIRK_H_

enum {
	/*
	 * Keep in sync with hid_quirk_str in hid_quirk.c, and with
	 * share/man/man4/hid_quirk.4
	 */
	HQ_NONE,		/* not a valid quirk */

	HQ_MATCH_VENDOR_ONLY,	/* match quirk on vendor only */

	/* Autoquirks */
	HQ_HAS_KBD_BOOTPROTO,	/* device supports keyboard boot protocol */
	HQ_HAS_MS_BOOTPROTO,	/* device supports mouse boot protocol */
	HQ_IS_XBOX360GP, 	/* device is XBox 360 GamePad */
	HQ_NOWRITE,		/* device does not support writes */

	/* Various quirks */

	HQ_HID_IGNORE,		/* device should be ignored by hid class */
	HQ_KBD_BOOTPROTO,	/* device should set the boot protocol */
	HQ_MS_BAD_CLASS,	/* doesn't identify properly */
	HQ_MS_LEADING_BYTE,	/* mouse sends an unknown leading byte */
	HQ_MS_REVZ,		/* mouse has Z-axis reversed */
	HQ_SPUR_BUT_UP,		/* spurious mouse button up events */

	HID_QUIRK_MAX
};

#endif					/* _HID_QUIRK_H_ */
