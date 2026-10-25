# MASIF

**A 4-track sample tracker and rack sampler for the M-VAVE FM-1.** Free firmware, GPL-3.0-only.

You write songs on the FM-1 itself, in rows and columns: a song is a list of chains, a chain a list of phrases, a
phrase 16 rows of notes and effects. Every sound comes from you. MASIF ships with no sounds. You load samples from
its web page, or record them on the FM-1 (from the computer over USB, or a bounce of your own song).

- **Web page (sampler, sync, guide):** https://thecemeterygates.github.io/masif-fm1/
- **Download:** [Releases](https://github.com/thecemeterygates/masif-fm1/releases): the firmware package and the installer
- **How to install and use it:** the GUIDE tab on the web page

> **Install at your own risk.** Installing replaces the FM-1's firmware. Do not unplug the FM-1, switch it off or
> let the computer sleep while it installs: an install cut off at the wrong moment can leave the FM-1 unable to
> start, and then only a hardware flasher can bring it back. MASIF comes with no warranty (GPL-3.0, sections 15
> and 16). MASIF is independent: it is not made, endorsed or supported by M-VAVE. "M-VAVE" and "FM-1" are
> trademarks of their owners.

## What it does

- 4 tracks, 16 songs, 128 instruments with tables, grooves, modulation (envelopes, LFOs, a side chain), a filter,
  drive and EQ per instrument, sends to chorus, delay and reverb, a mixer and a project page.
- 24 samples, about 730 KB of sample memory: about 19 s at 26 kHz 12 bit, up to about 108 s at 13 kHz 4-bit ADPCM.
- A sampler page on the FM-1: records the computer's sound over USB, or MASIF's own mix.
- USB audio both ways (tested on macOS).
- The web page (Chrome or Edge, Web MIDI): drop in sound files or record from a mic, the line in or USB; trim,
  loop, fade and process them; tick the ones you want and SYNC POOL puts them on the FM-1. BACKUP and RESTORE
  everything MASIF keeps on the FM-1.

## Install

1. Download the latest release and unzip it.
2. In a terminal in that folder (once):

       python3 -m venv .venv
       . .venv/bin/activate          # Windows: .venv\Scripts\activate
       pip install mido python-rtmidi

3. FM-1 plugged in and switched on, other MIDI apps closed:

       python3 tools/fm1_install.py packages/masif-0.1.fwsc

Then open the web page, CONNECT, add samples and SYNC POOL. After every update, SYNC POOL again: an install
rewrites the part of the flash where most of the sample time lives.

## Building

See [BUILDING.md](BUILDING.md).

## Credits

MASIF is built on the work of others, under the same licence:

- **zp12** by zvenson: the firmware MASIF is built on (its voices, sample store, USB link). https://github.com/zvenson/zp12
- **SLOOP** by isod89: the FM-1 firmware zp12 builds on. https://github.com/isod89/sloop-fm1
- **Felucca** by Leo Kuroshita (Hügelton Instruments): the FM-1 platform under SLOOP, and the installer
  `tools/fm1_install.py`.
- **Ketch** by Adrien Risser-Zhuang: USB audio both ways (`firmware/src/usb.c`, `uo.h`, `uac_fir.h`).
  https://github.com/risseraka/fm1-tracker-ketch
- **NoteSorcery**: the computer's audio into the FM-1, by way of Ketch.
- The JieLi AC79 SDK's boot files (Apache-2.0) go into the package at build time; they are not in this tree.
- The web page's fonts: Orbitron, Audiowide and VT323 (SIL Open Font License, their licences beside them).

## Licence

GPL-3.0-only: see [LICENSE](LICENSE) and [LICENSING.md](LICENSING.md).
