# The web interface

Open `http://<the Pi's address>/` in a browser on the same network (the
address is shown in the settings menu's status line and in the log once the
Pi has one from DHCP). One page, styled like the settings menu:

- **Status**: the emulator's version, whether the Mac is running, its speed,
  the Pi's address, both Ethernet addresses, sound and network.
- **Images on the SD card**: the `cd`, `hdd`, `rom` and `disks` folders,
  with sizes; upload a file into one (with a progress bar) or delete one.
  An upload is written as `name.part` and renamed when complete, so an
  interrupted upload leaves nothing behind. Images the running Mac uses
  can't be replaced or deleted. "Insert in CD drive" puts an image into
  the first CD-ROM drive, at once in a running Mac (as the menu's drive
  page does); the Status window lists the drives, with "Eject".
- **Settings**: `g3macpi.ini` as text. It is checked line by line before
  it is saved (an invalid line is reported and nothing is written).
- **Update the emulator**: upload a new `kernel8-rpi4.img`. It is accepted
  only if it is 64 KB to 32 MB and contains the G3MacPi image marker
  (`G3_KERNEL_MARKER`, in every build); the running image is kept as
  `kernel8-rpi4.bak`. "Restart the Pi" then stops the Mac (saving its
  state, as Ctrl+Alt+Delete) and restarts. If a new image doesn't start,
  take the SD card to a computer and rename `kernel8-rpi4.bak` back.

## Settings

```
[machine]
web = yes                ; the web interface (default yes)
web_password = something ; if set, the browser asks for it (any user name)
```

Without a password anyone on the network can use the page, upload and
replace the emulator: set one on a shared network. The password travels
unencrypted (HTTP Basic, RFC 7617); a `;` or `#` in it would start a comment.

## How it works

- `platform/common/http_server` — HTTP/1.1, one request per connection,
  bodies streamed with Content-Length (no chunked uploads).
- `platform/common/web_ui` — the page (`web_page.inc`) and the `/api`
  routes; files and actions through `WebHost`. Tested in
  `tests/unit/platform/test_web_ui.cpp`.
- `app/circle/web` — on the Pi: FatFs files (uploads written in 64 KB
  pieces), a task on core 0's scheduler serving port 80 with Circle's
  TCP/IP stack.
- One Ethernet port for two stations: Circle's stack gets a stand-in device
  (`CPiNetPort`); core 3 owns the adapter and switches frames by
  destination between the Pi and the Mac (`platform/common/net_switch`).
  The Mac's address is the Pi's with the locally-administered bit set; the
  adapter's filter takes both.
- `tools/webui_host` (`g3_webui_host <folder> [port] [password]`): the same
  interface on Linux, serving a folder; used to test with curl and a
  browser.

## Status

Tested on Linux: unit tests (requests in random pieces, uploads complete or
nothing, settings checked, update accepted only with the marker, password),
sabotage runs, curl (a 50 MB upload byte-identical; the real Pi image
installed byte-identical), and Chromium driving the page (upload, update).
On the Pi it is built but **not yet run**: QEMU's raspi4b has no Ethernet
adapter.
