# iichid - Generic HID layer for FreeBSD

iichid is a set of abstract HID drivers for FreeBSD. All the drivers use evdev
protocol to communicate with userland applications like libinput and
xf86-input-evdev. hidbus is used to connect HID drivers with transport
backends (usbhid for USB and iichid for I2C) and allows multiple HID drivers
to attach to a single physical device.

Current drivers and respective HID devices supported by them are following:

* **hms** - HID mouses. Physical relative devices with roller or optical sensor
  are supported as well as virtual absolute devices like VBox, Bhyve and VMWare
  mices.
* **hmt** - MS Windows 7+ compatible Multi-touch devices like touchscreens
  and precission touchpads.
* **hconf** - Mouse/touchpad mode switches on precission touchpads.
* **hkbd** - HID keyboards. AT-key subset.
* **hpen**  - Generic / MS Windows compatible HID pen tablets.
* **hcons** - Consumer page AKA Multimedia keys.
* **hsctrl** - System Controls page (Power/Sleep keys).

## System requirements

FreeBSD 12.1+. Recent (any from 2020) CURRENT or 12-STABLE are preferred.

## Downloading

This project does not have a special home page. The source code and the
issue tracker are hosted on the Github:

https://github.com/wulf7/iichid

## Building

To build driver, cd in to extracted archive directory and type

```
$ make
```

You need the sources of the running operating system under **/usr/src**

## Installing

To install file already built just type:

```
$ sudo make install
```

and you will get the compiled module installed in **/boot/modules**

To load the module at a boot time, add the module **iichid** to **kld_list**
variable in **/etc/rc.conf**

To handle keyboards in single user mode and at the early stages
of booting process, iichid should be loaded at kernel startup.
Add following lines to **/boot/loader.conf** in that case:

```
ig4_load="YES"
iicbus_load="YES"
iichid_load="YES"
```

## Configure Xorg

Generally speaking, it is a bad idea to use static Xorg configuration for
evdev devices due to dynamic unit number assignment. The simplest way
to get new devices recognized by Xorg-server is to rebuild it with DEVD
(since 1.20.7) or UDEV autoconfiguration backend enabled.

Static configuration is still possible. Add following snippet to a file under
**/usr/local/etc/X11/xorg.conf.d/**

```
Section "InputDevice"
	Identifier "HID device"
	Driver "libinput"
	Option "Device" "/dev/input/event6"
	Option "AutoServerLayout" "true"
EndSection
```

You may need to run **sudo libinput list-devices** to find out unit number
belonging to given device. Note that it can change across reboots.

## Bug reporting

You can report bugs at 'Project Issues' page
https://github.com/wulf7/iichid/issues
It is recommended to enclose console output of 'kldload iichid.ko' command
with your report.

Some additional information that can be helpful especially if device has not
been detected at all can be obtained with following commands:

```
$ devinfo -rv	# Very verbose
$ pciconf -lv
$ sudo usbconfig
$ dmesg
```
