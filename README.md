Microsoft IntelliMouse utilities. Tested on Pro IntelliMouse Shadow with `VendorID=045E` (Microsoft) and `ProductID=082A` (Pro IntelliMouse).

Tools:
* `TailLight.exe`: Command-line utility for changing the tail-light color.

### Tail-light protocol
Steps to update the tail-light:
* Connect to mouse HID device with `Usage=0212` and `UsagePage=FF07`.
* Send a *feature report* with the following header:
```
FeatureReport[0] = 0x24; // ReportID 36
FeatureReport[1] = 0xB2; // magic value
FeatureReport[2] = 0x03; // magic value
FeatureReport[3] = red;
FeatureReport[4] = green;
FeatureReport[5] = blue;
```

### How to reverse-engineer the protocol
It's possible to reverse-engineer the IntelliMouse USB HID protocol by installing [USBPcap](https://desowin.org/usbpcap/) and [Wireshark](https://www.wireshark.org/).

Steps:
1. Open Wireshark with USBPcap plugin.
2. Open "Microsoft Mouse and Keyboard Center" app.
3. Change tail-light color.
4. Observe USB HID feature report request packages that adhere to the format described above. 

![USB capture](USB_capture.png)

The feature report also contains a number of other non-zero bytes. I don't know if they carry a meaning or are just uninitialized memory.

## Driver development
Relevant resources for getting started with driver development against IntelliMouse:
* [Windows Driver Kit (WDK)](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk) installation.
* [KMDF filter driver for a HID device](https://github.com/microsoft/windows-driver-samples/tree/main/hid/firefly) sample.
