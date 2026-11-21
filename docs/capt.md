# Canon CAPT lasers (LBP2900, LBP3050): feasibility, protocol, status

This covers sharing a Canon **LBP2900** (and LBP2900B) or **LBP3050** (and
LBP3010 / LBP3018, which share its USB id) over Wi-Fi from an ESP32-S3
N16R8, without a Windows print server. It separates what is **verified**
from what is **assumed**. Nothing here has run on a real CAPT printer yet.

Status (2026-11-21):

| | |
|---|---|
| Protocol research | done, sources below |
| Hi-SCoA compression | written from the format description; output **byte-identical** to captdriver's encoder (the one its users print with) on 294 test bands |
| CAPT engine (commands, status, paper out, cancel, recovery) | written, tested against an emulated printer only |
| Drivers `canon-lbp2900`, `canon-lbp3050` in the firmware | written, firmware builds, host tests pass |
| On a real LBP2900 / LBP3050 | **not tested**. The test plan is at the end |

## 1. Feasibility

**Verdict: feasible on the ESP32-S3 alone, with no rendering host, for every
client that sends raster** (Android's built-in print service and Mopria,
iOS/macOS AirPrint, Windows 10/11's IPP class driver, Linux CUPS with IPP
Everywhere). These clients rasterise the document themselves and send PWG or
URF raster. The board only has to turn gray raster into the printer's 1-bit
image, compress it and run the protocol, and that fits easily.

**When a rendering host would be needed:** only for clients that can send
nothing but PDF or PostScript. The firmware does not advertise PDF, so IPP
clients fall back to raster. A PDF interpreter on the ESP32 (fonts,
transparency, images at 600 dpi) is not realistic in 512 KB of RAM. If such a
client has to be supported, the simplest working setup is a Raspberry Pi or a
PC running CUPS that renders to PWG raster and prints to this board over IPP
(`ipp://<board>/ipp/print`, driver "IPP Everywhere"). The board then stays the
only thing that talks CAPT. No current Android, iOS, macOS or Windows client
needs this.

### Why the printer cannot take a standard print job

Verified from several independent sources (captdriver's SPECS and code,
Canon's own Linux driver as reverse-engineered by ValdikSS, kernel logs):

- Both printers use the **USB printer class interface** (class 7). Linux's
  `usblp` binds to the LBP2900; this is why it shows up as a printer. But they
  have **no page description language**: no PCL, PostScript, PDF or plain
  raster. Sending such data prints nothing or garbage.
- The host renders every page to a **1-bit 600 dpi bitmap**, compresses it
  band by band with **Hi-SCoA** (Canon's own LZ-like scheme), and wraps it in
  **CAPT** command packets.
- CAPT is **request/reply**. Commands like the status queries (`0xA0A8`,
  `0xE0A0`), `0xA2A0` ReserveUnit and `0xE0A7` StartPrint have replies on bulk
  IN, and a reply **must be read before the next command** or the printer
  deadlocks. A driver that streams bytes and reads the back channel "when it
  can" (as the inkjet drivers here do) does not work.
- The printer **does not buffer jobs on its own**. The host has to wait for
  page counters, tell the engine when to start each page, and handle paper out
  (on the LBP2900 the user reloads paper and presses the printer's button).

### Resources on the ESP32-S3 N16R8

| need | size | where |
|---|---|---|
| A4 image at 600 dpi, 1 bit | 592 bytes x 6776 lines = 3.9 MB raw | never held whole |
| one band (70 lines) | 41 KB | heap |
| compressed page | text 0.05-1.1 MB; worst case (noise) 6.1 MB (A4, Letter) | the 7.5 MB PSRAM buffer |
| Hi-SCoA cost | at most 1.5 x raw, every band fits one 64 KB USB command | |

- The whole compressed page is kept until the printer has it. Then a page that
  ran into paper-out can be **sent again** without asking the client, and the
  printer is only reserved once the page is ready, so a slow network never
  starves a running engine.
- A page larger than the printer's own memory (2 MB on the LBP3000; LBP2900
  and LBP3050 unknown) is started while it is still being sent. The emulated
  printer covers this case (a 5.8 MB page streams with the buffer level never
  overrun).
- **Legal** paper is not offered: its worst case, 7.7 MB, does not fit.
- CPU: gray to 1 bit (clustered-dot screen) plus Hi-SCoA, for a mostly white A4
  page, is about 0.1 s on a desktop CPU. Expect roughly 1-2 s on the ESP32
  (this is an estimate, to be measured; each job logs its render, compress and
  printer time). The LBP2900 prints 12 pages per minute (5 s per page) and the
  LBP3050 prints 18 (3.3 s per page), so rendering should keep up.
- USB full speed (the S3's only mode) moves about 1 MB/s. A text page is 30-300
  KB compressed, so the transfer takes well under a second.

## 2. Architecture

```
Android / iOS / Windows / macOS / Linux
   │  IPP over Wi-Fi: Print-Job / Send-Document, PWG or URF raster (gray 8-bit, 300 or 600 dpi)
   ▼
components/airprint      IPP server, mDNS (_ipp._tcp, _universal, _print), raster decoder (one line at a time)
   ▼  page_line()
components/printers/canon_capt.c
   crop to the image area, RGB→gray, 45° clustered-dot screen, 300→600 dpi,
   bands of 70 lines → Hi-SCoA → whole page in PSRAM
   ▼  capt_print_page()
components/capt          capt.c: the CAPT engine (commands, status, waits with deadlines, paper out, cancel)
                         hiscoa.c: compression
   ▼  printer_io_t: write / read (request-reply) / status / cancelled
src/usb_printer.c        "raw" back channel: no status polling, synchronous bulk IN reads with timeouts
   ▼
USB OTG (full speed) ── printer
```

Between jobs a task asks the printer for its status every 3 s
(`print_job_start_poll`), so IPP, the web UI and the OLED show paper out,
jam and similar states.

## 3. The USB devices

| model | USB id | IEEE 1284 MDL | source |
|---|---|---|---|
| LBP2900 / LBP2900B | `04a9:2676` | `LBP2900` | lsusb and kernel logs (several), captdriver; the B variant's id is assumed to be the same |
| LBP3010 / LBP3018 / LBP3050 | `04a9:26da` | `LBP3010/LBP3018/LBP3050` | captdriver wiki (one shared id), captdriver's driver table (it matches the MDL string). **Not seen on a device** |

The firmware picks the driver by USB id first, then by MDL. A printer stuck in
a bad state is reported to show `MDL:Canon CAPT USB Device`; the USB id still
matches it.

Interface: printer class, bulk OUT and bulk IN (captdriver talks through
CUPS's usb backend, which uses these). The exact endpoint numbers and packet
sizes will appear in the board's log at attach (`usb_printer: printer
interface ...`). The LBP2900 answers the printer-class GET_PORT_STATUS request
with a stall (`usblp: error -32 reading printer status` in kernel logs), so the
firmware does not send that request to CAPT printers.

## 4. The protocol as implemented

Sources:
- **captdriver** (Alexey Galakhov 2013, mounaiban 2020-): `SPECS` and
  `src/prn_lbp2900.c`. Its users print on the LBP2900 ("WORKS") and the LBP3010
  ("WORKS"). It is GPL-3. No code was copied: the firmware is MIT, so it is
  written from the format and protocol descriptions. captdriver is used only
  as a test reference (`tools/captdriver_ref/`).
- **ValdikSS's fork** (2026): `info/lbp3000-protocol.md`, from USB captures of
  Canon's Windows XP and Linux drivers driving an LBP3000. Tested by its author
  on the LBP2900 and LBP3000.
- captdriver issues #7 (long delays between pages), #8 (stuck jobs), #33
  (LBP2900 prints too light) and #57 (fork: LBP2900 out of sync after paper out
  or cancel; multi-page jobs report a jam after page 1).

Framing (verified in every source): `u16le command, u16le total size,
payload`. A reply comes back as one 6-byte packet, then the rest. Some
firmwares write the size in BCD, and the reader accepts both.

Sequence. This is captdriver's, which is the one shown to print on both
models. The changes are marked ★.

1. ★ Read and drop stale replies (left over from a reply that timed out).
2. `A1A1` GetPrinterInfo (★ its block size limits the data commands), status.
3. `A3A2`, then `A2A0` ReserveUnit (payload `00 00 1e 00 …`) gives the job id.
   ★ If refused (`0x80`, a job left over by a crash or a pulled cable), clear
   the printer (`E0A3`, `E0A2`, `E0A4`, `E0A5` GoOnline) and retry once. This
   step comes from ValdikSS.
4. `E1A2` (LED), `E1A1` SetJobInfo (flag 1).
5. Per page:
   - If the status says offline or not initialised: `E0A3`, `E0A2`, `E0A4`,
     `E0A5` (magic `ee db ea ad`).
   - Wait until "busy" (bit `0x04`) clears.
   - `D0A9`, which holds `D0A0` page parameters (40 bytes), `D0A4` Hi-SCoA
     parameters, `D0A1` and `D0A2`.
   - `C0A0` raster chunks: whole bands up to the printer's block size. ★ After
     each chunk the status is read. When the free-buffer level reaches 0, the
     page is started early (streaming), then the sender waits for room. This
     follows ValdikSS's captures. captdriver only waits every 16 chunks.
   - `C0A4` end of raster.
   - Wait until "received" equals "decoding" (the page counters of `A0A8`).
   - `E1A1` (flag 2), `E0A7` StartPrint(page), `E1A1` (flag 6).
   - Wait until the page is out (`pipeline=0`, captdriver) or printing
     (`pipeline=1`, ValdikSS: faster). Paper out (not-ready / print-rejected
     with the engine stopped ★ for 1.5 s, so a short pause between two steps
     is not taken for paper out and does not print a page twice): blink the
     LED, wait for the button (LBP2900: `aux & 0x20`; LBP3010 family: the
     "paper ready" bit has to go off, then on again), then send the page again.
6. End: wait until the last page is done, `E1A1` (flag 4), `E0A9`
   ReleaseUnit. ★ After a cancel or an error: drop an unstarted page
   (`E0A4`), let started pages finish, then release. Started pages always come
   out before the release.

★ **Every wait has a deadline**: reply 5 s, printer busy 180 s, page 120 s,
paper 10 min. All are settable. When a deadline expires, the job fails with a
reason and the next job starts clean. In captdriver every loop is unbounded
(`while (1) sleep(1)`), and a printer that never sets the expected bit hangs
the job forever. That is the most likely cause of "freezes after a few
pages". captdriver also polls once a second; the firmware polls every 100 ms,
which removes most of the dead time between pages (issue #7).

★ One error does not affect the next job: the engine clears its error state
at every job start. The host tests found this bug.

Model differences (captdriver's table):

| | LBP2900 | LBP3010 / 3018 / 3050 |
|---|---|---|
| status | `E0A0`, then `A0A8` when the status says so | `A0A8` only |
| `D0A0` byte 12 (plain paper) | `0x00` | `0x01` |
| LED payloads | 12 bytes | 16 bytes (`13 00 …`, blink `31 00 …`) |
| "paper reloaded" | `aux & 0x20` (button) | extended byte 10 bit 7 |
| last band | "end of page" (ValdikSS) | "end of band" (captdriver) |
| `D0A0` bytes 2-3 | `30 2a` | `30 2a` (what captdriver sends; captdriver's SPECS says the LBP3010 uses `ba 09`) |

Page parameters (`D0A0`): paper code (A4 `02`, Letter `0d`, A5 `03`), toner
density `d << 2` in 4 bytes (★ default 7 = `0x1c`; captdriver sent `0x00`
to the LBP2900, which issue #33 reports as too light), `0x11` (600 dpi),
image origin x = 120, y = 96 px, line size (A4: 592 bytes, 4736 px), lines
(A4: 6776, as Canon's Windows driver), and sheet size (A4 4960 x 7014 px).

### What is assumed or unknown

- **The meaning of the status bits.** captdriver and ValdikSS read the same
  bits differently: `0x80` is "busy" to one and an error to the other; `0x04`
  is "buffer full" to one and "command busy" to the other. The engine follows
  the behaviour that has printed (it waits for `0x04` to clear before a page,
  and waits for `0x80` with a soft 10 s limit). The meanings of jam, door
  open and no cartridge (`eng` bits `0x0100`, `0x4000`, `0x2000`) come from the
  LBP3000 only.
- **LBP3050 specifics:** its USB id, its MDL string, whether `A3A2` and the
  16-byte LED payload are right for it, and the buffer level in `A0A8` bytes
  4-5. All of it comes from the LBP3010 path of captdriver; nobody has
  published an LBP3050 capture.
- The printers' memory (affects when streaming starts), and whether an LBP2900
  ever reports its free-buffer level. If it never reports a non-zero level, the
  engine ignores the field.
- Whether a printer accepts the next page's `D0A9` while the previous page is
  still printing. The engine does not do this; it waits, as captdriver does.
- How an LBP2900 reports being out of paper *before* a job. The test page
  and the status poll show "not ready (warming up or no paper)".
- The emulated printer (`components/capt/fake_capt.c`) implements *this
  reading* of the protocol. Its tests show that the engine does what is
  intended, recovers, and never violates the framing or the reply rule. They
  do not show that a real printer agrees.

## 5. Hardware and power

- **Board:** ESP32-S3 N16R8 dev board with two USB-C ports. The port marked
  **COM/UART** is for flashing and power. The port marked **USB** is the OTG
  host for the printer. Use a USB-C (male) to USB-B (male) cable, or a USB-C
  OTG adapter plus the printer's normal USB-A to USB-B cable.
- **VBUS:** the printers are self-powered and draw no current from USB, but
  a printer only connects when it sees 5 V on VBUS. On most of these boards
  the OTG port's VBUS is not connected until the **"USB-OTG" solder bridge**
  is closed (see README). Without it, nothing appears in the log when the
  printer is plugged in.
- **Power:** feed the board through the COM port from a 5 V, ≥ 1 A supply.
  With the bridge closed, the OTG port's VBUS comes from the same 5 V.
- Optional: the OLED (SCL GPIO41, SDA GPIO42) shows printing, out of paper,
  and similar states. The BOOT button: press twice for a test page, hold 2 s
  to cancel.

## 6. Build and flash

```
pip install platformio            # or the VS Code extension
pio run -e lbp2900                # first build creates .env; edit it:
#   PRINTER_NAME=Canon LBP2900  HOSTNAME=lbp2900  MODE=debug  (WIFI_SSID / WIFI_PASSWORD optional)
pio run -e lbp2900 -t upload      # board on the COM port
tools/set_wifi.py /dev/ttyUSB0    # Wi-Fi over the COM port (or: join "lbp2900-setup" with a phone)
pio run -e lbp2900 -t logs        # follow the log over Wi-Fi (MODE=debug)
```

One firmware per printer (`platformio.ini`):

| env | for | driver built in | output |
|---|---|---|---|
| `lbp2900` | LBP2900 / LBP2900B | `canon-lbp2900` only | `.pio/build/lbp2900/firmware.bin` |
| `lbp3050` | LBP3010 / LBP3018 / LBP3050 | `canon-lbp3050` only | `.pio/build/lbp3050/firmware.bin` |
| `esp32s3` (default) | every supported printer | all, chosen by USB id | `.pio/build/esp32s3/firmware.bin` |

Each one announces its model (IPP, mDNS) as soon as it starts, before the
printer is plugged in. The two boards need different `HOSTNAME` /
`PRINTER_NAME` in `.env` if they share a network (`.env` is shared by the
envs: edit it between the two builds, or flash and set the name later).

`MODE=debug` adds the remote log (`nc <board> 2323`), OTA (`pio run -t ota`)
and `http://<board>:8080/` (`/info`, `/log`, `/capt`). The build is
reproducible: `platformio.ini` pins the platform (espressif32 6.12.0, ESP-IDF
5.5), and `sdkconfig.defaults` holds every setting.

Host tests (no hardware):

```
make -C host && host/capt_host          # Hi-SCoA, engine scenarios, drivers on the emulated printer
CAPTDRIVER=1 tools/run_tests.sh         # everything, + Hi-SCoA byte comparison with captdriver
cd host && PRINTER_DRIVER=canon-lbp2900 ./airprint_host 8631   # the IPP server on an emulated LBP2900
tools/ipp_print.py http://localhost:8631/ipp/print --pages 3   # print to it (decoded pages: host/out/job-*.pbm)
```

## 7. Real-hardware test plan (both models)

Before each session: `MODE=debug`, the board's log open (`pio run -t logs` or
`nc <board> 2323`), and the trace on:
`curl -X POST 'http://<board>:8080/capt?trace=1'`. Each command and reply
then goes to the log (`I capt: > e0a0 …` / `< e0a0 …`). **Keep the logs: they
are what turns the assumptions above into facts.** Steps marked (stop) need
a fix before going further if they fail.

| # | step | expected | what it verifies |
|---|---|---|---|
| W1 | flash with `WIFI_SSID` empty; on a phone, join `<HOSTNAME>-setup` | the sign-in page opens (else `http://192.168.4.1/wifi`); networks listed; pick one, enter the password: "Saved. Connecting…"; within a minute the board is at `http://<HOSTNAME>.local/`, the setup network closes | setup AP, captive portal, station |
| W2 | `tools/set_wifi.py <COM port>` with another network | "Wi-Fi saved. The board is restarting."; the board joins it | COM port setup |
| W3 | switch the router off for 2 min | `<HOSTNAME>-setup` appears after ~1 min; router back on: the board reconnects, the setup network closes | fallback and recovery |
| 1 | plug the printer in, switch it on | `usb_printer: 04a9:2676 …`, the interfaces and endpoints, `IEEE 1284 ID: …MDL:LBP2900…`, `driver canon-lbp2900`, `back channel: raw` | USB id, MDL, interface (stop) |
| 2 | wait 10 s; `curl http://<board>:8080/capt` | `status: idle …` with `st …` values; log: `printer info: id …, block …` | the request/reply link, GetPrinterInfo, status (stop) |
| 3 | double-press BOOT (test page) | one sheet: frame 10 mm from the edges, title, 11 gray steps, lines 1-8 dots, 10 mm grid | the page sequence, Hi-SCoA, page parameters (stop) |
| 4 | measure the printed frame | 10 mm ± 1 from each edge | image origin and margins |
| 5 | `tools/ipp_print.py http://<board>:631/ipp/print --pages 1` | sheet with "1"; the job log line `CAPT …: render … compress … printer …` | IPP path, 300 dpi upscaling |
| 6 | `… --pages 5`, then `--pages 5 --dpi 600` | 5 sheets in order, no pause longer than the engine's own; note the seconds per page | multi-page sequencing (captdriver #57 symptom) |
| 7 | repeat 6 with `curl -X POST '…:8080/capt?pipeline=1'` | same pages, faster | the faster page handover |
| 8 | print from Android (Settings → Connected devices → Printing → Default Print Service: the printer appears by name) | the document | mDNS discovery and PWG from Android |
| 9 | print from an iPhone (AirPrint) and from Windows (Add printer → it is found as an IPP printer) | the document | URF, Windows IPP class driver |
| 10 | `--pages 4` with 2 sheets in the tray | 2 sheets; the job waits; `ipp_print.py --status` and the web page say "Out of paper…"; load paper and **press the printer's button**; pages 3-4 print once each | paper-out detection and resume (stop if a page prints twice or the job hangs) |
| 11 | `--pages 6 --cancel-after 8` | the current sheet finishes, the job ends "canceled", the next job prints normally | cancel and the state after it |
| 12 | hold BOOT 2 s during a job | same as 11 | button cancel |
| 13 | pull the USB cable during page 2 of 5, plug back, print 1 page | job aborted with "USB link failed"; the next job prints (log: maybe `reserve refused … clearing`) | recovery without reboot |
| 14 | switch the printer off during a job, on again, print | as 13 | recovery after power loss |
| 15 | 30-page job at 600 dpi with photos (`--image photo.jpg --pages 30`) | all pages; `/info` heap stable; no task watchdog | long jobs, large pages, memory |
| 16 | 20 jobs of 1 page in a row | all print; the status returns to idle | no leak or stale state between jobs |
| 17 | `curl -X POST '…:8080/capt?density=4'` (and 10), print | lighter / darker | toner density byte |

Report for each model: the log of steps 1-3 with the trace on, the sheets
of steps 3-4 (a photo), and the timing lines of steps 6 and 7.

## 8. Not done yet

- **Real-hardware verification** of everything above.
- The web UI has the printer status, the print and scan pages, live state
  and Wi-Fi setup (`/wifi`; see the README). Job list and cancel go through
  IPP (any client, or `ipp_print.py`). The diagnostic log and OTA are on the
  debug server (`:8080`, `tools/remote.py`), not a page yet.
- The web UI's own print dialog still lays out pages for the inkjets'
  printable area. Pages print, but the edges may differ by about 1 mm.
- Paper sizes other than A4, Letter and A5. Envelopes and heavy paper (the
  media byte and fuser mode exist in the protocol, but the firmware does not
  offer them).
- Duplex by hand, and toner level (the printers do not report it).
