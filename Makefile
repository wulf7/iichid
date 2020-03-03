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
SRCS	= iichid.c iichid.h
SRCS	+= hconf.c hconf.h hms.c hmt.c hpen.c hsctrl.c hcons.c
SRCS	+= hidbus.c hidbus.h hid_if.c hid_if.h hid.c hid.h hid_lookup.c
SRCS	+= hid_debug.h hid_debug.c
SRCS	+= hmap.h hmap.c
SRCS	+= usbdevs.h usbhid.c
SRCS	+= acpi_if.h bus_if.h device_if.h iicbus_if.h
SRCS	+= opt_acpi.h opt_usb.h opt_evdev.h
SRCS	+= strcasestr.c strcasestr.h
# Revert 5d3a4a2 for compiling hkbd on pre 1300068 systems
.if ${OSVERSION} >= 1300068 || \
    (${OSVERSION} >= 1201507 && ${OSVERSION} < 1300000)
SRCS	+= opt_kbd.h opt_hkbd.h hkbd.c
.endif
CFLAGS	+= -DHID_DEBUG
CFLAGS	+= -DIICHID_DEBUG
CFLAGS	+= -DEVDEV_SUPPORT
#CFLAGS	+= -DHAVE_ACPI_IICBUS
.if ${OSVERSION} >= 1300055 || \
    (${OSVERSION} >= 1201507 && ${OSVERSION} < 1300000)
CFLAGS	+= -DHAVE_IG4_POLLING
.endif

.include <bsd.kmod.mk>
