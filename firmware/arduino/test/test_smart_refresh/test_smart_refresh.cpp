// Unit tests for display_smart_refresh module
// Tests the SmartRefresh singleton for content change detection and region management

#include <unity.h>
#include <cstring>
#include <cstdint>
#include <cstdio>

// Minimal millis() stub for native testing
static uint32_t g_mock_millis = 0;
uint32_t millis() { return g_mock_millis; }
void set_mock_millis(uint32_t ms) { g_mock_millis = ms; }

// Include the implementation directly for native testing
// We need to redefine Arduino types first
typedef uint8_t byte;

// Now include the header and implementation
#include "../../src/display_smart_refresh.h"

// Re-implement SmartRefresh for native testing (since we can't link the .cpp directly)
// This mirrors the actual implementation for testing purposes

class TestableSmartRefresh {
public:
    static constexpr size_t MAX_REGIONS = 16;

    struct RegionState {
        uint8_t region_id;
        uint32_t content_hash;
        uint32_t last_update_ms;
        bool dirty;
    };

    void reset() {
        region_count_ = 0;
        stats_ = {};
        for (size_t i = 0; i < MAX_REGIONS; i++) {
            regions_[i] = {};
        }
    }

    void registerRegion(uint8_t region_id) {
        if (findRegion(region_id)) return;
        if (region_count_ >= MAX_REGIONS) return;

        regions_[region_count_].region_id = region_id;
        regions_[region_count_].content_hash = 0;
        regions_[region_count_].last_update_ms = 0;
        regions_[region_count_].dirty = true;
        region_count_++;
    }

    bool hasContentChanged(uint8_t region_id, const char* content) {
        stats_.total_checks++;

        RegionState* region = findRegion(region_id);
        if (!region) {
            stats_.actual_updates++;
            return true;
        }

        size_t len = content ? strlen(content) : 0;
        uint32_t new_hash = computeHash(content, len);

        if (new_hash != region->content_hash || region->dirty) {
            region->content_hash = new_hash;
            region->last_update_ms = millis();
            region->dirty = true;
            stats_.actual_updates++;
            return true;
        }

        stats_.skipped_updates++;
        return false;
    }

    bool hasContentChanged(uint8_t region_id, int32_t value) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%ld", (long)value);
        return hasContentChanged(region_id, buf);
    }

    bool hasContentChanged(uint8_t region_id, float value, int decimals = 1) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.*f", decimals, value);
        return hasContentChanged(region_id, buf);
    }

    void markDirty(uint8_t region_id) {
        RegionState* region = findRegion(region_id);
        if (region) {
            region->dirty = true;
        }
    }

    void markAllDirty() {
        for (size_t i = 0; i < region_count_; i++) {
            regions_[i].dirty = true;
        }
    }

    void markClean(uint8_t region_id) {
        RegionState* region = findRegion(region_id);
        if (region) {
            region->dirty = false;
        }
    }

    bool hasAnyDirty() const {
        for (size_t i = 0; i < region_count_; i++) {
            if (regions_[i].dirty) return true;
        }
        return false;
    }

    uint16_t getDirtyMask() const {
        uint16_t mask = 0;
        for (size_t i = 0; i < region_count_; i++) {
            if (regions_[i].dirty) {
                mask |= (1 << regions_[i].region_id);
            }
        }
        return mask;
    }

    struct Stats {
        uint32_t total_checks;
        uint32_t actual_updates;
        uint32_t skipped_updates;
    };

    const Stats& getStats() const { return stats_; }
    void resetStats() { stats_ = {}; }
    size_t getRegionCount() const { return region_count_; }

private:
    RegionState regions_[MAX_REGIONS];
    size_t region_count_ = 0;
    Stats stats_ = {};

    uint32_t computeHash(const char* data, size_t len) {
        // FNV-1a hash
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < len; i++) {
            hash ^= (uint8_t)data[i];
            hash *= 16777619u;
        }
        return hash;
    }

    RegionState* findRegion(uint8_t region_id) {
        for (size_t i = 0; i < region_count_; i++) {
            if (regions_[i].region_id == region_id) {
                return &regions_[i];
            }
        }
        return nullptr;
    }
};

// Global test instance
static TestableSmartRefresh g_refresh;

void setUp() {
    g_refresh.reset();
    set_mock_millis(0);
}

void tearDown() {
    // Nothing to clean up
}

// Test: Register a region
void test_register_region() {
    g_refresh.registerRegion(1);
    TEST_ASSERT_EQUAL(1, g_refresh.getRegionCount());

    // Registering same region again should not increase count
    g_refresh.registerRegion(1);
    TEST_ASSERT_EQUAL(1, g_refresh.getRegionCount());

    // Registering different region should increase count
    g_refresh.registerRegion(2);
    TEST_ASSERT_EQUAL(2, g_refresh.getRegionCount());
}

// Test: Max regions limit
void test_max_regions_limit() {
    for (size_t i = 0; i < TestableSmartRefresh::MAX_REGIONS; i++) {
        g_refresh.registerRegion(i);
    }
    TEST_ASSERT_EQUAL(TestableSmartRefresh::MAX_REGIONS, g_refresh.getRegionCount());

    // Attempting to register one more should not increase count
    g_refresh.registerRegion(99);
    TEST_ASSERT_EQUAL(TestableSmartRefresh::MAX_REGIONS, g_refresh.getRegionCount());
}

// Test: Content change detection with strings
void test_content_change_string() {
    g_refresh.registerRegion(1);
    g_refresh.markClean(1);  // Clear initial dirty state

    // First check with new content should return true
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(1, "72.5"));
    g_refresh.markClean(1);

    // Same content should return false
    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(1, "72.5"));

    // Different content should return true
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(1, "73.0"));
}

// Test: Content change detection with integers
void test_content_change_integer() {
    g_refresh.registerRegion(1);
    g_refresh.markClean(1);

    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(1, (int32_t)42));
    g_refresh.markClean(1);

    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(1, (int32_t)42));
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(1, (int32_t)43));
}

// Test: Content change detection with floats
void test_content_change_float() {
    g_refresh.registerRegion(1);
    g_refresh.markClean(1);

    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(1, 72.5f, 1));
    g_refresh.markClean(1);

    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(1, 72.5f, 1));

    // Same value with different precision
    g_refresh.registerRegion(2);
    g_refresh.markClean(2);
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(2, 72.50f, 2));
    g_refresh.markClean(2);
    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(2, 72.50f, 2));
}

// Test: Unregistered region always returns true
void test_unregistered_region() {
    // Region 99 is not registered
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(99, "test"));
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(99, "test"));  // Still true
}

// Test: Empty and null strings
void test_empty_null_strings() {
    g_refresh.registerRegion(1);
    g_refresh.markClean(1);

    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(1, ""));
    g_refresh.markClean(1);
    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(1, ""));

    // Null pointer
    g_refresh.registerRegion(2);
    g_refresh.markClean(2);
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(2, (const char*)nullptr));
    g_refresh.markClean(2);
    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(2, (const char*)nullptr));
}

// Test: Mark dirty forces update
void test_mark_dirty() {
    g_refresh.registerRegion(1);
    g_refresh.markClean(1);

    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(1, "test"));
    g_refresh.markClean(1);
    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(1, "test"));

    // Mark dirty and check again with same content
    g_refresh.markDirty(1);
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(1, "test"));
}

// Test: Mark all dirty
void test_mark_all_dirty() {
    g_refresh.registerRegion(1);
    g_refresh.registerRegion(2);
    g_refresh.registerRegion(3);

    // Set up with known content and clean
    g_refresh.hasContentChanged(1, "a");
    g_refresh.hasContentChanged(2, "b");
    g_refresh.hasContentChanged(3, "c");
    g_refresh.markClean(1);
    g_refresh.markClean(2);
    g_refresh.markClean(3);

    // All should be clean now
    TEST_ASSERT_FALSE(g_refresh.hasAnyDirty());

    // Mark all dirty
    g_refresh.markAllDirty();
    TEST_ASSERT_TRUE(g_refresh.hasAnyDirty());
}

// Test: Dirty mask calculation
void test_dirty_mask() {
    g_refresh.registerRegion(0);
    g_refresh.registerRegion(1);
    g_refresh.registerRegion(2);

    g_refresh.markClean(0);
    g_refresh.markClean(1);
    g_refresh.markClean(2);

    TEST_ASSERT_EQUAL_HEX16(0x0000, g_refresh.getDirtyMask());

    g_refresh.markDirty(0);
    TEST_ASSERT_EQUAL_HEX16(0x0001, g_refresh.getDirtyMask());

    g_refresh.markDirty(2);
    TEST_ASSERT_EQUAL_HEX16(0x0005, g_refresh.getDirtyMask());  // bits 0 and 2

    g_refresh.markDirty(1);
    TEST_ASSERT_EQUAL_HEX16(0x0007, g_refresh.getDirtyMask());  // bits 0, 1, and 2
}

// Test: Statistics tracking
void test_statistics() {
    g_refresh.registerRegion(1);
    g_refresh.markClean(1);

    const auto& stats_before = g_refresh.getStats();
    TEST_ASSERT_EQUAL_UINT32(0, stats_before.total_checks);

    g_refresh.hasContentChanged(1, "a");  // actual update
    g_refresh.markClean(1);
    g_refresh.hasContentChanged(1, "a");  // skipped
    g_refresh.hasContentChanged(1, "b");  // actual update

    const auto& stats = g_refresh.getStats();
    TEST_ASSERT_EQUAL_UINT32(3, stats.total_checks);
    TEST_ASSERT_EQUAL_UINT32(2, stats.actual_updates);
    TEST_ASSERT_EQUAL_UINT32(1, stats.skipped_updates);
}

// Test: Reset statistics
void test_reset_statistics() {
    g_refresh.registerRegion(1);
    g_refresh.hasContentChanged(1, "test");

    const auto& stats_before = g_refresh.getStats();
    TEST_ASSERT_GREATER_THAN(0, stats_before.total_checks);

    g_refresh.resetStats();

    const auto& stats_after = g_refresh.getStats();
    TEST_ASSERT_EQUAL_UINT32(0, stats_after.total_checks);
    TEST_ASSERT_EQUAL_UINT32(0, stats_after.actual_updates);
    TEST_ASSERT_EQUAL_UINT32(0, stats_after.skipped_updates);
}

// Test: Hash collision resistance (different strings should have different hashes)
void test_hash_collision_resistance() {
    g_refresh.registerRegion(1);

    // These strings are different and should not collide
    const char* strings[] = {
        "72", "73", "72.0", "72.1", "72.10", "72.01",
        "Temperature", "temperature", "TEMPERATURE",
        "0", "00", "000", "-0", "+0"
    };

    size_t num_strings = sizeof(strings) / sizeof(strings[0]);

    // A distinctive value used to reset the region between outer iterations.
    // Non-printable lead byte so it cannot equal any string under test.
    const char* kBaseline = "\x01<baseline>";

    for (size_t i = 0; i < num_strings; i++) {
        // markClean() clears the dirty flag but intentionally KEEPS the stored
        // content hash -- that is what lets an unchanged value be skipped. So the
        // assertion below is only about hashing if the region is first parked on a
        // value known to differ from strings[i]. Without this, state leaks between
        // outer iterations: after i=12 the stored hash is hash("+0"), and
        // strings[13] is "+0", so the check correctly reported "unchanged" and the
        // assertion failed on the implementation being right.
        g_refresh.markClean(1);
        g_refresh.hasContentChanged(1, kBaseline);
        g_refresh.markClean(1);

        TEST_ASSERT_TRUE_MESSAGE(g_refresh.hasContentChanged(1, strings[i]),
                                  "Change from baseline should be detected");

        for (size_t j = 0; j < num_strings; j++) {
            if (i != j) {
                g_refresh.markClean(1);
                // After setting content to strings[i], checking strings[j] should detect change
                g_refresh.hasContentChanged(1, strings[i]);
                g_refresh.markClean(1);
                TEST_ASSERT_TRUE_MESSAGE(g_refresh.hasContentChanged(1, strings[j]),
                                          "Different strings should not collide");
            }
        }
    }
}

// Test: Temperature display scenarios
void test_temperature_scenarios() {
    // Simulate typical temperature region behavior
    const uint8_t REGION_INSIDE_TEMP = 0;
    const uint8_t REGION_OUTSIDE_TEMP = 1;

    g_refresh.registerRegion(REGION_INSIDE_TEMP);
    g_refresh.registerRegion(REGION_OUTSIDE_TEMP);

    // Initial readings - both dirty
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(REGION_INSIDE_TEMP, 72.5f, 1));
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(REGION_OUTSIDE_TEMP, 65.0f, 1));

    g_refresh.markClean(REGION_INSIDE_TEMP);
    g_refresh.markClean(REGION_OUTSIDE_TEMP);

    // Same readings - no change
    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(REGION_INSIDE_TEMP, 72.5f, 1));
    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(REGION_OUTSIDE_TEMP, 65.0f, 1));

    // Only inside temp changes
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(REGION_INSIDE_TEMP, 73.0f, 1));
    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(REGION_OUTSIDE_TEMP, 65.0f, 1));

    // Verify only inside temp is dirty
    g_refresh.markClean(REGION_OUTSIDE_TEMP);
    TEST_ASSERT_EQUAL_HEX16(0x0001, g_refresh.getDirtyMask());
}

// Test: Rapid updates at same timestamp
void test_rapid_updates() {
    g_refresh.registerRegion(1);
    set_mock_millis(1000);

    // Rapid sequence of updates
    g_refresh.hasContentChanged(1, "a");
    g_refresh.markClean(1);
    g_refresh.hasContentChanged(1, "b");
    g_refresh.markClean(1);
    g_refresh.hasContentChanged(1, "c");
    g_refresh.markClean(1);

    // Final state should be 'c'
    TEST_ASSERT_FALSE(g_refresh.hasContentChanged(1, "c"));
    TEST_ASSERT_TRUE(g_refresh.hasContentChanged(1, "d"));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_register_region);
    RUN_TEST(test_max_regions_limit);
    RUN_TEST(test_content_change_string);
    RUN_TEST(test_content_change_integer);
    RUN_TEST(test_content_change_float);
    RUN_TEST(test_unregistered_region);
    RUN_TEST(test_empty_null_strings);
    RUN_TEST(test_mark_dirty);
    RUN_TEST(test_mark_all_dirty);
    RUN_TEST(test_dirty_mask);
    RUN_TEST(test_statistics);
    RUN_TEST(test_reset_statistics);
    RUN_TEST(test_hash_collision_resistance);
    RUN_TEST(test_temperature_scenarios);
    RUN_TEST(test_rapid_updates);

    return UNITY_END();
}
