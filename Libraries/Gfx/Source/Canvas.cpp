// SPDX-License-Identifier: Apache-2.0
#include <gfx/Canvas.h>

namespace gfx {

Canvas::Canvas(int width, int height, enum DisplayColorFormat format, const MemoryPolicy* policy)
    : buffer_(policy != nullptr
          ? pixel_buffer_create_with_policy(format, width, height, policy)
          : pixel_buffer_create(format, width, height)),
      surface_(buffer_) {
}

Canvas::~Canvas() {
    if (buffer_ != nullptr) {
        pixel_buffer_free(buffer_);
    }
}

} // namespace gfx
