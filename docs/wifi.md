# WiFi

Status: **built, unit-tested, not yet run on a real Pi.**

With `wifi = yes` (docs/machine-config.md) the Pi joins a WiFi network with
its on-board radio instead of using its Ethernet port, for both the web
interface and the Mac's Ethernet (BMAC+, docs/hardware/bmac.md).

## Parts

- **Driver and supplicant**: Circle's WLAN add-on
  (`third_party/circle/addon/wlan`: a port of Plan 9's driver for the
  Broadcom/Cypress radio, and of hostap's `wpa_supplicant`). It needs the
  radio's firmware in `/firmware` on the SD card; `tools/build-pi.sh`
  downloads the files Circle names (addon/wlan/firmware/Makefile), at the
  revision Circle pins, from Raspberry Pi OS's firmware-nonfree, with that
  package's licence terms (`firmware/LICENCE-firmware-nonfree.txt`).
  WPA/WPA2 personal and open networks (Circle's supplicant build has no
  WPA3/SAE).
- **Menu**: Enter on "Network port" opens a WiFi page where the name,
  password and country are typed (platform/common/key_text.h maps keys to
  characters for US and UK keyboards).
- **Search for networks** (platform/common/wifi_scan.h, `CKernel::ScanWifi`):
  the menu asks the front end to search (Action::ScanWifi). The radio is
  started for it if WiFi isn't in use yet (the network stays on Ethernet
  until a restart). Circle's driver command `escan 10` scans every 10 s
  until `escan 0`; the scan's messages are read for up to 8 s, until a
  pass that found something ends (an empty message). Broadcom's BSS
  records give the name, signal (dBm), channel and, from the information
  elements, the security; one entry per name (the strongest), strongest
  first, hidden networks left out. Circle's driver scans channels 1-14
  only (2.4 GHz). Picking a network takes its name and turns WiFi on; for
  WPA/WPA2 the password is typed next; WEP and WPA3-only are refused (the
  supplicant here does WPA-PSK). While the supplicant is running and not
  connected it scans too and may take queued results: a search can come
  back short, and searching again helps. Not yet tried on a Pi.
- **Configuration**: the settings `wifi_name`, `wifi_password`,
  `wifi_country` are written as `/g3macpi/wpa_supplicant.conf` each start
  (platform/common/wpa_config.h); the name in hex, so any name works.
  `freq_list` limits it to 2.4 GHz (channels 1-13): on the user's Pi 400
  the supplicant chose a dual-band router's 5 GHz access point (5290 MHz,
  a DFS channel) and Circle's driver reported "wifi join: failed" every
  time (2026-11-26). A 5 GHz-only network can't be joined.
- **Frame switch**: a task on core 0 (`CWifiSwitch`, app/circle/web.h). The
  Ethernet switch runs on core 3, but Circle's WLAN driver does its SDIO
  work in core 0's scheduler tasks and may block, so it is only called
  there. It runs each time core 0 yields, between the Mac's time slices.
- **Sharing the address** (platform/common/wifi_bridge.h): an access point
  accepts frames only from the address of the station that joined it, so
  the Mac cannot use its own address as it does on Ethernet. Its frames go
  out under the Pi's address, with ARP's and AARP's sender address changed
  too and DHCP requests asking for broadcast replies; frames that come back
  to the Pi's address go to the Mac if they are ARP for, or IPv4 to, the
  Mac's IPv4 address (learnt from its own frames), DHCP replies for its
  hardware address, or AppleTalk (DDP, AARP). Everything else goes to the
  Pi. Frames between the Pi and the Mac are delivered directly.

## Limits

- One Mac per Pi (the translation follows one IPv4 address).
- Protocols other than IPv4, ARP and AppleTalk reach the Mac only as
  broadcasts.
- Throughput depends on how often core 0 yields to its tasks while the
  Mac runs.
