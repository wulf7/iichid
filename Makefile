# $FreeBSD$

#.PATH:		${SRCTOP}/sys/dev/iicbus/input

KMOD	= iichid
SRCS	= iichid.c iichid.h hconf.c hconf.h hms.c hmt.c
SRCS	+= hidbus.c hidbus.h hid_if.c hid_if.h hid.c hid.h hid_lookup.c
SRCS	+= hid_debug.h hid_debug.c
SRCS	+= usbdevs.h usbhid.c
SRCS	+= acpi_if.h bus_if.h device_if.h iicbus_if.h
SRCS	+= opt_acpi.h opt_usb.h opt_evdev.h
CFLAGS	+= -DHID_DEBUG
#CFLAGS	+= -DHAVE_ACPI_IICBUS
#CFLAGS	+= -DHAVE_IG4_POLLING

.include <bsd.kmod.mk>
