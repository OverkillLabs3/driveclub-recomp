#pragma once
#include <math.h>
#include <stddef.h>
#include <stdio.h>

/* Shared with the host test so the test covers the overlay's actual display
 * conversion, including reverse and history-boundary rates. */
static inline void bbport_scene_compression_text(char *text, size_t size,
                                                float factor, int live) {
  /* Negative values are meaningful only in a live signed clock sample; the
   * configured fallback uses -1 to mean that no valid setting was read. */
  if (!isfinite(factor) || (!live && factor < 0))
    snprintf(text, size, "Time Compression: --");
  else
    snprintf(text, size, "Time Compression: %.6gx", factor == 0 ? 0.0 : (double)factor);
}
