# FT891 Next

A standalone control program for the Yaesu FT-891 on Linux. One background program owns the
radio's CAT port; you control it from a web browser on the same computer, from your phone,
and from other ham radio programs at the same time.

**Status:** version 0.6. Rig control, menus, memories, waterfall, filters and remote audio are
tested on air with a real FT-891. The SSTV decoder is tested with generated signals only;
on-air reports are welcome.

The interface is available in **English and Hungarian**. The first start follows the system
language (Hungarian on a Hungarian locale, English otherwise); change it on the Settings tab or
with `--lang en` / `--lang hu`. The choice is saved.

## Features

- **Rig control from any browser**: frequency, mode, band, VFO A/B, split, clarifier,
  levels, filters, S-meter and PO/SWR/ALC meters. Works on a phone, with a hold-to-talk PTT
  button.
- **Clean USB/LSB switching**: separate LSB, USB and Automatic sideband buttons through the
  SSB BFO menu. The radio's AUTO setting is only changed when the sideband really has to
  change.
- **Shared CAT port**: a rigctld-compatible server on `127.0.0.1:4532`, so WSJT-X, JS8Call,
  fldigi and loggers can use the radio together. A mode lock keeps external programs from
  changing the mode.
- **Menu editor**: all 159 menu items in a searchable list with descriptions. Changes are
  previewed, a backup is saved before every write, and every item is read back.
- **Profiles**: SSB, Digi and CW profiles plus your own, with a preview of what will change.
- **Memory manager**: channels 001-099 and P1L-P9U in a table, CSV export and import,
  recall with one click.
- **Waterfall** from the radio's USB sound card, click-to-tune and click-to-notch.
- **Filter panel**: IF shift, contour, manual notch and APF with a passband drawing;
  three-band microphone EQ editor.
- **Remote audio**: receive audio to the browser, microphone from the phone or computer to
  the radio during PTT.
- **SSTV decoder**: Scottie, Martin, Robot and PD modes, VIS detection, slant correction,
  PNG gallery.
- **Power on/off** over CAT.
- **Safety**: web PTT drops within 1.2 s when the connection is lost, TX time limit,
  CAT PTT drop detection, no menu writes during TX.

## Building

Needs gcc and make. Waterfall and audio use `arecord` and `aplay` from `alsa-utils`. The
phone microphone needs HTTPS, so install `libssl-dev` before building (Debian/Ubuntu
package names):

```bash
sudo apt install build-essential alsa-utils libssl-dev openssl
make
sudo usermod -a -G dialout $USER     # once, then log out and in again
./ft891ctl
```

Without `libssl-dev` the program still builds, but the microphone works only on the
computer itself.

The program finds the radio's USB port and baud rate, then prints its addresses and PIN:

```text
  This computer:  http://127.0.0.1:8891
  From a phone:   http://192.168.1.20:8891
  With mic:       https://192.168.1.20:8892
  PIN:            123456
```

No PIN is needed on the computer itself. On a phone you enter it once; it is remembered for
30 days while the program runs.

Recommended radio menu settings:

| Menu | Value | Why |
|---|---|---|
| 05-06 CAT RATE | 38400 | Any speed is detected; this one is the fastest |
| 05-07 CAT TOT | 1000 ms | The default 10 ms can break CAT replies |
| 05-08 CAT RTS | DISABLE | Both settings work when the port is recognised |

## Windows (experimental)

The Windows build is new and has not been tested on air yet. Everything works as on Linux
except HTTPS: the browser microphone works on the Windows computer itself, but a phone
microphone needs `tailscale serve` (see Remote use) or the Linux build. Audio uses the
Windows sound devices directly; choose the radio's "USB Audio CODEC" input in Settings.

Build it on Linux with MinGW:

```bash
sudo apt install mingw-w64
make windows          # makes ft891ctl.exe
```

Copy `ft891ctl.exe` to the Windows computer and start it from a command window. Install the
Silicon Labs CP210x driver first. The program finds the radio's "Enhanced COM Port"
automatically, or use `--cat-port COM5`. Settings are stored in `%APPDATA%\ft891ctl`.
Windows Firewall asks once whether the program may accept connections; allow it on private
networks if you want to use a phone.

## Remote use

On your home network open the printed address on your phone. From outside, use a **VPN**
such as WireGuard or Tailscale. **Do not forward the port on your router**: the interface
can key the transmitter, and a PIN is not enough protection on the internet.

Browsers give access to the microphone only over HTTPS. The program makes its own
certificate, so the browser warns once; accept it. On Android Chrome this is usually enough.
With Tailscale, a real certificate is simpler:

```bash
sudo tailscale serve --bg 8891
```

then open the `https://...ts.net` address.

**Audio input**: for remote microphone use the radio's rear input: menu 11-05 SSB MIC SELECT
(SSB) or 08-09 DATA IN SELECT (DATA) must be REAR. The interface warns and switches it with
one button, and switches back to the hand microphone the same way. Set the level with menu
16-11 SSB DATA GAIN and the microphone slider, on a dummy load, watching ALC.

## Other programs

Connect other programs to the built-in rigctld-compatible port, and do not run a separate
rigctld (it would hold the serial port):

- **WSJT-X, JS8Call**: Rig `Hamlib NET rigctl`, Network Server `127.0.0.1:4532`, PTT `CAT`.
- **fldigi**: Hamlib NET rigctl, same address.
- **Loggers**: rigctld commands `f` and `m`, or simple HTTP:

```json
GET http://127.0.0.1:8891/api/rig
{"connected":1,"freq":14074000,"freq_b":7074000,"mode":"PKTUSB","adif_mode":"DATA","tx":0,"split":0}
```

## Menus and memories

Menu writes are always previewed first, backed up to `~/.config/ft891ctl/backups` (last 20
kept, restorable from the interface) and read back. The program never writes CAT RATE,
CAT RTS, REF FREQ ADJ, EMERGENCY FREQ, the version menus or RESET, and never reads RESET.
Menu 11-07 is written only when the radio's menu table matches the 2017 CAT reference
(on an older table that number was SSB PTT SELECT).

Memory CSV header (any column order; the Hungarian header of earlier versions is also read):

```text
channel,freq_hz,mode,name,shift,clar_on,clar_hz,tone
```

Modes: LSB, USB, CW, CW-R, FM, FM-N, AM, AM-N, RTTY-L, RTTY-U, DATA-L, DATA-U. Names up to
12 ASCII characters. Shift: 0 simplex, 1 plus, 2 minus. Channels cannot be deleted over CAT,
only overwritten; the CTCSS tone value is not part of the memory command.

## SSTV

The SSTV tab listens for a VIS header and shows the picture line by line on every open
interface. Pictures are saved as PNG to `~/.config/ft891ctl/sstv` (last 50 kept). Supported
modes: Scottie 1, 2, DX, Martin 1, 2, Robot 36, 72, PD 50, 90, 120, 160, 180, 240, 290.
A mode can also be started by hand when the VIS was missed.

## Power on

Power off always works. Power on works only if the radio's USB port stays visible to the
computer while the radio is off; try it at home before relying on it remotely.

## Options

```text
--cat-port PATH   CAT port, e.g. /dev/ttyUSB0 or COM5 (default: automatic search)
--ptt-port PATH   RTS/DTR PTT port (default: the CAT port's pair)
--web-port N      web port (8891)
--web-bind ADDR   web address; 127.0.0.1 = this computer only
--tls-port N      HTTPS port (web port + 1), 0 = off
--rig-port N      rigctld-compatible port (4532), 0 = off
--rig-bind ADDR   rigctld-compatible address (127.0.0.1)
--poll-ms N       polling interval with an open interface (250)
--arecord PATH    arecord program
--aplay PATH      aplay program
--no-ai           do not use the radio's Auto Information
--trace           log every CAT command
--config-dir PATH settings folder (~/.config/ft891ctl)
--new-pin         make a new PIN
--lang hu|en      interface and message language (default: system language)
```

When something does not work, run `./ft891ctl --trace` and look at
`~/.config/ft891ctl/ft891ctl.log`.

## Testing without a radio

```bash
python3 tools/ft891sim.py --link /tmp/ft891 &
./ft891ctl --cat-port /tmp/ft891
make test      # automatic tests, about 3 minutes; the SSTV tests need python3-numpy
```

The simulator follows the 2017 FT-891 CAT reference and can inject faults (older menu table,
dropping CAT PTT, radio switched off). It does not replace tests on a real radio.

## Source files

| file | purpose |
|---|---|
| `src/rig.c` | CAT core: one thread owns the serial port, PTT handling, reconnect |
| `src/rigsrv.c` | rigctld-compatible server |
| `src/web.c` | web server, WebSocket, PIN, HTTPS |
| `src/menu.c`, `src/menus.def` | menu table, profiles, backups, memories |
| `src/serial.c` | serial port for Linux and Windows |
| `src/compat.c`, `src/net.h` | other Linux/Windows differences |
| `src/audio.c` | waterfall and audio (arecord, aplay, FFT, mu-law) |
| `src/sstv.c` | SSTV decoder |
| `web/index.html` | the interface, built into the program |
| `tools/ft891sim.py` | FT-891 simulator with fault injection |
| `tools/sstvgen.py` | SSTV signal generator for the tests |
| `tests/run_tests.py` | automatic tests |

## License

Copyright (C) 2026 HG0MV

This program is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either version 3
of the License, or (at your option) any later version. See [LICENSE](LICENSE).

73 de HG0MV
