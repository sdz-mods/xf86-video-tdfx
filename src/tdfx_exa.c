/*
 * tdfx_exa.c - EXA 2D acceleration for the 3dfx Voodoo (VSA-100) 2D engine.
 *
 * Re-implements the historical XAA solid-fill and screen-to-screen-copy
 * command-FIFO sequences against EXA (XAA was removed from xserver in 2012).
 * Acceleration targets the visible front buffer only: the 2D engine's
 * source/destination base address and pixel format are programmed per
 * operation from each pixmap's EXA offset/pitch.  The EXA offscreen pool is
 * left empty on purpose -- the VSA-100 2D engine can blt host->screen but has
 * no screen->host blt (confirmed in the VSA-100 databook), so a pixmap in VRAM
 * can only be read back over the very slow framebuffer aperture by the CPU.
 * With only Solid+Copy accelerated, every other draw (text, RENDER) is software
 * and would force such a readback, thrashing the engine and stalling the
 * desktop.  Front-buffer-only sidesteps that entirely (see TDFXExaInit).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include "xf86.h"
#include "xf86_OSproc.h"
#include "exa.h"
#include "tdfx.h"

#ifndef EXA_PM_IS_SOLID
#define EXA_PM_IS_SOLID(_pDraw, _pm) \
    (((_pm) & ((((Pixel)1) << (_pDraw)->depth) - 1)) == \
     ((((Pixel)1) << (_pDraw)->depth) - 1))
#endif

/* GX raster op -> tdfx ROP byte (plain 16 ROPs) */
static const int TDFXExaROP[16] = {
    0x00, 0x88, 0x44, 0xCC, 0x22, 0xAA, 0x66, 0xEE,
    0x11, 0x99, 0x55, 0xDD, 0x33, 0xBB, 0x77, 0xFF
};

/* linear 2D surface format word: pixel-format code in bits[16..], byte
 * pitch in the low bits (code 3 = 16bpp, 5 = 32bpp, via cpp+1) */
static int
TDFXExaFmt(TDFXPtr pTDFX, int pitch)
{
    if (pTDFX->cpp == 1)
        return (1 << 16) | pitch;
    return ((pTDFX->cpp + 1) << 16) | pitch;
}

/* card-relative byte offset of a pixmap for the 2D engine */
static int
TDFXExaOffset(TDFXPtr pTDFX, PixmapPtr pPix)
{
    return pTDFX->fbOffset + exaGetPixmapOffset(pPix);
}

/* keep COMMANDEXTRA / clip in a known (plain copy/fill) state */
static void
TDFXExaSetupState(ScrnInfoPtr pScrn)
{
    TDFXPtr pTDFX = TDFXPTR(pScrn);

    pTDFX->Cmd = 0;
    pTDFX->DrawState &= ~DRAW_STATE_TRANSPARENT;
    TDFXFirstSync(pScrn);

    pTDFX->Cmd &= ~SST_2D_USECLIP1;
    TDFXMakeRoom(pTDFX, 1);
    DECLARE(SSTCP_COMMANDEXTRA);
    TDFXWriteLong(pTDFX, SST_2D_COMMANDEXTRA, 0);
    pTDFX->PrevDrawState = pTDFX->DrawState;
}

/* ------------------------------- Solid fill ----------------------------- */

static Bool
TDFXExaPrepareSolid(PixmapPtr pPix, int alu, Pixel planemask, Pixel fg)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPix->drawable.pScreen);
    TDFXPtr pTDFX = TDFXPTR(pScrn);
    int fmt, off;

    if (!EXA_PM_IS_SOLID(&pPix->drawable, planemask))
        return FALSE;

    TDFXExaSetupState(pScrn);
    pTDFX->Cmd = TDFXExaROP[alu & 0xF] << 24;
    fmt = TDFXExaFmt(pTDFX, exaGetPixmapPitch(pPix));
    off = TDFXExaOffset(pTDFX, pPix);

    TDFXMakeRoom(pTDFX, 4);
    DECLARE(SSTCP_DSTBASEADDR | SSTCP_DSTFORMAT |
            SSTCP_COLORFORE | SSTCP_COLORBACK);
    TDFXWriteLong(pTDFX, SST_2D_DSTBASEADDR, off);
    TDFXWriteLong(pTDFX, SST_2D_DSTFORMAT, fmt);
    pTDFX->sst2DDstFmtShadow = fmt;
    TDFXWriteLong(pTDFX, SST_2D_COLORBACK, fg);
    TDFXWriteLong(pTDFX, SST_2D_COLORFORE, fg);
    return TRUE;
}

static void
TDFXExaSolid(PixmapPtr pPix, int x1, int y1, int x2, int y2)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPix->drawable.pScreen);
    TDFXPtr pTDFX = TDFXPTR(pScrn);
    int w = x2 - x1, h = y2 - y1;

    TDFXMakeRoom(pTDFX, 3);
    DECLARE(SSTCP_DSTSIZE | SSTCP_DSTXY | SSTCP_COMMAND);
    TDFXWriteLong(pTDFX, SST_2D_DSTSIZE, ((h & 0x1FFF) << 16) | (w & 0x1FFF));
    TDFXWriteLong(pTDFX, SST_2D_DSTXY, ((y1 & 0x1FFF) << 16) | (x1 & 0x1FFF));
    TDFXWriteLong(pTDFX, SST_2D_COMMAND,
                  pTDFX->Cmd | SST_2D_RECTANGLEFILL | SST_2D_GO);
}

static void
TDFXExaDoneSolid(PixmapPtr pPix)
{
}

/* --------------------------- Screen->screen copy ------------------------ */

static Bool
TDFXExaPrepareCopy(PixmapPtr pSrc, PixmapPtr pDst, int dx, int dy,
                   int alu, Pixel planemask)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDst->drawable.pScreen);
    TDFXPtr pTDFX = TDFXPTR(pScrn);
    int srcFmt, dstFmt, srcOff, dstOff;

    if (!EXA_PM_IS_SOLID(&pDst->drawable, planemask))
        return FALSE;

    TDFXExaSetupState(pScrn);
    pTDFX->Cmd = (TDFXExaROP[alu & 0xF] << 24) | SST_2D_SCRNTOSCRNBLIT;
    if (dx < 0)
        pTDFX->Cmd |= SST_2D_X_RIGHT_TO_LEFT;
    if (dy < 0)
        pTDFX->Cmd |= SST_2D_Y_BOTTOM_TO_TOP;

    srcFmt = TDFXExaFmt(pTDFX, exaGetPixmapPitch(pSrc));
    dstFmt = TDFXExaFmt(pTDFX, exaGetPixmapPitch(pDst));
    srcOff = TDFXExaOffset(pTDFX, pSrc);
    dstOff = TDFXExaOffset(pTDFX, pDst);

    TDFXMakeRoom(pTDFX, 4);
    DECLARE(SSTCP_DSTBASEADDR | SSTCP_DSTFORMAT |
            SSTCP_SRCBASEADDR | SSTCP_SRCFORMAT);
    TDFXWriteLong(pTDFX, SST_2D_DSTBASEADDR, dstOff);
    TDFXWriteLong(pTDFX, SST_2D_DSTFORMAT, dstFmt);
    pTDFX->sst2DDstFmtShadow = dstFmt;
    TDFXWriteLong(pTDFX, SST_2D_SRCBASEADDR, srcOff);
    TDFXWriteLong(pTDFX, SST_2D_SRCFORMAT, srcFmt);
    pTDFX->sst2DSrcFmtShadow = srcFmt;
    return TRUE;
}

static void
TDFXExaCopy(PixmapPtr pDst, int srcX, int srcY, int dstX, int dstY,
            int w, int h)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDst->drawable.pScreen);
    TDFXPtr pTDFX = TDFXPTR(pScrn);

    if (pTDFX->Cmd & SST_2D_Y_BOTTOM_TO_TOP) {
        srcY += h - 1;
        dstY += h - 1;
    }
    if (pTDFX->Cmd & SST_2D_X_RIGHT_TO_LEFT) {
        srcX += w - 1;
        dstX += w - 1;
    }
    pTDFX->sync(pScrn);

    TDFXMakeRoom(pTDFX, 4);
    DECLARE(SSTCP_SRCXY | SSTCP_DSTSIZE | SSTCP_DSTXY | SSTCP_COMMAND);
    TDFXWriteLong(pTDFX, SST_2D_SRCXY,
                  (srcX & 0x1FFF) | ((srcY & 0x1FFF) << 16));
    TDFXWriteLong(pTDFX, SST_2D_DSTSIZE,
                  (w & 0x1FFF) | ((h & 0x1FFF) << 16));
    TDFXWriteLong(pTDFX, SST_2D_DSTXY,
                  (dstX & 0x1FFF) | ((dstY & 0x1FFF) << 16));
    TDFXWriteLong(pTDFX, SST_2D_COMMAND, pTDFX->Cmd | SST_2D_GO);

    pTDFX->prevBlitDest.y1 = dstY;
}

static void
TDFXExaDoneCopy(PixmapPtr pDst)
{
}

/* --------------------------------- sync --------------------------------- */

static void
TDFXExaWaitMarker(ScreenPtr pScreen, int marker)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TDFXPtr pTDFX = TDFXPTR(pScrn);

    pTDFX->sync(pScrn);
}

/* --------------------------------- init --------------------------------- */

Bool
TDFXExaInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TDFXPtr pTDFX = TDFXPTR(pScrn);
    ExaDriverPtr exa;

    exa = exaDriverAlloc();
    if (!exa)
        return FALSE;
    pTDFX->exa = exa;

    exa->exa_major = EXA_VERSION_MAJOR;
    exa->exa_minor = EXA_VERSION_MINOR;

    /* Front buffer lives at FbBase + fbOffset (what fbScreenInit got).  Cap the
     * region EXA knows about to real VRAM -- the mapping aperture can be 2x the
     * installed RAM. */
    exa->memoryBase = (CARD8 *) pTDFX->FbBase + pTDFX->fbOffset;
    {
        long vram = (long) pScrn->videoRam * 1024;
        if (vram > 0 && vram < pTDFX->FbMapSize)
            exa->memorySize = vram - pTDFX->fbOffset;
        else
            exa->memorySize = pTDFX->FbMapSize - pTDFX->fbOffset;
    }
    /* Empty offscreen pool -- front-buffer acceleration only. The VSA-100
     * has no screen->host blt, so VRAM pixmaps can only be read back by
     * slow CPU aperture reads. */
    exa->offScreenBase = exa->memorySize;

    exa->pixmapOffsetAlign = 32;
    exa->pixmapPitchAlign = 32;
    exa->maxX = 2048;
    exa->maxY = 2048;
    exa->flags = EXA_OFFSCREEN_PIXMAPS;

    exa->WaitMarker = TDFXExaWaitMarker;
    exa->PrepareSolid = TDFXExaPrepareSolid;
    exa->Solid = TDFXExaSolid;
    exa->DoneSolid = TDFXExaDoneSolid;
    exa->PrepareCopy = TDFXExaPrepareCopy;
    exa->Copy = TDFXExaCopy;
    exa->DoneCopy = TDFXExaDoneCopy;

    /* initial 2D-engine state (per-op base/format is set in Prepare*) */
    pTDFX->PciCnt = TDFXReadLongMMIO(pTDFX, 0) & 0x1F;
    pTDFX->PrevDrawState = pTDFX->DrawState = 0;
    pTDFX->ModeReg.srcbaseaddr = pTDFX->fbOffset;
    TDFXWriteLongMMIO(pTDFX, SST_2D_SRCBASEADDR, pTDFX->ModeReg.srcbaseaddr);
    pTDFX->ModeReg.dstbaseaddr = pTDFX->fbOffset;
    TDFXWriteLongMMIO(pTDFX, SST_2D_DSTBASEADDR, pTDFX->ModeReg.dstbaseaddr);
    pTDFX->sst2DSrcFmtShadow = TDFXReadLongMMIO(pTDFX, SST_2D_SRCFORMAT);
    pTDFX->sst2DDstFmtShadow = TDFXReadLongMMIO(pTDFX, SST_2D_DSTFORMAT);

    if (!exaDriverInit(pScreen, exa)) {
        free(exa);
        pTDFX->exa = NULL;
        return FALSE;
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "EXA: VSA-100 2D acceleration (solid fill + copy) enabled\n");
    return TRUE;
}

void
TDFXExaFini(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    TDFXPtr pTDFX = TDFXPTR(pScrn);

    if (pTDFX->exa) {
        exaDriverFini(pScreen);
        free(pTDFX->exa);
        pTDFX->exa = NULL;
    }
}
