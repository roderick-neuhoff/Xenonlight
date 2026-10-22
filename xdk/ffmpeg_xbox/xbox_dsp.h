/*
 * Xbox 360 (Xenon VMX) DSP functions for XBMC-360's FFmpeg 1.2, installed by
 * hooks at the end of FFmpeg's own init functions (see build_ffmpeg.py).
 * Compiled as part of avcodec.lib; the app only uses the functions below.
 */
#ifndef XBOX_DSP_H
#define XBOX_DSP_H

/* Groups of functions the app can switch on (ff_xbox_vmx_flags). */
#define XVMX_QPEL     0x01   /* luma motion compensation (incl. full-pel copy) */
#define XVMX_CHROMA   0x02   /* chroma motion compensation */
#define XVMX_PREFETCH 0x04   /* dcbt prefetch of reference rows (XBMC-360) */
#define XVMX_IDCT     0x08   /* 4x4 and 8x8 inverse transform + add, DC-only add */
#define XVMX_ALL      0x0F

#ifdef __cplusplus
extern "C" {
#endif

/* Which groups the init hooks install. Change only while no decoder is open. */
extern int ff_xbox_vmx_flags;

/*
 * Compares every VMX function with FFmpeg's C version on random data and
 * marks the ones that match; only those are ever installed. Must run once
 * before the first decoder is opened. log() receives one line per mismatch
 * and a summary. Returns the number of mismatching functions.
 */
int ff_xbox_dsp_selftest(void (*log)(const char *line));

#ifdef __cplusplus
}
#endif

#endif
