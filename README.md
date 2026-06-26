xf86-video-tdfx — 3Dfx Voodoo driver for Xorg
========================================================

> **This is a fork** of the upstream Xorg `xf86-video-tdfx` driver
> (https://gitlab.freedesktop.org/xorg/driver/xf86-video-tdfx).
> It targets **3dfx Voodoo3 / Voodoo4 / Voodoo5 (VSA‑100 "Napalm")**
> hardware on a current X server, and adds two things upstream no longer
> provides: a clean dot‑clock path for the VSA‑100, and **2D hardware
> acceleration** (which the upstream driver lost when XAA was removed).

What this fork changes
----------------------

### 1. VSA‑100 PLL / dot‑clock fix

Upstream `CalcPLL()` free‑searches the PLL `m`/`k` coefficients to minimise
frequency error, with **no constraint on the VCO band**. On VSA‑100 that picks
unstable VCO operating points, so the pixel clock jitters — visible as a shaky
VGA image and, on cards that drive a digital/TMDS encoder, the HDMI/DVI sink
never locks.

The fix matches 3dfx's own clock algorithm: pin `m`/`k`, sweep `n`, and keep the
VCO in a high, stable band (clamped to a sane MHz window). `TDFX2XCUTOFF` is
also raised so a 148.5 MHz 1080p dot clock uses a single clock with no DAC 2×
path. Result: **solid native 1920×1080** (clean VGA + stable HDMI), with
the programmed PLL register now byte‑identical to the Windows driver.

### 2. EXA 2D acceleration

The upstream driver accelerated 2D through **XAA**. XAA was removed from the
X server back in **2012**, and the dead XAA code was subsequently stripped from
this driver — leaving the Voodoo with **no 2D acceleration at all** (every fill,
scroll and window copy fell back to software).

This fork re‑implements the VSA‑100 2D command‑FIFO sequences against **EXA**
(which the X server still ships):

* `PrepareSolid` / `Solid`  → hardware rectangle fill
* `PrepareCopy` / `Copy`    → hardware screen‑to‑screen blit (both directions)
* `WaitMarker`              → 2D engine sync

Acceleration targets the visible front buffer only: the 2D engine's
source/destination base address and pixel format are programmed per
operation from each pixmap's EXA offset/pitch.  The EXA offscreen pool is
left empty on purpose -- the VSA-100 2D engine can blt host->screen but has
no screen->host blt (confirmed in the VSA-100 databook), so a pixmap in VRAM
can only be read back over the very slow framebuffer aperture by the CPU.

New code lives in `src/tdfx_exa.c`; the driver now loads the `exa` sub‑module and calls
`TDFXExaInit()` / `TDFXExaFini()`.

Benchmarks
----------

`x11perf`, VSA‑100 @ 1920×1080×24, driver with **no acceleration**
(PLL fix only) vs **(EXA)**.

> All numbers were measured on this test system's CPU — an Intel Core
> i7‑4800MQ (2.7 GHz, Haswell). The no‑accel baseline is **CPU‑bound** (pure
> software rendering); EXA offloads to the card's fixed‑function 2D engine. On a
> slower, period‑appropriate CPU the speedups would be **larger**, not smaller —
> the software baseline scales with CPU speed, the 2D engine does not.

| Operation (x11perf)        | no‑accel |     EXA |  speedup |
|----------------------------|---------:|--------:|---------:|
| Rectangle fill 100×100     |  4,350/s | 33,600/s |  **7.7×** |
| Rectangle fill 500×500     |    183/s |  1,190/s |  **6.5×** |
| Filled circle (100px)      |  5,490/s | 26,600/s |  **4.8×** |
| Filled ellipse (100px)     | 10,500/s | 46,900/s |  **4.5×** |
| Fill trapezoid 100×100     |  4,370/s | 21,200/s |  **4.9×** |
| Scroll 100×100             |    203/s | 10,300/s |   **51×** |
| Scroll 500×500             |    8.5/s |   553/s  |   **65×** |
| Copy window→window 100×100 |    200/s | 10,400/s |   **52×** |
| Copy window→window 500×500 |    8.6/s |   555/s  |   **65×** |

Operations EXA does not accelerate (lines, pattern/stipple fills, bitmap text,
image up/download) are unchanged.

The VSA‑100 2D engine is **memory‑bandwidth bound** when running at 1080p:
CRTC scanout competes with the blitter for VRAM bandwidth, so the same
accelerated operations run ~1.5× faster at a lower display resolution
(and the speedup over software grows):

| Operation (EXA)            |  @1080p | @800×600 | resolution gain |
|----------------------------|--------:|---------:|----------------:|
| Rectangle fill 100×100     | 33,600/s| 54,100/s |          1.61×  |
| Filled ellipse (100px)     | 46,900/s| 72,000/s |          1.54×  |
| Scroll 500×500             |   553/s |   823/s  |          1.49×  |
| Copy window→window 500×500 |   555/s |   826/s  |          1.49×  |

### Desktop benchmark

A scripted `xdotool` window‑drag on a live Xfce session, no‑accel vs EXA, with
the **xfwm4 compositor both ON and OFF**. Values are the **mean of 10 runs**
(window moves/sec; ± is run‑to‑run coefficient of variation):

|              |  drag, comp OFF | drag, comp ON  |
|--------------|----------------:|---------------:|
| no‑accel     |   13/s  (±0%)   |  507/s  (±29%) |
| **EXA**      |  206/s  (±8%)   |  705/s  (±34%) |
| **EXA gain** |    **~16×**     |    **~1.4×**   |


Building
--------

    autoreconf -fi
    ./configure
    make
    sudo make install

Requires the Xorg driver SDK and the EXA headers (`xserver-xorg-dev` on Debian/
Ubuntu). Enable/disable acceleration with the existing `Option "NoAccel"`.

Status & limitations
--------------------

* Stable: validated with the full `x11perf` 2D suite at 1080p and 800×600
  (no crashes), driving an Xfce desktop on Debian 13.
* The 1080p PLL fix is exact; other modes may still need clock tuning on
  digital‑output cards.
* 3D / OpenGL is **not** accelerated — the old DRI1 + Mesa `tdfx` stack
  was removed from the kernel and Mesa years ago.

Upstream
--------

Original driver, bug reports for unmodified behaviour, and patch‑submission
guidelines:

* https://gitlab.freedesktop.org/xorg/driver/xf86-video-tdfx
* https://www.x.org/wiki/Development/Documentation/SubmittingPatches
