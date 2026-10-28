// SPDX-License-Identifier: Apache-2.0
#pragma once

/**
 * gfx - direct-to-display rendering for Tactility apps.
 *
 * Draw into a gfx::Surface inside a gfx::Target::render() callback; the target decides whether that
 * is a hardware frame buffer flip, a dirty-region update, or a band-by-band render. The same app code
 * then works on MIPI DSI, RGB, SPI and e-paper panels, with or without PSRAM.
 *
 * A gfx::Surface can also wrap any PixelBuffer directly, which makes the drawing primitives and
 * text usable off-screen (sprites, canvases, or panels driven outside Tactility's display API).
 */

#include "Canvas.h"
#include "DirtyRegions.h"
#include "DisplaySink.h"
#include "Font.h"
#include "Rect.h"
#include "Sink.h"
#include "Surface.h"
#include "Target.h"
#include "Text.h"
