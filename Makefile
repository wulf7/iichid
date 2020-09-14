# $FreeBSD$

#.PATH:		${SRCTOP}/sys/dev/iicbus/input

.if !defined(OSVERSION)
# Search for kernel source tree in standard places.
.if empty(KERNBUILDDIR)
.if !defined(SYSDIR)
.for _dir in ${SRCTOP:D${SRCTOP}/sys} /sys /usr/src/sys
.if !defined(SYSDIR) && exists(${_dir}/kern/) && exists(${_dir}/conf/kmod.mk)
SYSDIR= ${_dir:tA}
.endif
.endfor
.endif
.if !defined(SYSDIR) || !exists(${SYSDIR}/kern/) || \
    !exists(${SYSDIR}/conf/kmod.mk)
.error Unable to locate the kernel source tree. Set SYSDIR to override.
.endif
.endif
OSVERSION!=	awk '/^\#define[[:space:]]*__FreeBSD_version/ { print $$3 }' \
		    ${SYSDIR}/sys/param.h
.endif

KMOD	= iichid
.if !defined(DISABLE_IICHID)
SRCS	= iichid.c iichid.h
.endif
SRCS	+= hconf.c hconf.h hgame.c hgame.h hms.c hmt.c hpen.c hsctrl.c hcons.c
SRCS	+= xb360gp.c ps4dshock.c
MAN	+= hidraw.4 hkbd.4 hms.4 hmt.4
SRCS	+= hidbus.c hidbus.h hid_if.c hid_if.h hid.c hid.h
SRCS	+= hid_debug.h hid_debug.c
SRCS	+= hidmap.h hidmap.c
SRCS	+= usbdevs.h
.if !defined(DISABLE_USBHID)
SRCS	+= usbhid.c
.endif
SRCS	+= hidraw.c hidraw.h
SRCS	+= hid_quirk.h hid_quirk.c
SRCS	+= acpi_if.h bus_if.h device_if.h iicbus_if.h
SRCS	+= opt_acpi.h opt_usb.h opt_evdev.h
SRCS	+= strcasestr.c strcasestr.h
# Revert 5d3a4a2 for compiling hkbd on pre 1300068 systems
.if ${OSVERSION} >= 1300068 || \
    (${OSVERSION} >= 1201507 && ${OSVERSION} < 1300000)
SRCS	+= opt_kbd.h opt_hkbd.h hkbd.c
CFLAGS	+= -DENABLE_HKBD
.endif
# hskbd conflicts with hkbd
#SRCS	+= hskbd.c
#CFLAGS	+= -DINVARIANTS -DINVARIANT_SUPPORT
CFLAGS	+= -DHID_DEBUG
CFLAGS	+= -DIICHID_DEBUG
CFLAGS	+= -DEVDEV_SUPPORT
.if ${OSVERSION} >= 1300083 || \
    (${OSVERSION} >= 1201513 && ${OSVERSION} < 1300000)
CFLAGS	+= -DHAVE_ACPI_IICBUS
.endif
CFLAGS	+= -DUSBHID_BUS_PROBE_PRIO=-10
.if ${OSVERSION} >= 1300055 || \
    (${OSVERSION} >= 1201507 && ${OSVERSION} < 1300000)
CFLAGS	+= -DHAVE_IG4_POLLING
.endif

.include <bsd.kmod.mk>
