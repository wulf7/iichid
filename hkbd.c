#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");


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
 *
 */

/*
 * HID spec: http://www.usb.org/developers/devclass_docs/HID1_11.pdf
 */

#include "opt_kbd.h"
#include "opt_ukbd.h"
#include "opt_evdev.h"

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>
#include <sys/proc.h>

#include "hid.h"
#include "hidbus.h"

#define	HID_DEBUG_VAR hkbd_debug
#include "hid_debug.h"

#ifdef NOT_YET
#include <dev/usb/quirk/usb_quirk.h>
#endif

#ifdef EVDEV_SUPPORT
#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>
#endif

#include <sys/ioccom.h>
#include <sys/filio.h>
#include <sys/tty.h>
#include <sys/kbio.h>

#include <dev/kbd/kbdreg.h>

/* the initial key map, accent map and fkey strings */
#if defined(HKBD_DFLT_KEYMAP) && !defined(KLD_MODULE)
#define	KBD_DFLT_KEYMAP
#include "ukbdmap.h"
#endif

/* the following file must be included after "ukbdmap.h" */
#include <dev/kbd/kbdtables.h>

#ifdef HID_DEBUG
static int hkbd_debug = 0;
static int hkbd_no_leds = 0;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hkbd, CTLFLAG_RW, 0, "USB keyboard");
SYSCTL_INT(_hw_hid_hkbd, OID_AUTO, debug, CTLFLAG_RWTUN,
    &hkbd_debug, 0, "Debug level");
SYSCTL_INT(_hw_hid_hkbd, OID_AUTO, no_leds, CTLFLAG_RWTUN,
    &hkbd_no_leds, 0, "Disables setting of keyboard leds");
#endif

#define	HKBD_EMULATE_ATSCANCODE	       1
#define	HKBD_DRIVER_NAME          "hkbd"
#define	HKBD_NMOD                     8	/* units */
#define	HKBD_NKEYCODE                 6	/* units */
#define	HKBD_IN_BUF_SIZE  (2*(HKBD_NMOD + (2*HKBD_NKEYCODE)))	/* bytes */
#define	HKBD_IN_BUF_FULL  ((HKBD_IN_BUF_SIZE / 2) - 1)	/* bytes */
#define	HKBD_NFKEY        (sizeof(fkey_tab)/sizeof(fkey_tab[0]))	/* units */
#define	HKBD_BUFFER_SIZE	      64	/* bytes */

struct hkbd_data {
	uint16_t	modifiers;
#define	MOD_CONTROL_L	0x01
#define	MOD_CONTROL_R	0x10
#define	MOD_SHIFT_L	0x02
#define	MOD_SHIFT_R	0x20
#define	MOD_ALT_L	0x04
#define	MOD_ALT_R	0x40
#define	MOD_WIN_L	0x08
#define	MOD_WIN_R	0x80
/* internal */
#define	MOD_EJECT	0x0100
#define	MOD_FN		0x0200
	uint8_t	keycode[HKBD_NKEYCODE];
};

struct hkbd_softc {
	device_t sc_dev;
	struct mtx *sc_lock;	/* Giant */

	keyboard_t sc_kbd;
	keymap_t sc_keymap;
	accentmap_t sc_accmap;
	fkeytab_t sc_fkeymap[HKBD_NFKEY];
	struct hid_location sc_loc_apple_eject;
	struct hid_location sc_loc_apple_fn;
	struct hid_location sc_loc_ctrl_l;
	struct hid_location sc_loc_ctrl_r;
	struct hid_location sc_loc_shift_l;
	struct hid_location sc_loc_shift_r;
	struct hid_location sc_loc_alt_l;
	struct hid_location sc_loc_alt_r;
	struct hid_location sc_loc_win_l;
	struct hid_location sc_loc_win_r;
	struct hid_location sc_loc_events;
	struct hid_location sc_loc_numlock;
	struct hid_location sc_loc_capslock;
	struct hid_location sc_loc_scrolllock;
	struct callout sc_callout;
	struct hkbd_data sc_ndata;
	struct hkbd_data sc_odata;

	struct thread *sc_poll_thread;
#ifdef EVDEV_SUPPORT
	struct evdev_dev *sc_evdev;
#endif

	sbintime_t sc_co_basetime;
	int	sc_delay;
	uint32_t sc_ntime[HKBD_NKEYCODE];
	uint32_t sc_otime[HKBD_NKEYCODE];
	uint32_t sc_input[HKBD_IN_BUF_SIZE];	/* input buffer */
	uint32_t sc_time_ms;
	uint32_t sc_composed_char;	/* composed char code, if non-zero */
#ifdef HKBD_EMULATE_ATSCANCODE
	uint32_t sc_buffered_char[2];
#endif
	uint32_t sc_flags;		/* flags */
#define	HKBD_FLAG_COMPOSE	0x00000001
#define	HKBD_FLAG_POLLING	0x00000002
#define	HKBD_FLAG_ATTACHED	0x00000010
#define	HKBD_FLAG_GONE		0x00000020

#define	HKBD_FLAG_HID_MASK	0x003fffc0
#define	HKBD_FLAG_APPLE_EJECT	0x00000040
#define	HKBD_FLAG_APPLE_FN	0x00000080
#define	HKBD_FLAG_APPLE_SWAP	0x00000100
#define	HKBD_FLAG_CTRL_L	0x00000400
#define	HKBD_FLAG_CTRL_R	0x00000800
#define	HKBD_FLAG_SHIFT_L	0x00001000
#define	HKBD_FLAG_SHIFT_R	0x00002000
#define	HKBD_FLAG_ALT_L		0x00004000
#define	HKBD_FLAG_ALT_R		0x00008000
#define	HKBD_FLAG_WIN_L		0x00010000
#define	HKBD_FLAG_WIN_R		0x00020000
#define	HKBD_FLAG_EVENTS	0x00040000
#define	HKBD_FLAG_NUMLOCK	0x00080000
#define	HKBD_FLAG_CAPSLOCK	0x00100000
#define	HKBD_FLAG_SCROLLLOCK 	0x00200000

	int	sc_mode;		/* input mode (K_XLATE,K_RAW,K_CODE) */
	int	sc_state;		/* shift/lock key state */
	int	sc_accents;		/* accent key index (> 0) */
	int	sc_polling;		/* polling recursion count */
	int	sc_led_size;
	int	sc_kbd_size;

	uint16_t sc_inputs;
	uint16_t sc_inputhead;
	uint16_t sc_inputtail;
	uint16_t sc_modifiers;

	uint8_t	sc_iface_index;
	uint8_t	sc_iface_no;
	uint8_t sc_id_apple_eject;
	uint8_t sc_id_apple_fn;
	uint8_t sc_id_ctrl_l;
	uint8_t sc_id_ctrl_r;
	uint8_t sc_id_shift_l;
	uint8_t sc_id_shift_r;
	uint8_t sc_id_alt_l;
	uint8_t sc_id_alt_r;
	uint8_t sc_id_win_l;
	uint8_t sc_id_win_r;
	uint8_t sc_id_event;
	uint8_t sc_id_numlock;
	uint8_t sc_id_capslock;
	uint8_t sc_id_scrolllock;
	uint8_t sc_id_events;
	uint8_t sc_kbd_id;

	uint8_t sc_buffer[HKBD_BUFFER_SIZE];
};

#define	KEY_ERROR	  0x01

#define	KEY_PRESS	  0
#define	KEY_RELEASE	  0x400
#define	KEY_INDEX(c)	  ((c) & 0xFF)

#define	SCAN_PRESS	  0
#define	SCAN_RELEASE	  0x80
#define	SCAN_PREFIX_E0	  0x100
#define	SCAN_PREFIX_E1	  0x200
#define	SCAN_PREFIX_CTL	  0x400
#define	SCAN_PREFIX_SHIFT 0x800
#define	SCAN_PREFIX	(SCAN_PREFIX_E0  | SCAN_PREFIX_E1 | \
			 SCAN_PREFIX_CTL | SCAN_PREFIX_SHIFT)
#define	SCAN_CHAR(c)	((c) & 0x7f)

#define	HKBD_LOCK(sc)	HID_MTX_LOCK((sc)->sc_lock)
#define	HKBD_UNLOCK(sc)	HID_MTX_UNLOCK((sc)->sc_lock)
#define	HKBD_LOCK_ASSERT(sc)	HID_MTX_ASSERT((sc)->sc_lock, MA_OWNED)

struct hkbd_mods {
	uint32_t mask, key;
};

static const struct hkbd_mods hkbd_mods[HKBD_NMOD] = {
	{MOD_CONTROL_L, 0xe0},
	{MOD_CONTROL_R, 0xe4},
	{MOD_SHIFT_L, 0xe1},
	{MOD_SHIFT_R, 0xe5},
	{MOD_ALT_L, 0xe2},
	{MOD_ALT_R, 0xe6},
	{MOD_WIN_L, 0xe3},
	{MOD_WIN_R, 0xe7},
};

#define	NN 0				/* no translation */
/*
 * Translate USB keycodes to AT keyboard scancodes.
 */
/*
 * FIXME: Mac USB keyboard generates:
 * 0x53: keypad NumLock/Clear
 * 0x66: Power
 * 0x67: keypad =
 * 0x68: F13
 * 0x69: F14
 * 0x6a: F15
 * 
 * USB Apple Keyboard JIS generates:
 * 0x90: Kana
 * 0x91: Eisu
 */
static const uint8_t hkbd_trtab[256] = {
	0, 0, 0, 0, 30, 48, 46, 32,	/* 00 - 07 */
	18, 33, 34, 35, 23, 36, 37, 38,	/* 08 - 0F */
	50, 49, 24, 25, 16, 19, 31, 20,	/* 10 - 17 */
	22, 47, 17, 45, 21, 44, 2, 3,	/* 18 - 1F */
	4, 5, 6, 7, 8, 9, 10, 11,	/* 20 - 27 */
	28, 1, 14, 15, 57, 12, 13, 26,	/* 28 - 2F */
	27, 43, 43, 39, 40, 41, 51, 52,	/* 30 - 37 */
	53, 58, 59, 60, 61, 62, 63, 64,	/* 38 - 3F */
	65, 66, 67, 68, 87, 88, 92, 70,	/* 40 - 47 */
	104, 102, 94, 96, 103, 99, 101, 98,	/* 48 - 4F */
	97, 100, 95, 69, 91, 55, 74, 78,/* 50 - 57 */
	89, 79, 80, 81, 75, 76, 77, 71,	/* 58 - 5F */
	72, 73, 82, 83, 86, 107, 122, NN,	/* 60 - 67 */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* 68 - 6F */
	NN, NN, NN, NN, 115, 108, 111, 113,	/* 70 - 77 */
	109, 110, 112, 118, 114, 116, 117, 119,	/* 78 - 7F */
	121, 120, NN, NN, NN, NN, NN, 123,	/* 80 - 87 */
	124, 125, 126, 127, 128, NN, NN, NN,	/* 88 - 8F */
	129, 130, NN, NN, NN, NN, NN, NN,	/* 90 - 97 */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* 98 - 9F */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* A0 - A7 */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* A8 - AF */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* B0 - B7 */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* B8 - BF */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* C0 - C7 */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* C8 - CF */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* D0 - D7 */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* D8 - DF */
	29, 42, 56, 105, 90, 54, 93, 106,	/* E0 - E7 */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* E8 - EF */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* F0 - F7 */
	NN, NN, NN, NN, NN, NN, NN, NN,	/* F8 - FF */
};

#ifdef NOT_YET
static const uint8_t hkbd_boot_desc[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1,
	0x01, 0x05, 0x07, 0x19, 0xe0,
	0x29, 0xe7, 0x15, 0x00, 0x25,
	0x01, 0x75, 0x01, 0x95, 0x08,
	0x81, 0x02, 0x95, 0x01, 0x75,
	0x08, 0x81, 0x01, 0x95, 0x03,
	0x75, 0x01, 0x05, 0x08, 0x19,
	0x01, 0x29, 0x03, 0x91, 0x02,
	0x95, 0x05, 0x75, 0x01, 0x91,
	0x01, 0x95, 0x06, 0x75, 0x08,
	0x15, 0x00, 0x26, 0xff, 0x00,
	0x05, 0x07, 0x19, 0x00, 0x2a,
	0xff, 0x00, 0x81, 0x00, 0xc0
};
#endif

/* prototypes */
static void	hkbd_timeout(void *);
static int	hkbd_set_leds(struct hkbd_softc *, uint8_t);
static int	hkbd_set_typematic(keyboard_t *, int);
#ifdef HKBD_EMULATE_ATSCANCODE
static uint32_t	hkbd_atkeycode(int, int);
static int	hkbd_key2scan(struct hkbd_softc *, int, int, int);
#endif
static uint32_t	hkbd_read_char(keyboard_t *, int);
static void	hkbd_clear_state(keyboard_t *);
static int	hkbd_ioctl(keyboard_t *, u_long, caddr_t);
static int	hkbd_enable(keyboard_t *);
static int	hkbd_disable(keyboard_t *);
static void	hkbd_interrupt(struct hkbd_softc *);
static void	hkbd_event_keyinput(struct hkbd_softc *);

static device_probe_t hkbd_probe;
static device_attach_t hkbd_attach;
static device_detach_t hkbd_detach;
static device_resume_t hkbd_resume;

#ifdef EVDEV_SUPPORT
static evdev_event_t hkbd_ev_event;

static const struct evdev_methods hkbd_evdev_methods = {
	.ev_event = hkbd_ev_event,
};
#endif

static uint8_t
hkbd_any_key_pressed(struct hkbd_softc *sc)
{
	uint8_t i;
	uint8_t j;

	for (j = i = 0; i < HKBD_NKEYCODE; i++)
		j |= sc->sc_odata.keycode[i];

	return (j ? 1 : 0);
}

static void
hkbd_start_timer(struct hkbd_softc *sc)
{
	sbintime_t delay, now, prec;

	now = sbinuptime();

	/* check if initial delay passed and fallback to key repeat delay */
	if (sc->sc_delay == 0)
		sc->sc_delay = sc->sc_kbd.kb_delay2;

	/* compute timeout */
	delay = SBT_1MS * sc->sc_delay;
	sc->sc_co_basetime += delay;

	/* check if we are running behind */
	if (sc->sc_co_basetime < now)
		sc->sc_co_basetime = now;

	/* This is rarely called, so prefer precision to efficiency. */
	prec = qmin(delay >> 7, SBT_1MS * 10);
	if (!HID_IN_POLLING_MODE_FUNC())
		callout_reset_sbt(&sc->sc_callout, sc->sc_co_basetime, prec,
		    hkbd_timeout, sc, C_ABSOLUTE);
}

static void
hkbd_put_key(struct hkbd_softc *sc, uint32_t key)
{

	HKBD_LOCK_ASSERT(sc);

	DPRINTF("0x%02x (%d) %s\n", key, key,
	    (key & KEY_RELEASE) ? "released" : "pressed");

#ifdef EVDEV_SUPPORT
	if (evdev_rcpt_mask & EVDEV_RCPT_HW_KBD && sc->sc_evdev != NULL) {
		evdev_push_event(sc->sc_evdev, EV_KEY,
		    evdev_hid2key(KEY_INDEX(key)), !(key & KEY_RELEASE));
		evdev_sync(sc->sc_evdev);
	}
#endif

	if (sc->sc_inputs < HKBD_IN_BUF_SIZE) {
		sc->sc_input[sc->sc_inputtail] = key;
		++(sc->sc_inputs);
		++(sc->sc_inputtail);
		if (sc->sc_inputtail >= HKBD_IN_BUF_SIZE) {
			sc->sc_inputtail = 0;
		}
	} else {
		DPRINTF("input buffer is full\n");
	}
}

static void
hkbd_do_poll(struct hkbd_softc *sc, uint8_t wait)
{

	HKBD_LOCK_ASSERT(sc);
	KASSERT((sc->sc_flags & HKBD_FLAG_POLLING) != 0,
	    ("hkbd_do_poll called when not polling\n"));
	DPRINTFN(2, "polling\n");

	if (HID_IN_POLLING_MODE_FUNC() == 0) {
		/*
		 * In this context the kernel is polling for input,
		 * but the USB subsystem works in normal interrupt-driven
		 * mode, so we just wait on the USB threads to do the job.
		 * Note that we currently hold the Giant, but it's also used
		 * as the transfer mtx, so we must release it while waiting.
		 */
		while (sc->sc_inputs == 0) {
			/*
			 * Give USB threads a chance to run.  Note that
			 * kern_yield performs DROP_GIANT + PICKUP_GIANT.
			 */
			kern_yield(PRI_UNCHANGED);
			if (!wait)
				break;
		}
		return;
	}

	while (sc->sc_inputs == 0) {

#ifdef NOT_YET
		usbd_transfer_poll(sc->sc_xfer, HKBD_N_TRANSFER);
#endif

		/* Delay-optimised support for repetition of keys */
		if (hkbd_any_key_pressed(sc)) {
			/* a key is pressed - need timekeeping */
			DELAY(1000);

			/* 1 millisecond has passed */
			sc->sc_time_ms += 1;
		}

		hkbd_interrupt(sc);

		if (!wait)
			break;
	}
}

static int32_t
hkbd_get_key(struct hkbd_softc *sc, uint8_t wait)
{
	int32_t c;

	HKBD_LOCK_ASSERT(sc);
	KASSERT((HID_IN_POLLING_MODE_FUNC() == 0) ||
	    (sc->sc_flags & HKBD_FLAG_POLLING) != 0,
	    ("not polling in kdb or panic\n"));

	if (sc->sc_flags & HKBD_FLAG_POLLING)
		hkbd_do_poll(sc, wait);

	if (sc->sc_inputs == 0) {
		c = -1;
	} else {
		c = sc->sc_input[sc->sc_inputhead];
		--(sc->sc_inputs);
		++(sc->sc_inputhead);
		if (sc->sc_inputhead >= HKBD_IN_BUF_SIZE) {
			sc->sc_inputhead = 0;
		}
	}
	return (c);
}

static void
hkbd_interrupt(struct hkbd_softc *sc)
{
	uint32_t n_mod;
	uint32_t o_mod;
	uint32_t now = sc->sc_time_ms;
	int32_t dtime;
	uint8_t key;
	uint8_t i;
	uint8_t j;

	HKBD_LOCK_ASSERT(sc);

	if (sc->sc_ndata.keycode[0] == KEY_ERROR)
		return;

	n_mod = sc->sc_ndata.modifiers;
	o_mod = sc->sc_odata.modifiers;
	if (n_mod != o_mod) {
		for (i = 0; i < HKBD_NMOD; i++) {
			if ((n_mod & hkbd_mods[i].mask) !=
			    (o_mod & hkbd_mods[i].mask)) {
				hkbd_put_key(sc, hkbd_mods[i].key |
				    ((n_mod & hkbd_mods[i].mask) ?
				    KEY_PRESS : KEY_RELEASE));
			}
		}
	}
	/* Check for released keys. */
	for (i = 0; i < HKBD_NKEYCODE; i++) {
		key = sc->sc_odata.keycode[i];
		if (key == 0) {
			continue;
		}
		for (j = 0; j < HKBD_NKEYCODE; j++) {
			if (sc->sc_ndata.keycode[j] == 0) {
				continue;
			}
			if (key == sc->sc_ndata.keycode[j]) {
				goto rfound;
			}
		}
		hkbd_put_key(sc, key | KEY_RELEASE);
rfound:	;
	}

	/* Check for pressed keys. */
	for (i = 0; i < HKBD_NKEYCODE; i++) {
		key = sc->sc_ndata.keycode[i];
		if (key == 0) {
			continue;
		}
		sc->sc_ntime[i] = now + sc->sc_kbd.kb_delay1;
		for (j = 0; j < HKBD_NKEYCODE; j++) {
			if (sc->sc_odata.keycode[j] == 0) {
				continue;
			}
			if (key == sc->sc_odata.keycode[j]) {

				/* key is still pressed */

				sc->sc_ntime[i] = sc->sc_otime[j];
				dtime = (sc->sc_otime[j] - now);

				if (dtime > 0) {
					/* time has not elapsed */
					goto pfound;
				}
				sc->sc_ntime[i] = now + sc->sc_kbd.kb_delay2;
				break;
			}
		}
		if (j == HKBD_NKEYCODE) {
			/* New key - set initial delay and [re]start timer */
			sc->sc_co_basetime = sbinuptime();
			sc->sc_delay = sc->sc_kbd.kb_delay1;
			hkbd_start_timer(sc);
		}
		hkbd_put_key(sc, key | KEY_PRESS);

		/*
                 * If any other key is presently down, force its repeat to be
                 * well in the future (100s).  This makes the last key to be
                 * pressed do the autorepeat.
                 */
		for (j = 0; j != HKBD_NKEYCODE; j++) {
			if (j != i)
				sc->sc_ntime[j] = now + (100 * 1000);
		}
pfound:	;
	}

	sc->sc_odata = sc->sc_ndata;

	memcpy(sc->sc_otime, sc->sc_ntime, sizeof(sc->sc_otime));

	hkbd_event_keyinput(sc);
}

static void
hkbd_event_keyinput(struct hkbd_softc *sc)
{
	int c;

	HKBD_LOCK_ASSERT(sc);

	if ((sc->sc_flags & HKBD_FLAG_POLLING) != 0)
		return;

	if (sc->sc_inputs == 0)
		return;

	if (KBD_IS_ACTIVE(&sc->sc_kbd) &&
	    KBD_IS_BUSY(&sc->sc_kbd)) {
		/* let the callback function process the input */
		(sc->sc_kbd.kb_callback.kc_func) (&sc->sc_kbd, KBDIO_KEYINPUT,
		    sc->sc_kbd.kb_callback.kc_arg);
	} else {
		/* read and discard the input, no one is waiting for it */
		do {
			c = hkbd_read_char(&sc->sc_kbd, 0);
		} while (c != NOKEY);
	}
}

static void
hkbd_timeout(void *arg)
{
	struct hkbd_softc *sc = arg;

	HKBD_LOCK_ASSERT(sc);

	sc->sc_time_ms += sc->sc_delay;
	sc->sc_delay = 0;

	hkbd_interrupt(sc);

	/* Make sure any leftover key events gets read out */
	hkbd_event_keyinput(sc);

	if (hkbd_any_key_pressed(sc) || (sc->sc_inputs != 0)) {
		hkbd_start_timer(sc);
	}
}

static uint8_t
hkbd_apple_fn(uint8_t keycode) {
	switch (keycode) {
	case 0x28: return 0x49; /* RETURN -> INSERT */
	case 0x2a: return 0x4c; /* BACKSPACE -> DEL */
	case 0x50: return 0x4a; /* LEFT ARROW -> HOME */
	case 0x4f: return 0x4d; /* RIGHT ARROW -> END */
	case 0x52: return 0x4b; /* UP ARROW -> PGUP */
	case 0x51: return 0x4e; /* DOWN ARROW -> PGDN */
	default: return keycode;
	}
}

static uint8_t
hkbd_apple_swap(uint8_t keycode) {
	switch (keycode) {
	case 0x35: return 0x64;
	case 0x64: return 0x35;
	default: return keycode;
	}
}

static void
hkbd_intr_callback(void *context, void *data, uint16_t len)
{
	device_t dev = context;
	struct hkbd_softc *sc = device_get_softc(dev);
	uint8_t *buf = data;
	uint8_t i;
	uint8_t id = 0;

	HKBD_LOCK_ASSERT(sc);

	DPRINTF("actlen=%d bytes\n", len);

	if (len == 0) {
		DPRINTF("zero length data\n");
		return;
	}

	if (sc->sc_kbd_id != 0) {
		/* check and remove HID ID byte */
		id = buf[0];
		buf++;
		len--;
		if (len == 0) {
			DPRINTF("zero length data\n");
			return;
		}
	}

	/* clear temporary storage */
	memset(&sc->sc_ndata, 0, sizeof(sc->sc_ndata));

	/* scan through HID data */
	if ((sc->sc_flags & HKBD_FLAG_APPLE_EJECT) &&
	    (id == sc->sc_id_apple_eject)) {
		if (hid_get_data(buf, len, &sc->sc_loc_apple_eject))
			sc->sc_modifiers |= MOD_EJECT;
		else
			sc->sc_modifiers &= ~MOD_EJECT;
	}
	if ((sc->sc_flags & HKBD_FLAG_APPLE_FN) &&
	    (id == sc->sc_id_apple_fn)) {
		if (hid_get_data(buf, len, &sc->sc_loc_apple_fn))
			sc->sc_modifiers |= MOD_FN;
		else
			sc->sc_modifiers &= ~MOD_FN;
	}
	if ((sc->sc_flags & HKBD_FLAG_CTRL_L) && (id == sc->sc_id_ctrl_l)) {
		if (hid_get_data(buf, len, &sc->sc_loc_ctrl_l))
		  sc->	sc_modifiers |= MOD_CONTROL_L;
		else
		  sc->	sc_modifiers &= ~MOD_CONTROL_L;
	}
	if ((sc->sc_flags & HKBD_FLAG_CTRL_R) && (id == sc->sc_id_ctrl_r)) {
		if (hid_get_data(buf, len, &sc->sc_loc_ctrl_r))
			sc->sc_modifiers |= MOD_CONTROL_R;
		else
			sc->sc_modifiers &= ~MOD_CONTROL_R;
	}
	if ((sc->sc_flags & HKBD_FLAG_SHIFT_L) && (id == sc->sc_id_shift_l)) {
		if (hid_get_data(buf, len, &sc->sc_loc_shift_l))
			sc->sc_modifiers |= MOD_SHIFT_L;
		else
			sc->sc_modifiers &= ~MOD_SHIFT_L;
	}
	if ((sc->sc_flags & HKBD_FLAG_SHIFT_R) && (id == sc->sc_id_shift_r)) {
		if (hid_get_data(buf, len, &sc->sc_loc_shift_r))
			sc->sc_modifiers |= MOD_SHIFT_R;
		else
			sc->sc_modifiers &= ~MOD_SHIFT_R;
	}
	if ((sc->sc_flags & HKBD_FLAG_ALT_L) && (id == sc->sc_id_alt_l)) {
		if (hid_get_data(buf, len, &sc->sc_loc_alt_l))
			sc->sc_modifiers |= MOD_ALT_L;
		else
			sc->sc_modifiers &= ~MOD_ALT_L;
	}
	if ((sc->sc_flags & HKBD_FLAG_ALT_R) && (id == sc->sc_id_alt_r)) {
		if (hid_get_data(buf, len, &sc->sc_loc_alt_r))
			sc->sc_modifiers |= MOD_ALT_R;
		else
			sc->sc_modifiers &= ~MOD_ALT_R;
	}
	if ((sc->sc_flags & HKBD_FLAG_WIN_L) && (id == sc->sc_id_win_l)) {
		if (hid_get_data(buf, len, &sc->sc_loc_win_l))
			sc->sc_modifiers |= MOD_WIN_L;
		else
			sc->sc_modifiers &= ~MOD_WIN_L;
	}
	if ((sc->sc_flags & HKBD_FLAG_WIN_R) && (id == sc->sc_id_win_r)) {
		if (hid_get_data(buf, len, &sc->sc_loc_win_r))
			sc->sc_modifiers |= MOD_WIN_R;
		else
			sc->sc_modifiers &= ~MOD_WIN_R;
	}

	sc->sc_ndata.modifiers = sc->sc_modifiers;

	if ((sc->sc_flags & HKBD_FLAG_EVENTS) && (id == sc->sc_id_events)) {
		i = sc->sc_loc_events.count;
		if (i > HKBD_NKEYCODE)
			i = HKBD_NKEYCODE;
		if (i > len)
			i = len;
		while (i--) {
			sc->sc_ndata.keycode[i] =
			    hid_get_data(buf + i, len - i,
			    &sc->sc_loc_events);
		}
	}

#ifdef HID_DEBUG
	DPRINTF("modifiers = 0x%04x\n", (int)sc->sc_modifiers);
	for (i = 0; i < HKBD_NKEYCODE; i++) {
		if (sc->sc_ndata.keycode[i]) {
			DPRINTF("[%d] = 0x%02x\n",
			    (int)i, (int)sc->sc_ndata.keycode[i]);
		}
	}
#endif
	if (sc->sc_modifiers & MOD_FN) {
		for (i = 0; i < HKBD_NKEYCODE; i++) {
			sc->sc_ndata.keycode[i] = 
			    hkbd_apple_fn(sc->sc_ndata.keycode[i]);
		}
	}

	if (sc->sc_flags & HKBD_FLAG_APPLE_SWAP) {
		for (i = 0; i < HKBD_NKEYCODE; i++) {
			sc->sc_ndata.keycode[i] = 
			    hkbd_apple_swap(sc->sc_ndata.keycode[i]);
		}
	}

	hkbd_interrupt(sc);
}

/* A match on these entries will load ukbd */
static const struct hid_device_id __used hkbd_devs[] = {
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_KEYBOARD) },
};

static int
hkbd_probe(device_t dev)
{
	keyboard_switch_t *sw = kbd_get_switch(HKBD_DRIVER_NAME);
	int error;

	DPRINTFN(11, "\n");

	if (sw == NULL) {
		return (ENXIO);
	}

	error = hidbus_lookup_driver_info(dev, hkbd_devs, sizeof(hkbd_devs));
	if (error != 0)
                return (error);

#ifdef NOT_YET
	if (usb_test_quirk(uaa, UQ_KBD_IGNORE))
		return (ENXIO);
#endif

	return (BUS_PROBE_DEFAULT);
}

static void
hkbd_parse_hid(struct hkbd_softc *sc, const uint8_t *ptr, uint32_t len,
    uint8_t tlc_index)
{
	uint32_t flags;

	/* reset detected bits */
	sc->sc_flags &= ~HKBD_FLAG_HID_MASK;

	/* check if there is an ID byte */
	sc->sc_kbd_size = hid_report_size(ptr, len,
	    hid_input, &sc->sc_kbd_id);

	/* investigate if this is an Apple Keyboard */
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_CONSUMER, HUG_APPLE_EJECT),
	    hid_input, tlc_index, 0, &sc->sc_loc_apple_eject, &flags,
	    &sc->sc_id_apple_eject, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_APPLE_EJECT | 
			    HKBD_FLAG_APPLE_SWAP;
		DPRINTFN(1, "Found Apple eject-key\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(0xFFFF, 0x0003),
	    hid_input, tlc_index, 0, &sc->sc_loc_apple_fn, &flags,
	    &sc->sc_id_apple_fn, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_APPLE_FN;
		DPRINTFN(1, "Found Apple FN-key\n");
	}
	/* figure out some keys */
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_KEYBOARD, 0xE0),
	    hid_input, tlc_index, 0, &sc->sc_loc_ctrl_l, &flags,
	    &sc->sc_id_ctrl_l, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_CTRL_L;
		DPRINTFN(1, "Found left control\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_KEYBOARD, 0xE4),
	    hid_input, tlc_index, 0, &sc->sc_loc_ctrl_r, &flags,
	    &sc->sc_id_ctrl_r, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_CTRL_R;
		DPRINTFN(1, "Found right control\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_KEYBOARD, 0xE1),
	    hid_input, tlc_index, 0, &sc->sc_loc_shift_l, &flags,
	    &sc->sc_id_shift_l, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_SHIFT_L;
		DPRINTFN(1, "Found left shift\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_KEYBOARD, 0xE5),
	    hid_input, tlc_index, 0, &sc->sc_loc_shift_r, &flags,
	    &sc->sc_id_shift_r, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_SHIFT_R;
		DPRINTFN(1, "Found right shift\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_KEYBOARD, 0xE2),
	    hid_input, tlc_index, 0, &sc->sc_loc_alt_l, &flags,
	    &sc->sc_id_alt_l, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_ALT_L;
		DPRINTFN(1, "Found left alt\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_KEYBOARD, 0xE6),
	    hid_input, tlc_index, 0, &sc->sc_loc_alt_r, &flags,
	    &sc->sc_id_alt_r, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_ALT_R;
		DPRINTFN(1, "Found right alt\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_KEYBOARD, 0xE3),
	    hid_input, tlc_index, 0, &sc->sc_loc_win_l, &flags,
	    &sc->sc_id_win_l, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_WIN_L;
		DPRINTFN(1, "Found left GUI\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_KEYBOARD, 0xE7),
	    hid_input, tlc_index, 0, &sc->sc_loc_win_r, &flags,
	    &sc->sc_id_win_r, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_WIN_R;
		DPRINTFN(1, "Found right GUI\n");
	}
	/* figure out event buffer */
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_KEYBOARD, 0x00),
	    hid_input, tlc_index, 0, &sc->sc_loc_events, &flags,
	    &sc->sc_id_events, NULL)) {
		if (flags & HIO_VARIABLE) {
			DPRINTFN(1, "Ignoring keyboard event control\n");
		} else {
			sc->sc_flags |= HKBD_FLAG_EVENTS;
			DPRINTFN(1, "Found keyboard event array\n");
		}
	}

	/* figure out leds on keyboard */
	sc->sc_led_size = hid_report_size(ptr, len,
	    hid_output, NULL);

	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_LEDS, 0x01),
	    hid_output, tlc_index, 0, &sc->sc_loc_numlock, &flags,
	    &sc->sc_id_numlock, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_NUMLOCK;
		DPRINTFN(1, "Found keyboard numlock\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_LEDS, 0x02),
	    hid_output, tlc_index, 0, &sc->sc_loc_capslock, &flags,
	    &sc->sc_id_capslock, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_CAPSLOCK;
		DPRINTFN(1, "Found keyboard capslock\n");
	}
	if (hid_tlc_locate(ptr, len,
	    HID_USAGE2(HUP_LEDS, 0x03),
	    hid_output, tlc_index, 0, &sc->sc_loc_scrolllock, &flags,
	    &sc->sc_id_scrolllock, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_flags |= HKBD_FLAG_SCROLLLOCK;
		DPRINTFN(1, "Found keyboard scrolllock\n");
	}
}

static int
hkbd_attach(device_t dev)
{
	struct hkbd_softc *sc = device_get_softc(dev);
	const struct hid_device_info *hw = hid_get_device_info(dev);
	int unit = device_get_unit(dev);
	keyboard_t *kbd = &sc->sc_kbd;
	void *hid_ptr = NULL;
	usb_error_t err;
	uint16_t n;
	uint16_t hid_len;
	uint8_t tlc_index = hidbus_get_index(dev);
#ifdef EVDEV_SUPPORT
	struct evdev_dev *evdev;
	int i;
#endif

	sc->sc_dev = dev;
	sc->sc_lock = hidbus_get_lock(dev);
	HKBD_LOCK_ASSERT(sc);

	kbd_init_struct(kbd, HKBD_DRIVER_NAME, KB_OTHER, unit, 0, 0, 0);

	kbd->kb_data = (void *)sc;

	device_set_desc(dev, hw->name);

	sc->sc_mode = K_XLATE;

	callout_init_mtx(&sc->sc_callout, sc->sc_lock, 0);

	hidbus_set_intr(dev, hkbd_intr_callback);

	/* setup default keyboard maps */

	sc->sc_keymap = key_map;
	sc->sc_accmap = accent_map;
	for (n = 0; n < HKBD_NFKEY; n++) {
		sc->sc_fkeymap[n] = fkey_tab[n];
	}

	kbd_set_maps(kbd, &sc->sc_keymap, &sc->sc_accmap,
	    sc->sc_fkeymap, HKBD_NFKEY);

	KBD_FOUND_DEVICE(kbd);

	hkbd_clear_state(kbd);

	/*
	 * FIXME: set the initial value for lock keys in "sc_state"
	 * according to the BIOS data?
	 */
	KBD_PROBE_DONE(kbd);

	/* get HID descriptor */
	err = hid_get_report_descr(dev, &hid_ptr, &hid_len);

	if (err == 0) {
		DPRINTF("Parsing HID descriptor of %d bytes\n",
		    (int)hid_len);

		hkbd_parse_hid(sc, hid_ptr, hid_len, tlc_index);

		free(hid_ptr, M_TEMP);
	}

#ifdef NOT_YET
	/* check if we should use the boot protocol */
	if (usb_test_quirk(uaa, UQ_KBD_BOOTPROTO) ||
	    (err != 0) || (!(sc->sc_flags & HKBD_FLAG_EVENTS))) {

		DPRINTF("Forcing boot protocol\n");

		err = hid_set_protocol(dev, 0);

		if (err != 0) {
			DPRINTF("Set protocol error=%s (ignored)\n",
			    usbd_errstr(err));
		}

		hkbd_parse_hid(sc, hkbd_boot_desc, sizeof(hkbd_boot_desc));
	}
#endif

	/* ignore if SETIDLE fails, hence it is not crucial */
	hid_set_idle(dev, 0, 0);

	hkbd_ioctl(kbd, KDSETLED, (caddr_t)&sc->sc_state);

	KBD_INIT_DONE(kbd);

	if (kbd_register(kbd) < 0) {
		goto detach;
	}
	KBD_CONFIG_DONE(kbd);

	hkbd_enable(kbd);

#ifdef KBD_INSTALL_CDEV
	if (kbd_attach(kbd)) {
		goto detach;
	}
#endif

#ifdef EVDEV_SUPPORT
	evdev = evdev_alloc();
	evdev_set_name(evdev, device_get_desc(dev));
	evdev_set_phys(evdev, device_get_nameunit(dev));
	evdev_set_id(evdev, hw->idBus, hw->idVendor, hw->idProduct,
	    hw->idVersion);
	evdev_set_serial(evdev, hw->serial);
	evdev_set_methods(evdev, kbd, &hkbd_evdev_methods);
	evdev_support_event(evdev, EV_SYN);
	evdev_support_event(evdev, EV_KEY);
	if (sc->sc_flags & (HKBD_FLAG_NUMLOCK | HKBD_FLAG_CAPSLOCK |
			    HKBD_FLAG_SCROLLLOCK))
		evdev_support_event(evdev, EV_LED);
	evdev_support_event(evdev, EV_REP);

	for (i = 0x00; i <= 0xFF; i++)
		evdev_support_key(evdev, evdev_hid2key(i));
	if (sc->sc_flags & HKBD_FLAG_NUMLOCK)
		evdev_support_led(evdev, LED_NUML);
	if (sc->sc_flags & HKBD_FLAG_CAPSLOCK)
		evdev_support_led(evdev, LED_CAPSL);
	if (sc->sc_flags & HKBD_FLAG_SCROLLLOCK)
		evdev_support_led(evdev, LED_SCROLLL);

	if (evdev_register_mtx(evdev, sc->sc_lock))
		evdev_free(evdev);
	else
		sc->sc_evdev = evdev;
#endif

	sc->sc_flags |= HKBD_FLAG_ATTACHED;

	if (bootverbose) {
		genkbd_diag(kbd, bootverbose);
	}

	/* start the keyboard */
	hidbus_set_xfer(dev, HID_XFER_READ);

	return (0);			/* success */

detach:
	hkbd_detach(dev);
	return (ENXIO);			/* error */
}

static int
hkbd_detach(device_t dev)
{
	struct hkbd_softc *sc = device_get_softc(dev);
	int error;

	HKBD_LOCK_ASSERT(sc);

	DPRINTF("\n");

	sc->sc_flags |= HKBD_FLAG_GONE;

	callout_stop(&sc->sc_callout);

	/* kill any stuck keys */
	if (sc->sc_flags & HKBD_FLAG_ATTACHED) {
		/* stop receiving events from the USB keyboard */
		hidbus_set_xfer(dev, 0);

		/* release all leftover keys, if any */
		memset(&sc->sc_ndata, 0, sizeof(sc->sc_ndata));

		/* process releasing of all keys */
		hkbd_interrupt(sc);
	}

	hkbd_disable(&sc->sc_kbd);

#ifdef KBD_INSTALL_CDEV
	if (sc->sc_flags & HKBD_FLAG_ATTACHED) {
		error = kbd_detach(&sc->sc_kbd);
		if (error) {
			/* usb attach cannot return an error */
			device_printf(dev, "WARNING: kbd_detach() "
			    "returned non-zero! (ignored)\n");
		}
	}
#endif

#ifdef EVDEV_SUPPORT
	evdev_free(sc->sc_evdev);
#endif

	if (KBD_IS_CONFIGURED(&sc->sc_kbd)) {
		error = kbd_unregister(&sc->sc_kbd);
		if (error) {
			/* usb attach cannot return an error */
			device_printf(dev, "WARNING: kbd_unregister() "
			    "returned non-zero! (ignored)\n");
		}
	}
	sc->sc_kbd.kb_flags = 0;

	DPRINTF("%s: disconnected\n",
	    device_get_nameunit(dev));

	return (0);
}

static int
hkbd_resume(device_t dev)
{
	struct hkbd_softc *sc = device_get_softc(dev);

	HKBD_LOCK_ASSERT();

	hkbd_clear_state(&sc->sc_kbd);

	return (0);
}

#ifdef EVDEV_SUPPORT
static void
hkbd_ev_event(struct evdev_dev *evdev, uint16_t type, uint16_t code,
    int32_t value)
{
	keyboard_t *kbd = evdev_get_softc(evdev);
	struct hkbd_softc *sc = kbd->kb_data;

	if (evdev_rcpt_mask & EVDEV_RCPT_HW_KBD &&
	    (type == EV_LED || type == EV_REP)) {
		HKBD_LOCK(sc);
		kbd_ev_event(kbd, type, code, value);
		HKBD_UNLOCK(sc);
	}
}
#endif

/* early keyboard probe, not supported */
static int
hkbd_configure(int flags)
{
	return (0);
}

/* detect a keyboard, not used */
static int
hkbd__probe(int unit, void *arg, int flags)
{
	return (ENXIO);
}

/* reset and initialize the device, not used */
static int
hkbd_init(int unit, keyboard_t **kbdp, void *arg, int flags)
{
	return (ENXIO);
}

/* test the interface to the device, not used */
static int
hkbd_test_if(keyboard_t *kbd)
{
	return (0);
}

/* finish using this keyboard, not used */
static int
hkbd_term(keyboard_t *kbd)
{
	return (ENXIO);
}

/* keyboard interrupt routine, not used */
static int
hkbd_intr(keyboard_t *kbd, void *arg)
{
	return (0);
}

/* lock the access to the keyboard, not used */
static int
hkbd_lock(keyboard_t *kbd, int lock)
{
	return (1);
}

/*
 * Enable the access to the device; until this function is called,
 * the client cannot read from the keyboard.
 */
static int
hkbd_enable(keyboard_t *kbd)
{
	struct hkbd_softc *sc = kbd->kb_data;

	HKBD_LOCK(sc);
	KBD_ACTIVATE(kbd);
	HKBD_UNLOCK(sc);

	return (0);
}

/* disallow the access to the device */
static int
hkbd_disable(keyboard_t *kbd)
{
	struct hkbd_softc *sc = kbd->kb_data;

	HKBD_LOCK(sc);
	KBD_DEACTIVATE(kbd);
	HKBD_UNLOCK(sc);

	return (0);
}

/* check if data is waiting */
/* Currently unused. */
static int
hkbd_check(keyboard_t *kbd)
{
	struct hkbd_softc *sc = kbd->kb_data;

	HKBD_LOCK_ASSERT();

	if (!KBD_IS_ACTIVE(kbd))
		return (0);

	if (sc->sc_flags & HKBD_FLAG_POLLING)
		hkbd_do_poll(sc, 0);

#ifdef HKBD_EMULATE_ATSCANCODE
	if (sc->sc_buffered_char[0]) {
		return (1);
	}
#endif
	if (sc->sc_inputs > 0) {
		return (1);
	}
	return (0);
}

/* check if char is waiting */
static int
hkbd_check_char_locked(keyboard_t *kbd)
{
	struct hkbd_softc *sc = kbd->kb_data;

	HKBD_LOCK_ASSERT(sc);

	if (!KBD_IS_ACTIVE(kbd))
		return (0);

	if ((sc->sc_composed_char > 0) &&
	    (!(sc->sc_flags & HKBD_FLAG_COMPOSE))) {
		return (1);
	}
	return (hkbd_check(kbd));
}

static int
hkbd_check_char(keyboard_t *kbd)
{
	struct hkbd_softc *sc = kbd->kb_data;
	int result;

	HKBD_LOCK(sc);
	result = hkbd_check_char_locked(kbd);
	HKBD_UNLOCK(sc);

	return (result);
}

/* read one byte from the keyboard if it's allowed */
/* Currently unused. */
static int
hkbd_read(keyboard_t *kbd, int wait)
{
	struct hkbd_softc *sc = kbd->kb_data;
	int32_t usbcode;
#ifdef HKBD_EMULATE_ATSCANCODE
	uint32_t keycode;
	uint32_t scancode;

#endif

	HKBD_LOCK_ASSERT(sc);

	if (!KBD_IS_ACTIVE(kbd))
		return (-1);

#ifdef HKBD_EMULATE_ATSCANCODE
	if (sc->sc_buffered_char[0]) {
		scancode = sc->sc_buffered_char[0];
		if (scancode & SCAN_PREFIX) {
			sc->sc_buffered_char[0] &= ~SCAN_PREFIX;
			return ((scancode & SCAN_PREFIX_E0) ? 0xe0 : 0xe1);
		}
		sc->sc_buffered_char[0] = sc->sc_buffered_char[1];
		sc->sc_buffered_char[1] = 0;
		return (scancode);
	}
#endif					/* HKBD_EMULATE_ATSCANCODE */

	/* XXX */
	usbcode = hkbd_get_key(sc, (wait == FALSE) ? 0 : 1);
	if (!KBD_IS_ACTIVE(kbd) || (usbcode == -1))
		return (-1);

	++(kbd->kb_count);

#ifdef HKBD_EMULATE_ATSCANCODE
	keycode = hkbd_atkeycode(usbcode, sc->sc_ndata.modifiers);
	if (keycode == NN) {
		return -1;
	}
	return (hkbd_key2scan(sc, keycode, sc->sc_ndata.modifiers,
	    (usbcode & KEY_RELEASE)));
#else					/* !HKBD_EMULATE_ATSCANCODE */
	return (usbcode);
#endif					/* HKBD_EMULATE_ATSCANCODE */
}

/* read char from the keyboard */
static uint32_t
hkbd_read_char_locked(keyboard_t *kbd, int wait)
{
	struct hkbd_softc *sc = kbd->kb_data;
	uint32_t action;
	uint32_t keycode;
	int32_t usbcode;
#ifdef HKBD_EMULATE_ATSCANCODE
	uint32_t scancode;
#endif

	HKBD_LOCK_ASSERT(sc);

	if (!KBD_IS_ACTIVE(kbd))
		return (NOKEY);

next_code:

	/* do we have a composed char to return ? */

	if ((sc->sc_composed_char > 0) &&
	    (!(sc->sc_flags & HKBD_FLAG_COMPOSE))) {

		action = sc->sc_composed_char;
		sc->sc_composed_char = 0;

		if (action > 0xFF) {
			goto errkey;
		}
		goto done;
	}
#ifdef HKBD_EMULATE_ATSCANCODE

	/* do we have a pending raw scan code? */

	if (sc->sc_mode == K_RAW) {
		scancode = sc->sc_buffered_char[0];
		if (scancode) {
			if (scancode & SCAN_PREFIX) {
				sc->sc_buffered_char[0] = (scancode & ~SCAN_PREFIX);
				return ((scancode & SCAN_PREFIX_E0) ? 0xe0 : 0xe1);
			}
			sc->sc_buffered_char[0] = sc->sc_buffered_char[1];
			sc->sc_buffered_char[1] = 0;
			return (scancode);
		}
	}
#endif					/* HKBD_EMULATE_ATSCANCODE */

	/* see if there is something in the keyboard port */
	/* XXX */
	usbcode = hkbd_get_key(sc, (wait == FALSE) ? 0 : 1);
	if (usbcode == -1) {
		return (NOKEY);
	}
	++kbd->kb_count;

#ifdef HKBD_EMULATE_ATSCANCODE
	/* USB key index -> key code -> AT scan code */
	keycode = hkbd_atkeycode(usbcode, sc->sc_ndata.modifiers);
	if (keycode == NN) {
		return (NOKEY);
	}
	/* return an AT scan code for the K_RAW mode */
	if (sc->sc_mode == K_RAW) {
		return (hkbd_key2scan(sc, keycode, sc->sc_ndata.modifiers,
		    (usbcode & KEY_RELEASE)));
	}
#else					/* !HKBD_EMULATE_ATSCANCODE */

	/* return the byte as is for the K_RAW mode */
	if (sc->sc_mode == K_RAW) {
		return (usbcode);
	}
	/* USB key index -> key code */
	keycode = hkbd_trtab[KEY_INDEX(usbcode)];
	if (keycode == NN) {
		return (NOKEY);
	}
#endif					/* HKBD_EMULATE_ATSCANCODE */

	switch (keycode) {
	case 0x38:			/* left alt (compose key) */
		if (usbcode & KEY_RELEASE) {
			if (sc->sc_flags & HKBD_FLAG_COMPOSE) {
				sc->sc_flags &= ~HKBD_FLAG_COMPOSE;

				if (sc->sc_composed_char > 0xFF) {
					sc->sc_composed_char = 0;
				}
			}
		} else {
			if (!(sc->sc_flags & HKBD_FLAG_COMPOSE)) {
				sc->sc_flags |= HKBD_FLAG_COMPOSE;
				sc->sc_composed_char = 0;
			}
		}
		break;
	}

	/* return the key code in the K_CODE mode */
	if (usbcode & KEY_RELEASE) {
		keycode |= SCAN_RELEASE;
	}
	if (sc->sc_mode == K_CODE) {
		return (keycode);
	}
	/* compose a character code */
	if (sc->sc_flags & HKBD_FLAG_COMPOSE) {
		switch (keycode) {
			/* key pressed, process it */
		case 0x47:
		case 0x48:
		case 0x49:		/* keypad 7,8,9 */
			sc->sc_composed_char *= 10;
			sc->sc_composed_char += keycode - 0x40;
			goto check_composed;

		case 0x4B:
		case 0x4C:
		case 0x4D:		/* keypad 4,5,6 */
			sc->sc_composed_char *= 10;
			sc->sc_composed_char += keycode - 0x47;
			goto check_composed;

		case 0x4F:
		case 0x50:
		case 0x51:		/* keypad 1,2,3 */
			sc->sc_composed_char *= 10;
			sc->sc_composed_char += keycode - 0x4E;
			goto check_composed;

		case 0x52:		/* keypad 0 */
			sc->sc_composed_char *= 10;
			goto check_composed;

			/* key released, no interest here */
		case SCAN_RELEASE | 0x47:
		case SCAN_RELEASE | 0x48:
		case SCAN_RELEASE | 0x49:	/* keypad 7,8,9 */
		case SCAN_RELEASE | 0x4B:
		case SCAN_RELEASE | 0x4C:
		case SCAN_RELEASE | 0x4D:	/* keypad 4,5,6 */
		case SCAN_RELEASE | 0x4F:
		case SCAN_RELEASE | 0x50:
		case SCAN_RELEASE | 0x51:	/* keypad 1,2,3 */
		case SCAN_RELEASE | 0x52:	/* keypad 0 */
			goto next_code;

		case 0x38:		/* left alt key */
			break;

		default:
			if (sc->sc_composed_char > 0) {
				sc->sc_flags &= ~HKBD_FLAG_COMPOSE;
				sc->sc_composed_char = 0;
				goto errkey;
			}
			break;
		}
	}
	/* keycode to key action */
	action = genkbd_keyaction(kbd, SCAN_CHAR(keycode),
	    (keycode & SCAN_RELEASE),
	    &sc->sc_state, &sc->sc_accents);
	if (action == NOKEY) {
		goto next_code;
	}
done:
	return (action);

check_composed:
	if (sc->sc_composed_char <= 0xFF) {
		goto next_code;
	}
errkey:
	return (ERRKEY);
}

/* Currently wait is always false. */
static uint32_t
hkbd_read_char(keyboard_t *kbd, int wait)
{
	struct hkbd_softc *sc = kbd->kb_data;
	uint32_t keycode;

	HKBD_LOCK(sc);
	keycode = hkbd_read_char_locked(kbd, wait);
	HKBD_UNLOCK(sc);

	return (keycode);
}

/* some useful control functions */
static int
hkbd_ioctl_locked(keyboard_t *kbd, u_long cmd, caddr_t arg)
{
	struct hkbd_softc *sc = kbd->kb_data;
	int i;
#if defined(COMPAT_FREEBSD6) || defined(COMPAT_FREEBSD5) || \
    defined(COMPAT_FREEBSD4) || defined(COMPAT_43)
	int ival;

#endif

	HKBD_LOCK_ASSERT(sc);

	switch (cmd) {
	case KDGKBMODE:		/* get keyboard mode */
		*(int *)arg = sc->sc_mode;
		break;
#if defined(COMPAT_FREEBSD6) || defined(COMPAT_FREEBSD5) || \
    defined(COMPAT_FREEBSD4) || defined(COMPAT_43)
	case _IO('K', 7):
		ival = IOCPARM_IVAL(arg);
		arg = (caddr_t)&ival;
		/* FALLTHROUGH */
#endif
	case KDSKBMODE:		/* set keyboard mode */
		switch (*(int *)arg) {
		case K_XLATE:
			if (sc->sc_mode != K_XLATE) {
				/* make lock key state and LED state match */
				sc->sc_state &= ~LOCK_MASK;
				sc->sc_state |= KBD_LED_VAL(kbd);
			}
			/* FALLTHROUGH */
		case K_RAW:
		case K_CODE:
			if (sc->sc_mode != *(int *)arg) {
				if ((sc->sc_flags & HKBD_FLAG_POLLING) == 0)
					hkbd_clear_state(kbd);
				sc->sc_mode = *(int *)arg;
			}
			break;
		default:
			return (EINVAL);
		}
		break;

	case KDGETLED:			/* get keyboard LED */
		*(int *)arg = KBD_LED_VAL(kbd);
		break;
#if defined(COMPAT_FREEBSD6) || defined(COMPAT_FREEBSD5) || \
    defined(COMPAT_FREEBSD4) || defined(COMPAT_43)
	case _IO('K', 66):
		ival = IOCPARM_IVAL(arg);
		arg = (caddr_t)&ival;
		/* FALLTHROUGH */
#endif
	case KDSETLED:			/* set keyboard LED */
		/* NOTE: lock key state in "sc_state" won't be changed */
		if (*(int *)arg & ~LOCK_MASK)
			return (EINVAL);

		i = *(int *)arg;

		/* replace CAPS LED with ALTGR LED for ALTGR keyboards */
		if (sc->sc_mode == K_XLATE &&
		    kbd->kb_keymap->n_keys > ALTGR_OFFSET) {
			if (i & ALKED)
				i |= CLKED;
			else
				i &= ~CLKED;
		}
		KBD_LED_VAL(kbd) = *(int *)arg;
		if (KBD_HAS_DEVICE(kbd))
			return (hkbd_set_leds(sc, i));
		break;

	case KDGKBSTATE:		/* get lock key state */
		*(int *)arg = sc->sc_state & LOCK_MASK;
		break;
#if defined(COMPAT_FREEBSD6) || defined(COMPAT_FREEBSD5) || \
    defined(COMPAT_FREEBSD4) || defined(COMPAT_43)
	case _IO('K', 20):
		ival = IOCPARM_IVAL(arg);
		arg = (caddr_t)&ival;
		/* FALLTHROUGH */
#endif
	case KDSKBSTATE:		/* set lock key state */
		if (*(int *)arg & ~LOCK_MASK) {
			return (EINVAL);
		}
		sc->sc_state &= ~LOCK_MASK;
		sc->sc_state |= *(int *)arg;

		/* set LEDs and quit */
		return (hkbd_ioctl(kbd, KDSETLED, arg));

	case KDSETREPEAT:		/* set keyboard repeat rate (new
					 * interface) */
		if (!KBD_HAS_DEVICE(kbd)) {
			return (0);
		}
		/*
		 * Convert negative, zero and tiny args to the same limits
		 * as atkbd.  We could support delays of 1 msec, but
		 * anything much shorter than the shortest atkbd value
		 * of 250.34 is almost unusable as well as incompatible.
		 */
		kbd->kb_delay1 = imax(((int *)arg)[0], 250);
		kbd->kb_delay2 = imax(((int *)arg)[1], 34);
#ifdef EVDEV_SUPPORT
		if (sc->sc_evdev != NULL)
			evdev_push_repeats(sc->sc_evdev, kbd);
#endif
		return (0);

#if defined(COMPAT_FREEBSD6) || defined(COMPAT_FREEBSD5) || \
    defined(COMPAT_FREEBSD4) || defined(COMPAT_43)
	case _IO('K', 67):
		ival = IOCPARM_IVAL(arg);
		arg = (caddr_t)&ival;
		/* FALLTHROUGH */
#endif
	case KDSETRAD:			/* set keyboard repeat rate (old
					 * interface) */
		return (hkbd_set_typematic(kbd, *(int *)arg));

	case PIO_KEYMAP:		/* set keyboard translation table */
	case OPIO_KEYMAP:		/* set keyboard translation table
					 * (compat) */
	case PIO_KEYMAPENT:		/* set keyboard translation table
					 * entry */
	case PIO_DEADKEYMAP:		/* set accent key translation table */
		sc->sc_accents = 0;
		/* FALLTHROUGH */
	default:
		return (genkbd_commonioctl(kbd, cmd, arg));
	}

	return (0);
}

static int
hkbd_ioctl(keyboard_t *kbd, u_long cmd, caddr_t arg)
{
	struct hkbd_softc *sc = kbd->kb_data;
	int result;

	/*
	 * XXX Check if someone is calling us from a critical section:
	 */
	if (curthread->td_critnest != 0)
		return (EDEADLK);

	/*
	 * XXX KDGKBSTATE, KDSKBSTATE and KDSETLED can be called from any
	 * context where printf(9) can be called, which among other things
	 * includes interrupt filters and threads with any kinds of locks
	 * already held.  For this reason it would be dangerous to acquire
	 * the Giant here unconditionally.  On the other hand we have to
	 * have it to handle the ioctl.
	 * So we make our best effort to auto-detect whether we can grab
	 * the Giant or not.  Blame syscons(4) for this.
	 */
	switch (cmd) {
	case KDGKBSTATE:
	case KDSKBSTATE:
	case KDSETLED:
		if (!mtx_owned(&Giant) && !HID_IN_POLLING_MODE_FUNC())
			return (EDEADLK);	/* best I could come up with */
		/* FALLTHROUGH */
	default:
		HKBD_LOCK(sc);
		result = hkbd_ioctl_locked(kbd, cmd, arg);
		HKBD_UNLOCK(sc);
		return (result);
	}
}


/* clear the internal state of the keyboard */
static void
hkbd_clear_state(keyboard_t *kbd)
{
	struct hkbd_softc *sc = kbd->kb_data;

	HKBD_LOCK_ASSERT(sc);

	sc->sc_flags &= ~(HKBD_FLAG_COMPOSE | HKBD_FLAG_POLLING);
	sc->sc_state &= LOCK_MASK;	/* preserve locking key state */
	sc->sc_accents = 0;
	sc->sc_composed_char = 0;
#ifdef HKBD_EMULATE_ATSCANCODE
	sc->sc_buffered_char[0] = 0;
	sc->sc_buffered_char[1] = 0;
#endif
	memset(&sc->sc_ndata, 0, sizeof(sc->sc_ndata));
	memset(&sc->sc_odata, 0, sizeof(sc->sc_odata));
	memset(&sc->sc_ntime, 0, sizeof(sc->sc_ntime));
	memset(&sc->sc_otime, 0, sizeof(sc->sc_otime));
}

/* save the internal state, not used */
static int
hkbd_get_state(keyboard_t *kbd, void *buf, size_t len)
{
	return (len == 0) ? 1 : -1;
}

/* set the internal state, not used */
static int
hkbd_set_state(keyboard_t *kbd, void *buf, size_t len)
{
	return (EINVAL);
}

static int
hkbd_poll(keyboard_t *kbd, int on)
{
	struct hkbd_softc *sc = kbd->kb_data;

	HKBD_LOCK(sc);
	/*
	 * Keep a reference count on polling to allow recursive
	 * cngrab() during a panic for example.
	 */
	if (on)
		sc->sc_polling++;
	else if (sc->sc_polling > 0)
		sc->sc_polling--;

	if (sc->sc_polling != 0) {
		sc->sc_flags |= HKBD_FLAG_POLLING;
		sc->sc_poll_thread = curthread;
	} else {
		sc->sc_flags &= ~HKBD_FLAG_POLLING;
		sc->sc_delay = 0;
	}
	HKBD_UNLOCK(sc);

	return (0);
}

/* local functions */

static int
hkbd_set_leds(struct hkbd_softc *sc, uint8_t leds)
{
	uint8_t id;
	uint8_t any;
	uint8_t *buf;
	int len;

	HKBD_LOCK_ASSERT(sc);
	DPRINTF("leds=0x%02x\n", leds);

	/* start transfer, if not already started */

	hidbus_set_xfer(sc->sc_dev, HID_XFER_READ | HID_XFER_WRITE);

#ifdef HID_DEBUG
	if (hkbd_no_leds)
		return (0);
#endif

	memset(sc->sc_buffer, 0, HKBD_BUFFER_SIZE);

	id = 0;
	any = 0;

	/* Assumption: All led bits must be in the same ID. */

	if (sc->sc_flags & HKBD_FLAG_NUMLOCK) {
		hid_put_data_unsigned(sc->sc_buffer + 1, HKBD_BUFFER_SIZE - 1,
		    &sc->sc_loc_numlock, leds & NLKED ? 1 : 0);
		id = sc->sc_id_numlock;
		any = 1;
	}

	if (sc->sc_flags & HKBD_FLAG_SCROLLLOCK) {
		hid_put_data_unsigned(sc->sc_buffer + 1, HKBD_BUFFER_SIZE - 1,
		    &sc->sc_loc_scrolllock, leds & SLKED ? 1 : 0);
		id = sc->sc_id_scrolllock;
		any = 1;
	}

	if (sc->sc_flags & HKBD_FLAG_CAPSLOCK) {
		hid_put_data_unsigned(sc->sc_buffer + 1, HKBD_BUFFER_SIZE - 1,
		    &sc->sc_loc_capslock, leds & CLKED ? 1 : 0);
		id = sc->sc_id_capslock;
		any = 1;
	}

	/* if no leds, nothing to do */
	if (!any)
		return (0);

#ifdef EVDEV_SUPPORT
	if (sc->sc_evdev != NULL)
		evdev_push_leds(sc->sc_evdev, leds);
#endif

	/* range check output report length */
	len = sc->sc_led_size;
	if (len > (HKBD_BUFFER_SIZE - 1))
		len = (HKBD_BUFFER_SIZE - 1);

	/* check if we need to prefix an ID byte */
	sc->sc_buffer[0] = id;

	if (id != 0) {
		len++;
		buf = sc->sc_buffer;
	} else {
		buf = sc->sc_buffer + 1;
	}

	DPRINTF("len=%d, id=%d\n", len, id);

	/* start data transfer */
	return (hid_set_report(sc->sc_dev, buf, len, HID_OUTPUT_REPORT, id));
}

static int
hkbd_set_typematic(keyboard_t *kbd, int code)
{
#ifdef EVDEV_SUPPORT
	struct hkbd_softc *sc = kbd->kb_data;
#endif
	static const int delays[] = {250, 500, 750, 1000};
	static const int rates[] = {34, 38, 42, 46, 50, 55, 59, 63,
		68, 76, 84, 92, 100, 110, 118, 126,
		136, 152, 168, 184, 200, 220, 236, 252,
	272, 304, 336, 368, 400, 440, 472, 504};

	if (code & ~0x7f) {
		return (EINVAL);
	}
	kbd->kb_delay1 = delays[(code >> 5) & 3];
	kbd->kb_delay2 = rates[code & 0x1f];
#ifdef EVDEV_SUPPORT
	if (sc->sc_evdev != NULL)
		evdev_push_repeats(sc->sc_evdev, kbd);
#endif
	return (0);
}

#ifdef HKBD_EMULATE_ATSCANCODE
static uint32_t
hkbd_atkeycode(int usbcode, int shift)
{
	uint32_t keycode;

	keycode = hkbd_trtab[KEY_INDEX(usbcode)];
	/*
	 * Translate Alt-PrintScreen to SysRq.
	 *
	 * Some or all AT keyboards connected through USB have already
	 * mapped Alted PrintScreens to an unusual usbcode (0x8a).
	 * hkbd_trtab translates this to 0x7e, and key2scan() would
	 * translate that to 0x79 (Intl' 4).  Assume that if we have
	 * an Alted 0x7e here then it actually is an Alted PrintScreen.
	 *
	 * The usual usbcode for all PrintScreens is 0x46.  hkbd_trtab
	 * translates this to 0x5c, so the Alt check to classify 0x5c
	 * is routine.
	 */
	if ((keycode == 0x5c || keycode == 0x7e) &&
	    shift & (MOD_ALT_L | MOD_ALT_R))
		return (0x54);
	return (keycode);
}

static int
hkbd_key2scan(struct hkbd_softc *sc, int code, int shift, int up)
{
	static const int scan[] = {
		/* 89 */
		0x11c,	/* Enter */
		/* 90-99 */
		0x11d,	/* Ctrl-R */
		0x135,	/* Divide */
		0x137,	/* PrintScreen */
		0x138,	/* Alt-R */
		0x147,	/* Home */
		0x148,	/* Up */
		0x149,	/* PageUp */
		0x14b,	/* Left */
		0x14d,	/* Right */
		0x14f,	/* End */
		/* 100-109 */
		0x150,	/* Down */
		0x151,	/* PageDown */
		0x152,	/* Insert */
		0x153,	/* Delete */
		0x146,	/* Pause/Break */
		0x15b,	/* Win_L(Super_L) */
		0x15c,	/* Win_R(Super_R) */
		0x15d,	/* Application(Menu) */

		/* SUN TYPE 6 USB KEYBOARD */
		0x168,	/* Sun Type 6 Help */
		0x15e,	/* Sun Type 6 Stop */
		/* 110 - 119 */
		0x15f,	/* Sun Type 6 Again */
		0x160,	/* Sun Type 6 Props */
		0x161,	/* Sun Type 6 Undo */
		0x162,	/* Sun Type 6 Front */
		0x163,	/* Sun Type 6 Copy */
		0x164,	/* Sun Type 6 Open */
		0x165,	/* Sun Type 6 Paste */
		0x166,	/* Sun Type 6 Find */
		0x167,	/* Sun Type 6 Cut */
		0x125,	/* Sun Type 6 Mute */
		/* 120 - 130 */
		0x11f,	/* Sun Type 6 VolumeDown */
		0x11e,	/* Sun Type 6 VolumeUp */
		0x120,	/* Sun Type 6 PowerDown */

		/* Japanese 106/109 keyboard */
		0x73,	/* Keyboard Intl' 1 (backslash / underscore) */
		0x70,	/* Keyboard Intl' 2 (Katakana / Hiragana) */
		0x7d,	/* Keyboard Intl' 3 (Yen sign) (Not using in jp106/109) */
		0x79,	/* Keyboard Intl' 4 (Henkan) */
		0x7b,	/* Keyboard Intl' 5 (Muhenkan) */
		0x5c,	/* Keyboard Intl' 6 (Keypad ,) (For PC-9821 layout) */
		0x71,   /* Apple Keyboard JIS (Kana) */
		0x72,   /* Apple Keyboard JIS (Eisu) */
	};

	if ((code >= 89) && (code < (int)(89 + nitems(scan)))) {
		code = scan[code - 89];
	}
	/* PrintScreen */
	if (code == 0x137 && (!(shift & (MOD_CONTROL_L | MOD_CONTROL_R |
	    MOD_SHIFT_L | MOD_SHIFT_R)))) {
		code |= SCAN_PREFIX_SHIFT;
	}
	/* Pause/Break */
	if ((code == 0x146) && (!(shift & (MOD_CONTROL_L | MOD_CONTROL_R)))) {
		code = (0x45 | SCAN_PREFIX_E1 | SCAN_PREFIX_CTL);
	}
	code |= (up ? SCAN_RELEASE : SCAN_PRESS);

	if (code & SCAN_PREFIX) {
		if (code & SCAN_PREFIX_CTL) {
			/* Ctrl */
			sc->sc_buffered_char[0] = (0x1d | (code & SCAN_RELEASE));
			sc->sc_buffered_char[1] = (code & ~SCAN_PREFIX);
		} else if (code & SCAN_PREFIX_SHIFT) {
			/* Shift */
			sc->sc_buffered_char[0] = (0x2a | (code & SCAN_RELEASE));
			sc->sc_buffered_char[1] = (code & ~SCAN_PREFIX_SHIFT);
		} else {
			sc->sc_buffered_char[0] = (code & ~SCAN_PREFIX);
			sc->sc_buffered_char[1] = 0;
		}
		return ((code & SCAN_PREFIX_E0) ? 0xe0 : 0xe1);
	}
	return (code);

}

#endif					/* HKBD_EMULATE_ATSCANCODE */

static keyboard_switch_t hkbdsw = {
	.probe = &hkbd__probe,
	.init = &hkbd_init,
	.term = &hkbd_term,
	.intr = &hkbd_intr,
	.test_if = &hkbd_test_if,
	.enable = &hkbd_enable,
	.disable = &hkbd_disable,
	.read = &hkbd_read,
	.check = &hkbd_check,
	.read_char = &hkbd_read_char,
	.check_char = &hkbd_check_char,
	.ioctl = &hkbd_ioctl,
	.lock = &hkbd_lock,
	.clear_state = &hkbd_clear_state,
	.get_state = &hkbd_get_state,
	.set_state = &hkbd_set_state,
	.get_fkeystr = &genkbd_get_fkeystr,
	.poll = &hkbd_poll,
	.diag = &genkbd_diag,
};

KEYBOARD_DRIVER(hkbd, hkbdsw, hkbd_configure);

static int
hkbd_driver_load(module_t mod, int what, void *arg)
{
	switch (what) {
	case MOD_LOAD:
		kbd_add_driver(&hkbd_kbd_driver);
		break;
	case MOD_UNLOAD:
		kbd_delete_driver(&hkbd_kbd_driver);
		break;
	}
	return (0);
}

static devclass_t hkbd_devclass;

static device_method_t hkbd_methods[] = {
	DEVMETHOD(device_probe, hkbd_probe),
	DEVMETHOD(device_attach, hkbd_attach),
	DEVMETHOD(device_detach, hkbd_detach),
	DEVMETHOD(device_resume, hkbd_resume),

	DEVMETHOD_END
};

static driver_t hkbd_driver = {
	.name = "hkbd",
	.methods = hkbd_methods,
	.size = sizeof(struct hkbd_softc),
};

DRIVER_MODULE(hkbd, hidbus, hkbd_driver, hkbd_devclass, hkbd_driver_load, 0);
MODULE_DEPEND(hkbd, hid, 1, 1, 1);
#ifdef EVDEV_SUPPORT
MODULE_DEPEND(hkbd, evdev, 1, 1, 1);
#endif
MODULE_VERSION(hkbd, 1);
#ifdef NOT_YET
HID_PNP_HOST_INFO(hkbd_devs);
#endif
