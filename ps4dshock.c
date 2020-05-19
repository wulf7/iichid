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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * Sony PS4 DualShock 4 driver
 * https://eleccelerator.com/wiki/index.php?title=DualShock_4
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include "hid.h"
#include "hidbus.h"
#include "hid_quirk.h"
#include "hmap.h"

#define	HID_DEBUG_VAR	ps4dshock_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int ps4dshock_debug = 1;

static SYSCTL_NODE(_hw_hid, OID_AUTO, ps4dshock, CTLFLAG_RW, 0,
		"Sony PS4 DualShock Gamepad");
SYSCTL_INT(_hw_hid_ps4dshock, OID_AUTO, debug, CTLFLAG_RWTUN,
		&ps4dshock_debug, 0, "Debug level");
#endif

#define	PS4DS_NAME	"Sony PS4 Dualshock 4"

/*
 * Hardware timestamp export is functional but as of May 2020 it does not
 * fully supported by libinput. Disable it for now as it results in extra
 * userland wakeups when touch state does not change between consecutive
 * reports. Evdev tries to filter out such an events but ever changing
 * timestamp interferes with that.
 */
/* #define	PS4DSMTP_ENABLE_HW_TIMESTAMPS	1 */

static const uint8_t	ps4dshock_rdesc[] = {
	0x05, 0x01,		// Usage Page (Generic Desktop Ctrls)
	0x09, 0x05,		// Usage (Game Pad)
	0xA1, 0x01,		// Collection (Application)
	0x85, 0x01,		//   Report ID (1)
	0x09, 0x30,		//   Usage (X)
	0x09, 0x31,		//   Usage (Y)
	0x09, 0x32,		//   Usage (Z)
	0x09, 0x35,		//   Usage (Rz)
	0x15, 0x00,		//   Logical Minimum (0)
	0x26, 0xFF, 0x00,	//   Logical Maximum (255)
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x04,		//   Report Count (4)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x09, 0x39,		//   Usage (Hat switch)
	0x15, 0x00,		//   Logical Minimum (0)
	0x25, 0x07,		//   Logical Maximum (7)
	0x35, 0x00,		//   Physical Minimum (0)
	0x46, 0x3B, 0x01,	//   Physical Maximum (315)
	0x65, 0x14,		//   Unit (System: English Rotation, Length: Centimeter)
	0x75, 0x04,		//   Report Size (4)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x42,		//   Input (Data,Var,Abs,Null State)
	0x65, 0x00,		//   Unit (None)
	0x45, 0x00,		//   Physical Maximum (0)
	0x05, 0x09,		//   Usage Page (Button)
	0x19, 0x01,		//   Usage Minimum (0x01)
	0x29, 0x0E,		//   Usage Maximum (0x0E)
	0x15, 0x00,		//   Logical Minimum (0)
	0x25, 0x01,		//   Logical Maximum (1)
	0x75, 0x01,		//   Report Size (1)
	0x95, 0x0E,		//   Report Count (14)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x06, 0x00, 0xFF,	//   Usage Page (Vendor Defined 0xFF00)
	0x09, 0x20,		//   Usage (0x20)
	0x75, 0x06,		//   Report Size (6)
	0x95, 0x01,		//   Report Count (1)
	0x15, 0x00,		//   Logical Minimum (0)
	0x25, 0x3F,		//   Logical Maximum (63)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x05, 0x01,		//   Usage Page (Generic Desktop Ctrls)
	0x09, 0x33,		//   Usage (Rx)
	0x09, 0x34,		//   Usage (Ry)
	0x15, 0x00,		//   Logical Minimum (0)
	0x26, 0xFF, 0x00,	//   Logical Maximum (255)
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x02,		//   Report Count (2)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x06, 0x00, 0xFF,	//   Usage Page (Vendor Defined 0xFF00)
	0x09, 0x21,		//   Usage (0x21)
	0x27, 0xFF, 0xFF, 0x00, 0x00,	//   Logical Maximum (65534)
	0x75, 0x10,		//   Report Size (16)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x05, 0x06,		//   Usage Page (Generic Dev Ctrls)
	0x09, 0x20,		//   Usage (Battery Strength)
	0x26, 0xFF, 0x00,	//   Logical Maximum (255)
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0xC0,			// End Collection
	0x05, 0x01,		// Usage Page (Generic Desktop Ctrls)
	0x09, 0x08,		// Usage (Multi-axis Controller)
	0xA1, 0x01,		// Collection (Application)
	0x05, 0x01,		//   Usage Page (Generic Desktop Ctrls)
	0x19, 0x30,		//   Usage Minimum (X)
	0x29, 0x32,		//   Usage Maximum (Z)
	0x16, 0x00, 0x80,	//   Logical Minimum (-32768)
	0x26, 0xFF, 0x7F,	//   Logical Maximum (32767)
	0x75, 0x10,		//   Report Size (16)
	0x95, 0x03,		//   Report Count (3)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x19, 0x33,		//   Usage Minimum (RX)
	0x29, 0x35,		//   Usage Maximum (RZ)
	0x16, 0x00, 0x80,	//   Logical Minimum (-32768)
	0x26, 0xFF, 0x7F,	//   Logical Maximum (32767)
	0x95, 0x03,		//   Report Count (3)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x06, 0x00, 0xFF,	//   Usage Page (Vendor Defined 0xFF00)
	0x09, 0x21,		//   Usage (0x21)
	0x15, 0x00,		//   Logical Minimum (0)
	0x26, 0xFF, 0x00,	//   Logical Maximum (255)
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x05,		//   Report Count (5)
	0x81, 0x03,		//   Input (Const)
	0xC0,			// End Collection
	0x05, 0x0C,		// Usage Page (Consumer)
	0x09, 0x05,		// Usage (Headphone)
	0xA1, 0x01,		// Collection (Application)
	0x75, 0x05,		//   Report Size (5)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x03,		//   Input (Const)
	0x06, 0x00, 0xFF,	//   Usage Page (Vendor Defined 0xFF00)
	0x09, 0x20,		//   Usage (0x20)
	0x09, 0x21,		//   Usage (0x21)
	0x15, 0x00,		//   Logical Minimum (0)
	0x25, 0x01,		//   Logical Maximum (1)
	0x75, 0x01,		//   Report Size (1)
	0x95, 0x02,		//   Report Count (2)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x75, 0x01,		//   Report Size (1)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x03,		//   Input (Const)
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x02,		//   Report Count (2)
	0x81, 0x03,		//   Input (Const)
	0xC0,			// End Collection
	0x05, 0x0D,		// Usage Page (Digitizer)
	0x09, 0x05,		// Usage (Touch Pad)
	0xA1, 0x01,		// Collection (Application)
	0x06, 0x00, 0xFF,	//   Usage Page (Vendor Defined 0xFF00)
	0x09, 0x21,		//   Usage (0x21)
	0x15, 0x00,		//   Logical Minimum (0)
	0x25, 0x03,		//   Logical Maximum (3)
	0x75, 0x04,		//   Report Size (4)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x75, 0x04,		//   Report Size (4)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x03,		//   Input (Data,Var,Abs)
	0x05, 0x0D,		//   Usage Page (Digitizer)
	0x09, 0x56,		//   Usage (0x56)
	0x55, 0x0C,		//   Unit Exponent (-4)
	0x66, 0x01, 0x10,	//   Unit (System: SI Linear, Time: Seconds)
	0x46, 0xCC, 0x06,	//   Physical Maximum (1740)
	0x26, 0xFF, 0x00,	//   Logical Maximum (255)
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x65, 0x00,		//   Unit (None)
	0x45, 0x00,		//   Physical Maximum (0)
	0x05, 0x0D,		//   Usage Page (Digitizer)
	0x09, 0x22,		//   Usage (Finger)
	0xA1, 0x02,		//   Collection (Logical)
	0x09, 0x51,		//     Usage (0x51)
	0x25, 0x7F,		//     Logical Maximum (127)
	0x75, 0x07,		//     Report Size (7)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x42,		//     Usage (Tip Switch)
	0x25, 0x01,		//     Logical Maximum (1)
	0x75, 0x01,		//     Report Size (1)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x05, 0x01,		//     Usage Page (Generic Desktop Ctrls)
	0x09, 0x30,		//     Usage (X)
	0x55, 0x0E,		//     Unit Exponent (-2)
	0x65, 0x11,		//     Unit (System: SI Linear, Length: Centimeter)
	0x35, 0x00,		//     Physical Minimum (0)
	0x46, 0x80, 0x02,	//     Physical Maximum (640)
	0x26, 0x80, 0x07,	//     Logical Maximum (1920)
	0x75, 0x0C,		//     Report Size (12)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x31,		//     Usage (Y)
	0x46, 0xC0, 0x00,	//     Physical Maximum (192)
	0x26, 0xAE, 0x03,	//     Logical Maximum (942)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x65, 0x00,		//     Unit (None)
	0x45, 0x00,		//     Physical Maximum (0)
	0xC0,			//   End Collection
	0x05, 0x0D,		//   Usage Page (Digitizer)
	0x09, 0x22,		//   Usage (Finger)
	0xA1, 0x02,		//   Collection (Logical)
	0x09, 0x51,		//     Usage (0x51)
	0x25, 0x7F,		//     Logical Maximum (127)
	0x75, 0x07,		//     Report Size (7)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x42,		//     Usage (Tip Switch)
	0x25, 0x01,		//     Logical Maximum (1)
	0x75, 0x01,		//     Report Size (1)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x05, 0x01,		//     Usage Page (Generic Desktop Ctrls)
	0x09, 0x30,		//     Usage (X)
	0x55, 0x0E,		//     Unit Exponent (-2)
	0x65, 0x11,		//     Unit (System: SI Linear, Length: Centimeter)
	0x35, 0x00,		//     Physical Minimum (0)
	0x46, 0x80, 0x02,	//     Physical Maximum (640)
	0x26, 0x80, 0x07,	//     Logical Maximum (1920)
	0x75, 0x0C,		//     Report Size (12)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x31,		//     Usage (Y)
	0x46, 0xC0, 0x00,	//     Physical Maximum (192)
	0x26, 0xAE, 0x03,	//     Logical Maximum (942)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x65, 0x00,		//     Unit (None)
	0x45, 0x00,		//     Physical Maximum (0)
	0xC0,			//   End Collection
	0x05, 0x0D,		//   Usage Page (Digitizer)
	0x09, 0x56,		//   Usage (0x56)
	0x55, 0x0C,		//   Unit Exponent (-4)
	0x66, 0x01, 0x10,	//   Unit (System: SI Linear, Time: Seconds)
	0x46, 0xCC, 0x06,	//   Physical Maximum (1740)
	0x26, 0xFF, 0x00,	//   Logical Maximum (255)
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x65, 0x00,		//   Unit (None)
	0x45, 0x00,		//   Physical Maximum (0)
	0x05, 0x0D,		//   Usage Page (Digitizer)
	0x09, 0x22,		//   Usage (Finger)
	0xA1, 0x02,		//   Collection (Logical)
	0x09, 0x51,		//     Usage (0x51)
	0x25, 0x7F,		//     Logical Maximum (127)
	0x75, 0x07,		//     Report Size (7)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x42,		//     Usage (Tip Switch)
	0x25, 0x01,		//     Logical Maximum (1)
	0x75, 0x01,		//     Report Size (1)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x05, 0x01,		//     Usage Page (Generic Desktop Ctrls)
	0x09, 0x30,		//     Usage (X)
	0x55, 0x0E,		//     Unit Exponent (-2)
	0x65, 0x11,		//     Unit (System: SI Linear, Length: Centimeter)
	0x35, 0x00,		//     Physical Minimum (0)
	0x46, 0x80, 0x02,	//     Physical Maximum (640)
	0x26, 0x80, 0x07,	//     Logical Maximum (1920)
	0x75, 0x0C,		//     Report Size (12)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x31,		//     Usage (Y)
	0x46, 0xC0, 0x00,	//     Physical Maximum (192)
	0x26, 0xAE, 0x03,	//     Logical Maximum (942)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x65, 0x00,		//     Unit (None)
	0x45, 0x00,		//     Physical Maximum (0)
	0xC0,			//   End Collection
	0x05, 0x0D,		//   Usage Page (Digitizer)
	0x09, 0x22,		//   Usage (Finger)
	0xA1, 0x02,		//   Collection (Logical)
	0x09, 0x51,		//     Usage (0x51)
	0x25, 0x7F,		//     Logical Maximum (127)
	0x75, 0x07,		//     Report Size (7)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x42,		//     Usage (Tip Switch)
	0x25, 0x01,		//     Logical Maximum (1)
	0x75, 0x01,		//     Report Size (1)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x05, 0x01,		//     Usage Page (Generic Desktop Ctrls)
	0x09, 0x30,		//     Usage (X)
	0x55, 0x0E,		//     Unit Exponent (-2)
	0x65, 0x11,		//     Unit (System: SI Linear, Length: Centimeter)
	0x35, 0x00,		//     Physical Minimum (0)
	0x46, 0x80, 0x02,	//     Physical Maximum (640)
	0x26, 0x80, 0x07,	//     Logical Maximum (1920)
	0x75, 0x0C,		//     Report Size (12)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x31,		//     Usage (Y)
	0x46, 0xC0, 0x00,	//     Physical Maximum (192)
	0x26, 0xAE, 0x03,	//     Logical Maximum (942)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x65, 0x00,		//     Unit (None)
	0x45, 0x00,		//     Physical Maximum (0)
	0xC0,			//   End Collection
	0x05, 0x0D,		//   Usage Page (Digitizer)
	0x09, 0x56,		//   Usage (0x56)
	0x55, 0x0C,		//   Unit Exponent (-4)
	0x66, 0x01, 0x10,	//   Unit (System: SI Linear, Time: Seconds)
	0x46, 0xCC, 0x06,	//   Physical Maximum (1740)
	0x26, 0xFF, 0x00,	//   Logical Maximum (255)
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x65, 0x00,		//   Unit (None)
	0x45, 0x00,		//   Physical Maximum (0)
	0x05, 0x0D,		//   Usage Page (Digitizer)
	0x09, 0x22,		//   Usage (Finger)
	0xA1, 0x02,		//   Collection (Logical)
	0x09, 0x51,		//     Usage (0x51)
	0x25, 0x7F,		//     Logical Maximum (127)
	0x75, 0x07,		//     Report Size (7)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x42,		//     Usage (Tip Switch)
	0x25, 0x01,		//     Logical Maximum (1)
	0x75, 0x01,		//     Report Size (1)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x05, 0x01,		//     Usage Page (Generic Desktop Ctrls)
	0x09, 0x30,		//     Usage (X)
	0x55, 0x0E,		//     Unit Exponent (-2)
	0x65, 0x11,		//     Unit (System: SI Linear, Length: Centimeter)
	0x35, 0x00,		//     Physical Minimum (0)
	0x46, 0x80, 0x02,	//     Physical Maximum (640)
	0x26, 0x80, 0x07,	//     Logical Maximum (1920)
	0x75, 0x0C,		//     Report Size (12)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x31,		//     Usage (Y)
	0x46, 0xC0, 0x00,	//     Physical Maximum (192)
	0x26, 0xAE, 0x03,	//     Logical Maximum (942)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x65, 0x00,		//     Unit (None)
	0x45, 0x00,		//     Physical Maximum (0)
	0xC0,			//   End Collection
	0x05, 0x0D,		//   Usage Page (Digitizer)
	0x09, 0x22,		//   Usage (Finger)
	0xA1, 0x02,		//   Collection (Logical)
	0x09, 0x51,		//     Usage (0x51)
	0x25, 0x7F,		//     Logical Maximum (127)
	0x75, 0x07,		//     Report Size (7)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x42,		//     Usage (Tip Switch)
	0x25, 0x01,		//     Logical Maximum (1)
	0x75, 0x01,		//     Report Size (1)
	0x95, 0x01,		//     Report Count (1)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x05, 0x01,		//     Usage Page (Generic Desktop Ctrls)
	0x09, 0x30,		//     Usage (X)
	0x55, 0x0E,		//     Unit Exponent (-2)
	0x65, 0x11,		//     Unit (System: SI Linear, Length: Centimeter)
	0x35, 0x00,		//     Physical Minimum (0)
	0x46, 0x80, 0x02,	//     Physical Maximum (640)
	0x26, 0x80, 0x07,	//     Logical Maximum (1920)
	0x75, 0x0C,		//     Report Size (12)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x09, 0x31,		//     Usage (Y)
	0x46, 0xC0, 0x00,	//     Physical Maximum (192)
	0x26, 0xAE, 0x03,	//     Logical Maximum (942)
	0x81, 0x02,		//     Input (Data,Var,Abs)
	0x65, 0x00,		//     Unit (None)
	0x45, 0x00,		//     Physical Maximum (0)
	0xC0,			//   End Collection
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x03,		//   Report Count (3)
	0x81, 0x03,		//   Input (Const)
	/* Output and feature reports */
	0x85, 0x05,		//   Report ID (5)
	0x06, 0x00, 0xFF,	//   Usage Page (Vendor Defined 0xFF00)
	0x09, 0x22,		//   Usage (0x22)
	0x15, 0x00,		//   Logical Minimum (0)
	0x26, 0xFF, 0x00,	//   Logical Maximum (255)
	0x95, 0x1F,		//   Report Count (31)
	0x91, 0x02,		//   Output (Data,Var,Abs)
	0x85, 0x04,		//   Report ID (4)
	0x09, 0x23,		//   Usage (0x23)
	0x95, 0x24,		//   Report Count (36)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x02,		//   Report ID (2)
	0x09, 0x24,		//   Usage (0x24)
	0x95, 0x24,		//   Report Count (36)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x08,		//   Report ID (8)
	0x09, 0x25,		//   Usage (0x25)
	0x95, 0x03,		//   Report Count (3)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x10,		//   Report ID (16)
	0x09, 0x26,		//   Usage (0x26)
	0x95, 0x04,		//   Report Count (4)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x11,		//   Report ID (17)
	0x09, 0x27,		//   Usage (0x27)
	0x95, 0x02,		//   Report Count (2)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x12,		//   Report ID (18)
	0x06, 0x02, 0xFF,	//   Usage Page (Vendor Defined 0xFF02)
	0x09, 0x21,		//   Usage (0x21)
	0x95, 0x0F,		//   Report Count (15)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x13,		//   Report ID (19)
	0x09, 0x22,		//   Usage (0x22)
	0x95, 0x16,		//   Report Count (22)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x14,		//   Report ID (20)
	0x06, 0x05, 0xFF,	//   Usage Page (Vendor Defined 0xFF05)
	0x09, 0x20,		//   Usage (0x20)
	0x95, 0x10,		//   Report Count (16)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x15,		//   Report ID (21)
	0x09, 0x21,		//   Usage (0x21)
	0x95, 0x2C,		//   Report Count (44)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x06, 0x80, 0xFF,	//   Usage Page (Vendor Defined 0xFF80)
	0x85, 0x80,		//   Report ID (-128)
	0x09, 0x20,		//   Usage (0x20)
	0x95, 0x06,		//   Report Count (6)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x81,		//   Report ID (-127)
	0x09, 0x21,		//   Usage (0x21)
	0x95, 0x06,		//   Report Count (6)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x82,		//   Report ID (-126)
	0x09, 0x22,		//   Usage (0x22)
	0x95, 0x05,		//   Report Count (5)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x83,		//   Report ID (-125)
	0x09, 0x23,		//   Usage (0x23)
	0x95, 0x01,		//   Report Count (1)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x84,		//   Report ID (-124)
	0x09, 0x24,		//   Usage (0x24)
	0x95, 0x04,		//   Report Count (4)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x85,		//   Report ID (-123)
	0x09, 0x25,		//   Usage (0x25)
	0x95, 0x06,		//   Report Count (6)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x86,		//   Report ID (-122)
	0x09, 0x26,		//   Usage (0x26)
	0x95, 0x06,		//   Report Count (6)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x87,		//   Report ID (-121)
	0x09, 0x27,		//   Usage (0x27)
	0x95, 0x23,		//   Report Count (35)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x88,		//   Report ID (-120)
	0x09, 0x28,		//   Usage (0x28)
	0x95, 0x22,		//   Report Count (34)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x89,		//   Report ID (-119)
	0x09, 0x29,		//   Usage (0x29)
	0x95, 0x02,		//   Report Count (2)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x90,		//   Report ID (-112)
	0x09, 0x30,		//   Usage (0x30)
	0x95, 0x05,		//   Report Count (5)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x91,		//   Report ID (-111)
	0x09, 0x31,		//   Usage (0x31)
	0x95, 0x03,		//   Report Count (3)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x92,		//   Report ID (-110)
	0x09, 0x32,		//   Usage (0x32)
	0x95, 0x03,		//   Report Count (3)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0x93,		//   Report ID (-109)
	0x09, 0x33,		//   Usage (0x33)
	0x95, 0x0C,		//   Report Count (12)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA0,		//   Report ID (-96)
	0x09, 0x40,		//   Usage (0x40)
	0x95, 0x06,		//   Report Count (6)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA1,		//   Report ID (-95)
	0x09, 0x41,		//   Usage (0x41)
	0x95, 0x01,		//   Report Count (1)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA2,		//   Report ID (-94)
	0x09, 0x42,		//   Usage (0x42)
	0x95, 0x01,		//   Report Count (1)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA3,		//   Report ID (-93)
	0x09, 0x43,		//   Usage (0x43)
	0x95, 0x30,		//   Report Count (48)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA4,		//   Report ID (-92)
	0x09, 0x44,		//   Usage (0x44)
	0x95, 0x0D,		//   Report Count (13)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA5,		//   Report ID (-91)
	0x09, 0x45,		//   Usage (0x45)
	0x95, 0x15,		//   Report Count (21)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA6,		//   Report ID (-90)
	0x09, 0x46,		//   Usage (0x46)
	0x95, 0x15,		//   Report Count (21)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xF0,		//   Report ID (-16)
	0x09, 0x47,		//   Usage (0x47)
	0x95, 0x3F,		//   Report Count (63)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xF1,		//   Report ID (-15)
	0x09, 0x48,		//   Usage (0x48)
	0x95, 0x3F,		//   Report Count (63)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xF2,		//   Report ID (-14)
	0x09, 0x49,		//   Usage (0x49)
	0x95, 0x0F,		//   Report Count (15)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA7,		//   Report ID (-89)
	0x09, 0x4A,		//   Usage (0x4A)
	0x95, 0x01,		//   Report Count (1)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA8,		//   Report ID (-88)
	0x09, 0x4B,		//   Usage (0x4B)
	0x95, 0x01,		//   Report Count (1)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xA9,		//   Report ID (-87)
	0x09, 0x4C,		//   Usage (0x4C)
	0x95, 0x08,		//   Report Count (8)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xAA,		//   Report ID (-86)
	0x09, 0x4E,		//   Usage (0x4E)
	0x95, 0x01,		//   Report Count (1)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xAB,		//   Report ID (-85)
	0x09, 0x4F,		//   Usage (0x4F)
	0x95, 0x39,		//   Report Count (57)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xAC,		//   Report ID (-84)
	0x09, 0x50,		//   Usage (0x50)
	0x95, 0x39,		//   Report Count (57)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xAD,		//   Report ID (-83)
	0x09, 0x51,		//   Usage (0x51)
	0x95, 0x0B,		//   Report Count (11)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xAE,		//   Report ID (-82)
	0x09, 0x52,		//   Usage (0x52)
	0x95, 0x01,		//   Report Count (1)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xAF,		//   Report ID (-81)
	0x09, 0x53,		//   Usage (0x53)
	0x95, 0x02,		//   Report Count (2)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0x85, 0xB0,		//   Report ID (-80)
	0x09, 0x54,		//   Usage (0x54)
	0x95, 0x3F,		//   Report Count (63)
	0xB1, 0x02,		//   Feature (Data,Var,Abs)
	0xC0,			// End Collection
};

#define	PS4DS_MAX_TOUCHPAD_PACKETS	4
#define	PS4DS_OUTPUT_REPORT5_SIZE	32
#define	PS4DS_OUTPUT_REPORT11_SIZE	78

static hmap_cb_t	ps4dshock_hat_switch_cb;
static hmap_cb_t	ps4dshock_compl_cb;
static hmap_cb_t	ps4dsacc_compl_cb;
static hmap_cb_t	ps4dsmtp_data_cb;
static hmap_cb_t	ps4dsmtp_npackets_cb;
static hmap_cb_t	ps4dsmtp_compl_cb;

struct ps4ds_out5 {
	uint8_t features;
	uint8_t	reserved1;
	uint8_t	reserved2;
	uint8_t	rumble_right;
	uint8_t	rumble_left;
	uint8_t	led_color_r;
	uint8_t	led_color_g;
	uint8_t	led_color_b;
	uint8_t	led_delay_on;
	uint8_t	led_delay_off;
} __attribute__((packed));

static const struct ps4ds_led {
	uint8_t	r;
	uint8_t	g;
	uint8_t	b;
} ps4ds_leds[] = {
	/* The first 4 entries match the PS4, other from Linux driver */
	{ 0x00, 0x00, 0x40 },	/* Blue   */
	{ 0x40, 0x00, 0x00 },	/* Red	  */
	{ 0x00, 0x40, 0x00 },	/* Green  */
	{ 0x20, 0x00, 0x20 },	/* Pink   */
	{ 0x02, 0x01, 0x00 },	/* Orange */
	{ 0x00, 0x01, 0x01 },	/* Teal   */
	{ 0x01, 0x01, 0x01 }	/* White  */
};

enum ps4ds_led_state {
	PS4DS_LED_OFF,
	PS4DS_LED_ON,
	PS4DS_LED_BLINKING,
};

enum {
#ifdef PS4DSMTP_ENABLE_HW_TIMESTAMPS
	PS4DS_TSTAMP,
#endif
	PS4DS_CID1,
	PS4DS_TIP1,
	PS4DS_X1,
	PS4DS_Y1,
	PS4DS_CID2,
	PS4DS_TIP2,
	PS4DS_X2,
	PS4DS_Y2,
	PS4DS_NTPUSAGES,
};

struct ps4dshock_softc {
	struct hmap_softc	super_sc;

	bool			is_bluetooth;

	enum ps4ds_led_state	led_state;
	struct ps4ds_led	led_color;
	uint8_t			led_delay_on;
	uint8_t			led_delay_off;

	uint8_t			rumble_right;
	uint8_t			rumble_left;
};

struct ps4dsmtp_softc {
	struct hmap_softc	super_sc;

	struct hid_location	btn_loc;
	u_int		npackets;
	int32_t		*data_ptr;
	int32_t		data[PS4DS_MAX_TOUCHPAD_PACKETS][PS4DS_NTPUSAGES];

#ifdef PS4DSMTP_ENABLE_HW_TIMESTAMPS
	uint8_t		hw_tstamp;
	int32_t		ev_tstamp;
	bool		touch;
#endif
};

#define PS4DS_MAP_BTN(number, code)		\
	{ HMAP_KEY(HUP_BUTTON, number, code) }
#define PS4DS_MAP_ABS(usage, code)		\
	{ HMAP_ABS(HUP_GENERIC_DESKTOP, HUG_##usage, code) }
#define PS4DS_MAP_VSW(usage, code)	\
	{ HMAP_SW(HUP_MICROSOFT, usage, code) }
#define PS4DS_MAP_GCB(usage, callback)	\
	{ HMAP_ANY_CB(HUP_GENERIC_DESKTOP, HUG_##usage, callback) }
#define PS4DS_MAP_VCB(usage, callback)	\
	{ HMAP_ANY_CB(HUP_MICROSOFT, usage, callback) }
#define PS4DS_COMPLCB(cb)			\
	{ HMAP_COMPL_CB(&cb) }

static const struct hmap_item ps4dshock_map[] = {
	PS4DS_MAP_ABS(X,		ABS_X),
	PS4DS_MAP_ABS(Y,		ABS_Y),
	PS4DS_MAP_ABS(Z,		ABS_Z),
	PS4DS_MAP_ABS(RX,		ABS_RX),
	PS4DS_MAP_ABS(RY,		ABS_RY),
	PS4DS_MAP_ABS(RZ,		ABS_RZ),
	PS4DS_MAP_BTN(1,		BTN_WEST),
	PS4DS_MAP_BTN(2,		BTN_SOUTH),
	PS4DS_MAP_BTN(3,		BTN_EAST),
	PS4DS_MAP_BTN(4,		BTN_NORTH),
	PS4DS_MAP_BTN(5,		BTN_TL),
	PS4DS_MAP_BTN(6,		BTN_TR),
	PS4DS_MAP_BTN(7,		BTN_TL2),
	PS4DS_MAP_BTN(8,		BTN_TR2),
	PS4DS_MAP_BTN(9,		BTN_SELECT),
	PS4DS_MAP_BTN(10,		BTN_START),
	PS4DS_MAP_BTN(11,		BTN_THUMBL),
	PS4DS_MAP_BTN(12,		BTN_THUMBR),
	PS4DS_MAP_BTN(13,		BTN_MODE),
	/* Click button is handled by touchpad driver */
	/* PS4DS_MAP_BTN(14,	BTN_LEFT), */
	PS4DS_MAP_GCB(HAT_SWITCH,	ps4dshock_hat_switch_cb),
	PS4DS_COMPLCB(			ps4dshock_compl_cb),
};
static const struct hmap_item ps4dsacc_map[] = {
	PS4DS_MAP_ABS(X,		ABS_X),
	PS4DS_MAP_ABS(Y,		ABS_Y),
	PS4DS_MAP_ABS(Z,		ABS_Z),
	PS4DS_MAP_ABS(RX,		ABS_RX),
	PS4DS_MAP_ABS(RY,		ABS_RY),
	PS4DS_MAP_ABS(RZ,		ABS_RZ),
	PS4DS_COMPLCB(			ps4dsacc_compl_cb),
};
static const struct hmap_item ps4dshead_map[] = {
	PS4DS_MAP_VSW(0x0020,		SW_MICROPHONE_INSERT),
	PS4DS_MAP_VSW(0x0021,		SW_HEADPHONE_INSERT),
};
static const struct hmap_item ps4dsmtp_map[] = {

	{ HMAP_ABS_CB(HUP_MICROSOFT, 0x0021, 		ps4dsmtp_npackets_cb)},
#ifdef PS4DSMTP_ENABLE_HW_TIMESTAMPS
	{ HMAP_ABS_CB(HUP_DIGITIZERS, HUD_SCAN_TIME,	ps4dsmtp_data_cb) },
#endif
	{ HMAP_ABS_CB(HUP_DIGITIZERS, HUD_CONTACTID,	ps4dsmtp_data_cb) },
	{ HMAP_ABS_CB(HUP_DIGITIZERS, HUD_TIP_SWITCH,	ps4dsmtp_data_cb) },
	{ HMAP_ABS_CB(HUP_GENERIC_DESKTOP, HUG_X,	ps4dsmtp_data_cb) },
	{ HMAP_ABS_CB(HUP_GENERIC_DESKTOP, HUG_Y,	ps4dsmtp_data_cb) },
	{ HMAP_COMPL_CB(				ps4dsmtp_compl_cb) },
};

static const struct hid_device_id ps4dshock_devs[] = {
	{ HID_BVP(BUS_USB, 0x54c, 0x9cc),
	  HID_TLC(HUP_GENERIC_DESKTOP, HUG_GAME_PAD) },
};
static const struct hid_device_id ps4dsacc_devs[] = {
	{ HID_BVP(BUS_USB, 0x54c, 0x9cc),
	  HID_TLC(HUP_GENERIC_DESKTOP, HUG_MULTIAXIS_CNTROLLER) },
};
static const struct hid_device_id ps4dshead_devs[] = {
	{ HID_BVP(BUS_USB, 0x54c, 0x9cc),
	  HID_TLC(HUP_CONSUMER, HUC_HEADPHONE) },
};
static const struct hid_device_id ps4dsmtp_devs[] = {
	{ HID_BVP(BUS_USB, 0x54c, 0x9cc),
	  HID_TLC(HUP_DIGITIZERS, HUD_TOUCHPAD) },
};

static int
ps4dshock_hat_switch_cb(HMAP_CB_ARGS)
{
	static const struct { int32_t x; int32_t y; } hat_switch_map[] = {
	    {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0},
	    {-1, -1},{0, 0}
	};
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();
	u_int idx;

	switch (HMAP_CB_GET_STATE()) {
	case HMAP_CB_IS_ATTACHING:
		evdev_support_event(evdev, EV_ABS);
		evdev_support_abs(evdev, ABS_HAT0X, 0, -1, 1, 0, 0, 0);
		evdev_support_abs(evdev, ABS_HAT0Y, 0, -1, 1, 0, 0, 0);
		break;

	case HMAP_CB_IS_RUNNING:
		idx = MIN(nitems(hat_switch_map) - 1, (u_int)ctx);
		evdev_push_abs(evdev, ABS_HAT0X, hat_switch_map[idx].x);
		evdev_push_abs(evdev, ABS_HAT0Y, hat_switch_map[idx].y);
	}

	return (0);
}

static int
ps4dshock_compl_cb(HMAP_CB_ARGS)
{
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_ATTACHING)
		evdev_support_prop(evdev, INPUT_PROP_DIRECT);

        /* Do not execute callback at interrupt handler and detach */
        return (ENOSYS);
}

static int
ps4dsacc_compl_cb(HMAP_CB_ARGS)
{
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_ATTACHING)
		evdev_support_prop(evdev, INPUT_PROP_ACCELEROMETER);

        /* Do not execute callback at interrupt handler and detach */
        return (ENOSYS);
}

static int
ps4dsmtp_npackets_cb(HMAP_CB_ARGS)
{
	struct ps4dsmtp_softc *sc = HMAP_CB_GET_SOFTC();

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_RUNNING) {
		sc->npackets = MIN(PS4DS_MAX_TOUCHPAD_PACKETS, (u_int)ctx);
		/* Reset pointer here as it is first usage in touchpad TLC */
		sc->data_ptr = &sc->data[0][0];
	}

	return (0);
}

static int
ps4dsmtp_data_cb(HMAP_CB_ARGS)
{
	struct ps4dsmtp_softc *sc = HMAP_CB_GET_SOFTC();

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_RUNNING) {
		*sc->data_ptr = (int32_t)ctx;
		++sc->data_ptr;
	}

	return (0);
}

static int
ps4dsmtp_compl_cb(HMAP_CB_ARGS)
{
	struct ps4dsmtp_softc *sc = HMAP_CB_GET_SOFTC();
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();
	u_int i;
#ifdef PS4DSMTP_ENABLE_HW_TIMESTAMPS
	uint8_t hw_tstamp, hw_tstamp_diff;
	bool touch;
#endif

	switch (HMAP_CB_GET_STATE()) {
	case HMAP_CB_IS_ATTACHING:
		/*
		 * Dualshock 4 touchpad TLC contained in fixed report
		 * descriptor is almost compatible with MS precission touchpad
		 * specs and hmt(4) driver. But... for some reasons "Click"
		 * button location was grouped with other GamePad buttons by
		 * touchpad designers so it belongs to GamePad TLC. Fix it with
		 * direct reading of "Click" button value from interrupt frame.
		 */
		sc->btn_loc = (struct hid_location) { 1, 0, 49 };
		evdev_support_event(evdev, EV_SYN);
		evdev_support_event(evdev, EV_KEY);
		evdev_support_event(evdev, EV_ABS);
		evdev_support_event(evdev, EV_MSC);
		evdev_support_key(evdev, BTN_LEFT);
#ifdef PS4DSMTP_ENABLE_HW_TIMESTAMPS
		evdev_support_msc(evdev, MSC_TIMESTAMP);
#endif
		evdev_support_abs(evdev, ABS_MT_SLOT, 0, 0, 1, 0, 0, 0);
		evdev_support_abs(evdev, ABS_MT_TRACKING_ID, 0, -1, 127, 0, 0, 0);
		evdev_support_abs(evdev, ABS_MT_POSITION_X, 0, 0, 1920, 0, 0, 30);
		evdev_support_abs(evdev, ABS_MT_POSITION_Y, 0, 0, 942, 0, 0, 49);
		evdev_support_prop(evdev, INPUT_PROP_POINTER);
		evdev_support_prop(evdev, INPUT_PROP_BUTTONPAD);
		evdev_set_flag(evdev, EVDEV_FLAG_MT_STCOMPAT);
		break;
	case HMAP_CB_IS_RUNNING:
		/* Only packets with ReportID=1 are accepted */
		if (ctx != 1)
			return (ENOTSUP);
		evdev_push_key(evdev, BTN_LEFT,
		    HMAP_CB_GET_UDATA(&sc->btn_loc));
		for (i = 0; i < sc->npackets; i++) {
			evdev_push_abs(evdev, ABS_MT_SLOT, 0);
			if (sc->data[i][PS4DS_TIP1] == 0) {
				evdev_push_abs(evdev, ABS_MT_TRACKING_ID,
				    sc->data[i][PS4DS_CID1]);
				evdev_push_abs(evdev, ABS_MT_POSITION_X,
				    sc->data[i][PS4DS_X1]);
				evdev_push_abs(evdev, ABS_MT_POSITION_Y,
				    sc->data[i][PS4DS_Y1]);
			} else
				evdev_push_abs(evdev, ABS_MT_TRACKING_ID, -1);
			evdev_push_abs(evdev, ABS_MT_SLOT, 1);
			if (sc->data[i][PS4DS_TIP2] == 0) {
				evdev_push_abs(evdev, ABS_MT_TRACKING_ID,
				    sc->data[i][PS4DS_CID2]);
				evdev_push_abs(evdev, ABS_MT_POSITION_X,
				    sc->data[i][PS4DS_X2]);
				evdev_push_abs(evdev, ABS_MT_POSITION_Y,
				    sc->data[i][PS4DS_Y2]);
			} else
				evdev_push_abs(evdev, ABS_MT_TRACKING_ID, -1);
#ifdef PS4DSMTP_ENABLE_HW_TIMESTAMPS
			/*
			 * Export hardware timestamps in libinput-friendly way.
			 * Make timestamp counter 32-bit, scale up hardware
			 * timestamps to be on per 1usec basis and reset
			 * counter at the start of each touch.
			 */
			hw_tstamp = (uint8_t)sc->data[i][PS4DS_TSTAMP];
			hw_tstamp_diff = hw_tstamp - sc->hw_tstamp;
			sc->hw_tstamp = hw_tstamp;
			touch = sc->data[i][PS4DS_TIP1] == 0 ||
			    sc->data[i][PS4DS_TIP2] == 0;
			if (touch) {
				if (hw_tstamp_diff != 0) {
					if (sc->touch)
						/*
						 * Hardware timestamp counter
						 * ticks in 682 usec interval.
						 */
						sc->ev_tstamp += hw_tstamp_diff
						    * 682;
					evdev_push_msc(evdev, MSC_TIMESTAMP,
					    sc->ev_tstamp);
				}
			} else
				sc->ev_tstamp = 0;
			sc->touch = touch;
#endif
			evdev_sync(evdev);
		}
		break;
	}

	/* Do execute callback at interrupt handler and detach */
	return (0);
}

static int
ps4dshock_write(struct ps4dshock_softc *sc)
{
	hid_size_t osize = sc->is_bluetooth ?
	    PS4DS_OUTPUT_REPORT11_SIZE : PS4DS_OUTPUT_REPORT5_SIZE;
	uint8_t buf[osize];
	int offset;
	bool led_on, led_blinks;

	memset(buf, 0, osize);
	buf[0] = sc->is_bluetooth ? 0x11 : 0x05;
	offset = sc->is_bluetooth ? 3 : 1;
	led_on = sc->led_state != PS4DS_LED_OFF;
	led_blinks = sc->led_state == PS4DS_LED_BLINKING;
	*(struct ps4ds_out5 *)(buf + offset) = (struct ps4ds_out5) {
		.features = 0x07, /* blink + LEDs + motor */
		.rumble_right = sc->rumble_right,
		.rumble_left = sc->rumble_left,
		.led_color_r = led_on ? sc->led_color.r : 0,
		.led_color_g = led_on ? sc->led_color.g : 0,
		.led_color_b = led_on ? sc->led_color.b : 0,
		.led_delay_on = led_blinks ? sc->led_delay_on : 0,
		.led_delay_off = led_blinks ? sc->led_delay_off : 0,
	};

	/*
	 * The lower 6 bits of buf[1] field of the Bluetooth report
	 * control the interval at which Dualshock 4 reports data:
	 * 0x00 - 1ms
	 * 0x01 - 1ms
	 * 0x02 - 2ms
	 * 0x3E - 62ms
	 * 0x3F - disabled
	 */
//	if (sc->sc->is_bluetooth) {
//		buf[1] = 0xC0 /* HID + CRC */ | sc->bt_poll_interval;
		/* CRC generation */
//		uint8_t bthdr = 0xA2;
//		uint32_t crc;

//		crc = crc32_le(0xFFFFFFFF, &bthdr, 1);
//		crc = ~crc32_le(crc, buf, osize - 4);
//		put_unaligned_le32(crc, &buf[74]);
//	}

	return (hid_write(sc->super_sc.dev, buf, osize));
}

static void
ps4dshock_identify(driver_t *driver, device_t parent)
{

	/* Overload PS4 DualShock gamepad rudimentary report descriptor */
	if (hidbus_lookup_id(parent, ps4dshock_devs, sizeof(ps4dshock_devs))
	    != NULL)
		hid_set_report_descr(parent, ps4dshock_rdesc,
		    sizeof(ps4dshock_rdesc));
}

static int
ps4dshock_probe(device_t dev)
{
	int error;

	error = hidbus_lookup_driver_info(dev, ps4dshock_devs, sizeof(ps4dshock_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	error = hmap_add_map(dev, ps4dshock_map, nitems(ps4dshock_map), NULL);
	if (error != 0)
		return (error);

	device_set_desc(dev, PS4DS_NAME" Gamepad");

	return (BUS_PROBE_DEFAULT);
}

static int
ps4dsacc_probe(device_t dev)
{
	int error;

	error = hidbus_lookup_driver_info(dev, ps4dsacc_devs, sizeof(ps4dsacc_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	error = hmap_add_map(dev, ps4dsacc_map, nitems(ps4dsacc_map), NULL);
	if (error != 0)
		return (error);

	device_set_desc(dev, PS4DS_NAME" Sensors");

	return (BUS_PROBE_DEFAULT);
}

static int
ps4dshead_probe(device_t dev)
{
	int error;

	error = hidbus_lookup_driver_info(dev, ps4dshead_devs, sizeof(ps4dshead_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	error = hmap_add_map(dev, ps4dshead_map, nitems(ps4dshead_map), NULL);
	if (error != 0)
		return (error);

	device_set_desc(dev, PS4DS_NAME" Headset");

	return (BUS_PROBE_DEFAULT);
}

static int
ps4dsmtp_probe(device_t dev)
{
	int error;

	error = hidbus_lookup_driver_info(dev, ps4dsmtp_devs, sizeof(ps4dsmtp_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	error = hmap_add_map(dev, ps4dsmtp_map, nitems(ps4dsmtp_map), NULL);
	if (error != 0)
		return (error);

	device_set_desc(dev, PS4DS_NAME" Touchpad");

	return (BUS_PROBE_DEFAULT);
}

static int
ps4dshock_attach(device_t dev)
{
	struct ps4dshock_softc *sc = device_get_softc(dev);

	/* ps4dshock_write() needs super_sc.dev initialized */
	sc->super_sc.dev = dev;

	sc->led_state = PS4DS_LED_ON;
	sc->led_color = ps4ds_leds[device_get_unit(dev) % nitems(ps4ds_leds)];
	ps4dshock_write(sc);

	return (hmap_attach(dev));
}

static int
ps4dsacc_attach(device_t dev)
{
	uint8_t buf[37];
	int error;

	/* Read accelerometers and gyroscopes calibration data */
	error = hid_get_report(dev, buf, sizeof(buf), NULL,
	    HID_FEATURE_REPORT, 0x02);
	if (error)
		DPRINTF("get feature report failed, error=%d "
		    "(ignored)\n", error);

	return (hmap_attach(dev));
}

static devclass_t ps4dshock_devclass;
static devclass_t ps4dsacc_devclass;
static devclass_t ps4dshead_devclass;
static devclass_t ps4dsmtp_devclass;

static device_method_t ps4dshock_methods[] = {
	DEVMETHOD(device_identify,	ps4dshock_identify),
	DEVMETHOD(device_attach,	ps4dshock_attach),
	DEVMETHOD(device_probe,		ps4dshock_probe),
	DEVMETHOD_END
};
static device_method_t ps4dsacc_methods[] = {
	DEVMETHOD(device_probe,		ps4dsacc_probe),
	DEVMETHOD(device_attach,	ps4dsacc_attach),
	DEVMETHOD_END
};
static device_method_t ps4dshead_methods[] = {
	DEVMETHOD(device_probe,		ps4dshead_probe),
	DEVMETHOD_END
};
static device_method_t ps4dsmtp_methods[] = {
	DEVMETHOD(device_probe,		ps4dsmtp_probe),
	DEVMETHOD_END
};

DEFINE_CLASS_1(ps4dsacc, ps4dsacc_driver, ps4dsacc_methods,
    sizeof(struct hmap_softc), hmap_driver);
DRIVER_MODULE(ps4dsacc, hidbus, ps4dsacc_driver, ps4dsacc_devclass, NULL, 0);
DEFINE_CLASS_1(ps4dshead, ps4dshead_driver, ps4dshead_methods,
    sizeof(struct hmap_softc), hmap_driver);
DRIVER_MODULE(ps4dshead, hidbus, ps4dshead_driver, ps4dshead_devclass, NULL, 0);
DEFINE_CLASS_1(ps4dsmtp, ps4dsmtp_driver, ps4dsmtp_methods,
    sizeof(struct ps4dsmtp_softc), hmap_driver);
DRIVER_MODULE(ps4dsmtp, hidbus, ps4dsmtp_driver, ps4dsmtp_devclass, NULL, 0);
DEFINE_CLASS_1(ps4dshock, ps4dshock_driver, ps4dshock_methods,
    sizeof(struct ps4dshock_softc), hmap_driver);
DRIVER_MODULE(ps4dshock, hidbus, ps4dshock_driver, ps4dshock_devclass, NULL, 0);

MODULE_DEPEND(ps4dshock, hid, 1, 1, 1);
MODULE_DEPEND(ps4dshock, hmap, 1, 1, 1);
MODULE_DEPEND(ps4dshock, evdev, 1, 1, 1);
MODULE_VERSION(ps4dshock, 1);
