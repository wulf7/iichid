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

/* hidbus interrupts interface */
#define hid_intr_setup(bus, mtx, intr, ctx) \
	(HID_INTR_SETUP(device_get_parent(bus), mtx, intr, ctx))
#define hid_intr_unsetup(bus) \
	(HID_INTR_UNSETUP(device_get_parent(bus)))
#define hid_intr_start(bus) \
	(HID_INTR_START(device_get_parent(bus)))
#define hid_intr_stop(bus) \
	(HID_INTR_STOP(device_get_parent(bus)))

/* hidbus HID interface */
#define hid_get_report_desc(bus, buf, len) \
	(HID_GET_REPORT_DESC(device_get_parent(bus), buf, len))
#define hid_get_input_report(bus, buf, len) \
	(HID_GET_INPUT_REPORT(device_get_parent(bus), buf, len))
#define hid_set_output_report(bus, buf, len) \
	(HID_SET_OUTPUT_REPORT(device_get_parent(bus), buf, len))
#define hid_get_report(bus, buf, len, type, id) \
	(HID_GET_REPORT(device_get_parent(bus), buf, len, type, id))
#define hid_set_report(bus, buf, len, type, id) \
	(HID_SET_REPORT(device_get_parent(bus), buf, len, type, id))

#endif	/* _HIDBUS_H_ */
