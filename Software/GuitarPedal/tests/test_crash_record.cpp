// Host tests for Util/crash_record.h.
#include "../Util/crash_record.h"

#include <cstdio>
#include <cstring>

using namespace bkshepherd;

static int failures = 0;
#define CHECK(cond)                                                                                                            \
    do {                                                                                                                       \
        if (!(cond)) {                                                                                                         \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                                                         \
            ++failures;                                                                                                        \
        }                                                                                                                      \
    } while (0)

static void test_zeroed_record_is_invalid() {
    CrashRecord r;
    std::memset(&r, 0, sizeof(r));
    CHECK(CrashRecordIsValid(r) == false);
}

static void test_filled_record_is_valid_and_keeps_fields() {
    CrashRecord r;
    CrashRecordFill(r, 0x24001234u, 0x24000abcu, 0x00008200u, 7, 123456u);
    CHECK(CrashRecordIsValid(r) == true);
    CHECK(r.magic == kCrashRecordMagic);
    CHECK(r.pc == 0x24001234u);
    CHECK(r.lr == 0x24000abcu);
    CHECK(r.cfsr == 0x00008200u);
    CHECK(r.effectID == 7);
    CHECK(r.uptimeMs == 123456u);
}

static void test_corrupted_field_invalidates() {
    CrashRecord r;
    CrashRecordFill(r, 1u, 2u, 3u, 4, 5u);
    r.pc ^= 0x1u;
    CHECK(CrashRecordIsValid(r) == false);
}

static void test_wrong_magic_invalidates() {
    CrashRecord r;
    CrashRecordFill(r, 1u, 2u, 3u, 4, 5u);
    r.magic = 0x12345678u;
    r.checksum = CrashRecordChecksum(r); // even with a consistent checksum, wrong magic fails
    CHECK(CrashRecordIsValid(r) == false);
}

static void test_clear_invalidates() {
    CrashRecord r;
    CrashRecordFill(r, 1u, 2u, 3u, 4, 5u);
    CrashRecordClear(r);
    CHECK(CrashRecordIsValid(r) == false);
    CHECK(r.magic == 0u);
}

static void test_layout_is_28_bytes() { CHECK(sizeof(CrashRecord) == 28); }

int main() {
    test_zeroed_record_is_invalid();
    test_filled_record_is_valid_and_keeps_fields();
    test_corrupted_field_invalidates();
    test_wrong_magic_invalidates();
    test_clear_invalidates();
    test_layout_is_28_bytes();
    if (failures == 0) {
        std::printf("test_crash_record: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
