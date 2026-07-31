#include "display_capture.h"

#if USE_DISPLAY

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <algorithm>
#include <cstdio>
#include "logging/logger.h"
#include "mqtt_client.h"

// Registered during static initialisation rather than inside getInstance().
// The singleton's constructor logs, and so does display_capture_handle() before
// it ever calls getInstance() -- with lazy registration both of those went out
// under module id 0 instead of "DispCap".
static const uint8_t log_module_id = Logger::getInstance().registerModule("DispCap");

// Base64 encoding table
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

DisplayCapture::DisplayCapture() {
  // Allocate GFXcanvas1 for shadow buffer
  // GFXcanvas1 uses 1 bit per pixel, perfect for eInk
  canvas_ = new GFXcanvas1(WIDTH, HEIGHT);
  if (canvas_) {
    canvas_->fillScreen(1);  // White background (1 = white for GFXcanvas1)
    LOGM_INFO("Screenshot canvas allocated: %dx%d (%d bytes)", WIDTH, HEIGHT, BUFFER_SIZE);
  } else {
    LOGM_ERROR("Failed to allocate screenshot canvas");
  }
}

DisplayCapture::~DisplayCapture() {
  if (canvas_) {
    delete canvas_;
    canvas_ = nullptr;
  }
}

DisplayCapture& DisplayCapture::getInstance() {
  static DisplayCapture instance;
  return instance;
}

const uint8_t* DisplayCapture::capture(size_t* out_size) {
  if (!out_size) {
    LOGM_ERROR("out_size is null");
    return nullptr;
  }

  if (!canvas_) {
    LOGM_ERROR("Canvas not initialized");
    *out_size = 0;
    return nullptr;
  }

  if (!has_content_) {
    LOGM_WARN("Canvas has no content - display may not have synced drawing");
  }

  // GFXcanvas1 stores buffer internally, we can access it via getBuffer()
  const uint8_t* buffer = canvas_->getBuffer();
  if (!buffer) {
    LOGM_ERROR("Canvas buffer is null");
    *out_size = 0;
    return nullptr;
  }

  *out_size = BUFFER_SIZE;
  LOGM_DEBUG("Captured %d bytes from canvas (%dx%d)", BUFFER_SIZE, WIDTH, HEIGHT);
  return buffer;
}

size_t DisplayCapture::captureBase64(char* out_buffer, size_t buffer_size) {
  if (!out_buffer || buffer_size < BASE64_SIZE) {
    LOGM_ERROR("Invalid output buffer (size=%d, need=%d)", buffer_size, BASE64_SIZE);
    return 0;
  }

  size_t size;
  const uint8_t* data = capture(&size);
  if (!data || size == 0) {
    LOGM_ERROR("Failed to capture display");
    return 0;
  }

  return base64Encode(data, size, out_buffer, buffer_size);
}

size_t DisplayCapture::base64Encode(const uint8_t* input, size_t input_len, char* output,
                                    size_t output_size) {
  size_t output_len = ((input_len + 2) / 3) * 4;

  if (output_size < output_len + 1) {
    LOGM_ERROR("Output buffer too small");
    return 0;
  }

  size_t i = 0, j = 0;
  uint8_t char_array_3[3];
  uint8_t char_array_4[4];

  while (input_len--) {
    char_array_3[i++] = *(input++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;

      for (i = 0; i < 4; i++) {
        output[j++] = base64_chars[char_array_4[i]];
      }
      i = 0;
    }
  }

  if (i) {
    for (size_t k = i; k < 3; k++) {
      char_array_3[k] = '\0';
    }

    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

    for (size_t k = 0; k < i + 1; k++) {
      output[j++] = base64_chars[char_array_4[k]];
    }

    while (i++ < 3) {
      output[j++] = '=';
    }
  }

  output[j] = '\0';
  return j;
}

// Global convenience functions
GFXcanvas1* display_capture_canvas() { return DisplayCapture::getInstance().getCanvas(); }

// C linkage for MQTT command handler
extern "C" void display_capture_handle(const char* payload, size_t length) {
  LOGM_INFO("Screenshot command received");

  DisplayCapture& cap = DisplayCapture::getInstance();

  if (!cap.hasContent()) {
    LOGM_WARN("No display content captured yet");
  }

  // Allocate buffer for base64 data on heap (it's large ~5KB)
  char* base64_buffer = new char[DisplayCapture::BASE64_SIZE];
  if (!base64_buffer) {
    LOGM_ERROR("Failed to allocate base64 buffer");
    return;
  }

  size_t base64_len = cap.captureBase64(base64_buffer, DisplayCapture::BASE64_SIZE);
  if (base64_len == 0) {
    LOGM_ERROR("Failed to capture and encode display");
    delete[] base64_buffer;
    return;
  }

  // Build JSON response. JsonDocument (ArduinoJson 7) replaces the deprecated
  // StaticJsonDocument<N>; it sizes itself, and this payload is five scalars.
  JsonDocument meta_doc;
  meta_doc["width"] = DisplayCapture::WIDTH;
  meta_doc["height"] = DisplayCapture::HEIGHT;
  meta_doc["format"] = "1bit";
  meta_doc["data_size"] = base64_len;
  meta_doc["buffer_size"] = DisplayCapture::BUFFER_SIZE;

  char meta_buffer[256];
  size_t meta_len = serializeJson(meta_doc, meta_buffer, sizeof(meta_buffer));

  // Publish metadata to /debug/screenshot/meta
  char topic[128];
  const char* client_id = mqtt_get_client_id();
  snprintf(topic, sizeof(topic), "espsensor/%s/debug/screenshot/meta", client_id);

  PubSubClient* client = mqtt_get_client();
  if (client && client->connected()) {
    client->publish(topic, (const uint8_t*)meta_buffer, meta_len, false);
    LOGM_INFO("Published screenshot metadata");

    // Publish base64 data to /debug/screenshot/data
    snprintf(topic, sizeof(topic), "espsensor/%s/debug/screenshot/data", client_id);

    // Must fit inside MQTT_MAX_PACKET_SIZE (1024) alongside the topic and the
    // ~10-byte MQTT header; the old 4096 exceeded the client buffer, so every
    // publish failed and the screenshot feature had never actually delivered a
    // frame. 768 leaves comfortable headroom -> a full frame is 7 chunks.
    const size_t CHUNK_SIZE = 768;
    size_t offset = 0;
    int chunk_num = 0;

    while (offset < base64_len) {
      size_t chunk_len = min(CHUNK_SIZE, base64_len - offset);

      if (base64_len > CHUNK_SIZE) {
        snprintf(topic, sizeof(topic), "espsensor/%s/debug/screenshot/data/%d", client_id,
                 chunk_num);
      }

      bool success =
          client->publish(topic, (const uint8_t*)(base64_buffer + offset), chunk_len, false);

      if (success) {
        LOGM_DEBUG("Published chunk %d (%d bytes, offset=%d)", chunk_num, chunk_len, offset);
      } else {
        LOGM_ERROR("Failed to publish chunk %d", chunk_num);
        break;
      }

      offset += chunk_len;
      chunk_num++;

      if (offset < base64_len) {
        delay(10);
      }
    }

    LOGM_INFO("Screenshot capture complete: %d bytes in %d chunks", base64_len, chunk_num);
  } else {
    LOGM_ERROR("MQTT client not connected");
  }

  delete[] base64_buffer;
}

#endif  // USE_DISPLAY
