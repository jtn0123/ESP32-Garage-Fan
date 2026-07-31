#pragma once

#include "config.h"
#if USE_DISPLAY

#include <Arduino.h>
#include <cstdint>
#include <Adafruit_GFX.h>

// Display framebuffer capture for remote debugging and monitoring
// Uses GFXcanvas1 as a shadow buffer that mirrors all display drawing operations.
//
// The canvas is exposed via display_capture_canvas() and can be used alongside
// the main display for all drawing operations. After drawing completes, the
// canvas buffer is used for screenshots.
//
// Usage in drawing code:
//   GFXcanvas1* canvas = display_capture_canvas();
//   if (canvas) {
//     canvas->fillScreen(GxEPD_WHITE);
//     canvas->print("Hello");
//     // ... draw to canvas the same as display ...
//   }
//
// For screenshot:
//   DisplayCapture& cap = DisplayCapture::getInstance();
//   size_t size;
//   const uint8_t* buffer = cap.capture(&size);

class DisplayCapture {
 public:
  static DisplayCapture& getInstance();

  // Get the canvas for drawing (mirrors display operations)
  GFXcanvas1* getCanvas() { return canvas_; }

  // Capture current canvas to buffer (1-bit packed)
  // Returns pointer to canvas buffer and sets size
  const uint8_t* capture(size_t* out_size);

  // Get as base64 string (for MQTT transmission)
  // Returns length of base64 string, 0 on error
  // out_buffer must be at least BASE64_SIZE bytes
  size_t captureBase64(char* out_buffer, size_t buffer_size);

  // Check if canvas has been initialized and drawn to
  bool hasContent() const { return canvas_ != nullptr && has_content_; }

  // Mark that content has been drawn
  void setHasContent() { has_content_ = true; }

  // Display dimensions (250x122 for 2.13" eInk)
  static constexpr uint16_t WIDTH = 250;
  static constexpr uint16_t HEIGHT = 122;

  // Buffer sizes (WIDTH must be padded to byte boundary for GFXcanvas1)
  static constexpr uint16_t WIDTH_BYTES = (WIDTH + 7) / 8;                // 32 bytes per row
  static constexpr size_t BUFFER_SIZE = WIDTH_BYTES * HEIGHT;             // 3904 bytes
  static constexpr size_t BASE64_SIZE = ((BUFFER_SIZE + 2) / 3) * 4 + 1;  // ~5206 bytes

 private:
  DisplayCapture();
  ~DisplayCapture();
  DisplayCapture(const DisplayCapture&) = delete;
  DisplayCapture& operator=(const DisplayCapture&) = delete;

  GFXcanvas1* canvas_ = nullptr;
  bool has_content_ = false;

  // Base64 encoding helper
  size_t base64Encode(const uint8_t* input, size_t input_len, char* output, size_t output_size);
};

// C linkage for MQTT command handler
extern "C" void display_capture_handle(const char* payload, size_t length);

// Get the shadow canvas for drawing operations
// Returns nullptr if capture system not initialized
GFXcanvas1* display_capture_canvas();

#endif  // USE_DISPLAY
