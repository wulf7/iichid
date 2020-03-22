/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
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
 *
 * $FreeBSD$
 */

#ifndef _HIDRAW_H
#define _HIDRAW_H

#include <sys/ioccom.h>

#define	HIDRAW_BUFFER_SIZE	64	/* number of input reports buffered */
#define	HID_MAX_DESCRIPTOR_SIZE	4096	/* artificial limit taken from Linux */

struct hidraw_report_descriptor {
	uint32_t	size;
	uint8_t		value[HID_MAX_DESCRIPTOR_SIZE];
};

struct hidraw_devinfo {
	uint32_t	bustype;
	int16_t		vendor;
	int16_t		product;
};

#define	HIDIOCGRDESCSIZE	_IOR('H', 0x01, int)
#define	HIDIOCGRDESC		_IO('H', 0x02)
#define	HIDIOCGRAWINFO		_IOR('H', 0x03, struct hidraw_devinfo)
#define	HIDIOCGRAWNAME(len)	_IOC(IOC_OUT, 'H', 0x04, len)
#define	HIDIOCGRAWPHYS(len)	_IOC(IOC_OUT, 'H', 0x05, len)
#define	HIDIOCSFEATURE(len)	_IOC(IOC_IN, 'H', 0x06, len)
#define	HIDIOCGFEATURE(len)	_IOC(IOC_INOUT, 'H', 0x07, len)
#define	HIDIOCGRAWUNIQ(len)	_IOC(IOC_OUT, 'H', 0x08, len)

#endif	/* _HIDRAW_H */
