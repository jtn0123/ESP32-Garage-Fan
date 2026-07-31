#pragma once

#include "config.h"
#if USE_DISPLAY

#include <Adafruit_GFX.h>

// DualGFX: Wrapper that forwards drawing operations to two GFX targets
// Used for screenshot capture - draws to both display and shadow canvas
//
// Usage:
//   DualGFX dual(&display, canvas);
//   dual.fillScreen(GxEPD_WHITE);
//   dual.print("Hello");  // Draws to both targets
//
// Global context:
//   set_dual_gfx_context(&dual);  // Before drawing
//   get_dual_gfx_context()->drawRect(...);  // In helper functions
//   set_dual_gfx_context(nullptr);  // After drawing
//
// Note: This wraps common operations used by the UI spec drawing code.
// Not all Adafruit_GFX methods are wrapped - add more as needed.

class DualGFX {
 public:
  explicit DualGFX(Adafruit_GFX* primary, Adafruit_GFX* secondary = nullptr)
      : primary_(primary), secondary_(secondary) {}

  void setSecondary(Adafruit_GFX* secondary) { secondary_ = secondary; }
  Adafruit_GFX* getPrimary() { return primary_; }
  Adafruit_GFX* getSecondary() { return secondary_; }

  // --- Drawing primitives ---

  void drawPixel(int16_t x, int16_t y, uint16_t color) {
    primary_->drawPixel(x, y, color);
    if (secondary_)
      secondary_->drawPixel(x, y, mapColor(color));
  }

  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    primary_->drawLine(x0, y0, x1, y1, color);
    if (secondary_)
      secondary_->drawLine(x0, y0, x1, y1, mapColor(color));
  }

  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    primary_->drawRect(x, y, w, h, color);
    if (secondary_)
      secondary_->drawRect(x, y, w, h, mapColor(color));
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    primary_->fillRect(x, y, w, h, color);
    if (secondary_)
      secondary_->fillRect(x, y, w, h, mapColor(color));
  }

  void fillScreen(uint16_t color) {
    primary_->fillScreen(color);
    if (secondary_)
      secondary_->fillScreen(mapColor(color));
  }

  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
    primary_->drawCircle(x, y, r, color);
    if (secondary_)
      secondary_->drawCircle(x, y, r, mapColor(color));
  }

  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
    primary_->fillCircle(x, y, r, color);
    if (secondary_)
      secondary_->fillCircle(x, y, r, mapColor(color));
  }

  // --- Bitmap drawing ---

  void drawBitmap(int16_t x, int16_t y, const uint8_t* bitmap, int16_t w, int16_t h,
                  uint16_t color) {
    primary_->drawBitmap(x, y, bitmap, w, h, color);
    if (secondary_)
      secondary_->drawBitmap(x, y, bitmap, w, h, mapColor(color));
  }

  void drawBitmap(int16_t x, int16_t y, const uint8_t* bitmap, int16_t w, int16_t h, uint16_t color,
                  uint16_t bg) {
    primary_->drawBitmap(x, y, bitmap, w, h, color, bg);
    if (secondary_)
      secondary_->drawBitmap(x, y, bitmap, w, h, mapColor(color), mapColor(bg));
  }

  // --- Text operations ---

  void setCursor(int16_t x, int16_t y) {
    primary_->setCursor(x, y);
    if (secondary_)
      secondary_->setCursor(x, y);
  }

  void setTextColor(uint16_t color) {
    primary_->setTextColor(color);
    if (secondary_)
      secondary_->setTextColor(mapColor(color));
  }

  void setTextColor(uint16_t color, uint16_t bg) {
    primary_->setTextColor(color, bg);
    if (secondary_)
      secondary_->setTextColor(mapColor(color), mapColor(bg));
  }

  void setTextSize(uint8_t size) {
    primary_->setTextSize(size);
    if (secondary_)
      secondary_->setTextSize(size);
  }

  void setTextWrap(bool wrap) {
    primary_->setTextWrap(wrap);
    if (secondary_)
      secondary_->setTextWrap(wrap);
  }

  void setFont(const GFXfont* font = nullptr) {
    primary_->setFont(font);
    if (secondary_)
      secondary_->setFont(font);
  }

  size_t print(const char* str) {
    size_t n = primary_->print(str);
    if (secondary_)
      secondary_->print(str);
    return n;
  }

  size_t print(const String& str) { return print(str.c_str()); }

  size_t print(char c) {
    size_t n = primary_->print(c);
    if (secondary_)
      secondary_->print(c);
    return n;
  }

  size_t print(int val, int base = DEC) {
    size_t n = primary_->print(val, base);
    if (secondary_)
      secondary_->print(val, base);
    return n;
  }

  size_t println(const char* str = "") {
    size_t n = primary_->println(str);
    if (secondary_)
      secondary_->println(str);
    return n;
  }

  // --- Dimensions ---

  int16_t width() const { return primary_->width(); }
  int16_t height() const { return primary_->height(); }
  int16_t getCursorX() const { return primary_->getCursorX(); }
  int16_t getCursorY() const { return primary_->getCursorY(); }

  void getTextBounds(const char* str, int16_t x, int16_t y, int16_t* x1, int16_t* y1, uint16_t* w,
                     uint16_t* h) {
    primary_->getTextBounds(str, x, y, x1, y1, w, h);
  }

 private:
  Adafruit_GFX* primary_;
  Adafruit_GFX* secondary_;

  // Map GxEPD2 colors to the capture canvas convention: 0 = ink (black),
  // 1 = background (white). This MUST agree with everyone else touching the
  // canvas — DisplayCapture's constructor fills it with 1s as "white", and the
  // DUAL_* macros in display_renderer map black to 0. This mapper used to be
  // inverted (black -> 1), so every stroke drawn through DualGFX wrote 1-on-1
  // and vanished: captures showed only the elements drawn by the DUAL_* /
  // draw_icon paths, which is why remote screenshots were missing the frame,
  // header, labels and all small text.
  static uint16_t mapColor(uint16_t color) { return (color == 0x0000) ? 0 : 1; }
};

// Global drawing context for screenshot capture
// Set this before drawing, helper functions will use it
void set_dual_gfx_context(DualGFX* ctx);
DualGFX* get_dual_gfx_context();

// RAII helper to set/restore context
class DualGFXScope {
 public:
  explicit DualGFXScope(DualGFX* ctx) : prev_(get_dual_gfx_context()) { set_dual_gfx_context(ctx); }
  ~DualGFXScope() { set_dual_gfx_context(prev_); }

 private:
  DualGFX* prev_;
};

#endif  // USE_DISPLAY
