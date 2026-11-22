#include "error_log.h"
#include "error_log_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !ERROR_LOG_ENABLED || !ERROR_LOG_FLASH_ENABLED
#error "Build the Flash test with logging and Flash enabled"
#endif

#include "error_log_port.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define SLOT_SIZE 64U
#define PROGRAM_SIZE 32U
#define SLOT_COUNT 5U

typedef enum {
    PROGRAM_NORMAL,
    PROGRAM_PARTIAL_AND_FAIL,
    PROGRAM_ALL_AND_FAIL,
    PROGRAM_SILENT_CORRUPTION
} program_fault_t;

typedef struct {
    uint8_t bytes[SLOT_SIZE * SLOT_COUNT];
    uint32_t fail_read_offset;
    unsigned program_calls;
    unsigned fail_program_call;
    program_fault_t fault;
} fake_flash_t;

typedef struct {
    char output[1024];
    size_t length;
    unsigned calls;
    unsigned fail_call;
} replay_capture_t;

error_log_port_key_t error_log_port_enter(void)
{
    return 0U;
}

void error_log_port_exit(error_log_port_key_t key)
{
    CHECK(key == 0U);
}

uint32_t error_log_port_tick(void)
{
    return 12U;
}

static bool flash_read(void *context, uint32_t offset, uint8_t *out, size_t length)
{
    fake_flash_t *flash = context;

    CHECK(offset <= sizeof flash->bytes);
    CHECK(length <= sizeof flash->bytes - offset);
    if (offset == flash->fail_read_offset) {
        return false;
    }
    memcpy(out, &flash->bytes[offset], length);
    return true;
}

static bool flash_program(void *context, uint32_t offset, const uint8_t *in, size_t length)
{
    fake_flash_t *flash = context;

    CHECK(offset <= sizeof flash->bytes);
    CHECK(length <= sizeof flash->bytes - offset);
    CHECK(offset % PROGRAM_SIZE == 0U && length == PROGRAM_SIZE);
    if (offset % SLOT_SIZE == 0U) {
        CHECK(memcmp(in, "EFL1", 4U) == 0);
    } else {
        CHECK(memcmp(&flash->bytes[offset - PROGRAM_SIZE], "EFL1", 4U) == 0);
        CHECK(memcmp(&in[PROGRAM_SIZE - 4U], "COM1", 4U) == 0);
    }
    ++flash->program_calls;

    const bool fail = flash->program_calls == flash->fail_program_call;
    size_t write_length = length;
    if (fail && flash->fault == PROGRAM_PARTIAL_AND_FAIL) {
        /* Leave one marker byte erased if power is cut during the commit write. */
        write_length = length - 1U;
    }
    for (size_t i = 0U; i < write_length; ++i) {
        CHECK(flash->bytes[offset + i] == 0xFFU);
        flash->bytes[offset + i] &= in[i];
    }
    if (fail && flash->fault == PROGRAM_SILENT_CORRUPTION) {
        flash->bytes[offset] = 0U;
        return true;
    }

    return !fail || flash->fault == PROGRAM_NORMAL;
}

static error_log_flash_driver_t blank_flash(fake_flash_t *flash)
{
    *flash = (fake_flash_t){ .fail_read_offset = UINT32_MAX };
    memset(flash->bytes, 0xFF, sizeof flash->bytes);
    return (error_log_flash_driver_t) {
        .context = flash,
        .region_size = sizeof flash->bytes,
        .read = flash_read,
        .program = flash_program
    };
}

static bool capture_line(void *context, const char *line, size_t length)
{
    replay_capture_t *capture = context;

    ++capture->calls;
    if (capture->calls == capture->fail_call) {
        return false;
    }
    CHECK(length <= sizeof capture->output - capture->length - 1U);
    memcpy(&capture->output[capture->length], line, length);
    capture->length += length;
    capture->output[capture->length] = '\0';
    return true;
}

static void test_empty_and_validation(void)
{
    fake_flash_t hardware;
    const error_log_flash_driver_t driver = blank_flash(&hardware);
    error_log_flash_driver_t bad_driver = driver;
    error_log_flash_t flash = {0};
    const error_log_record_t record = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
    error_log_flash_entry_t entry = { .slot = 91U, .firmware_id = 92U, .record = record };
    uint32_t cursor = 0U;
    replay_capture_t capture = {0};

    CHECK(error_log_flash_init(NULL, &driver, 0xA001U) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    CHECK(error_log_flash_init(&flash, NULL, 0xA001U) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    bad_driver.program = NULL;
    CHECK(error_log_flash_init(&flash, &bad_driver, 0xA001U) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    bad_driver = driver;
    bad_driver.region_size = 0U;
    CHECK(error_log_flash_init(&flash, &bad_driver, 0xA001U) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    bad_driver.region_size = SLOT_SIZE + 1U;
    CHECK(error_log_flash_init(&flash, &bad_driver, 0xA001U) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    CHECK(error_log_flash_append(&flash, &record) == ERROR_LOG_FLASH_NOT_READY);
    CHECK(error_log_flash_poll(NULL) == ERROR_LOG_FLASH_INVALID_ARGUMENT);

    CHECK(error_log_save2(3U, 4U, 5U, 6U, 7U) == ERROR_LOG_OK);
    hardware.fail_read_offset = SLOT_SIZE;
    CHECK(error_log_flash_init(&flash, &driver, 0xA001U) == ERROR_LOG_FLASH_IO_ERROR);
    CHECK(!flash.ready && error_log_flash_poll(&flash) == ERROR_LOG_FLASH_NOT_READY);
    CHECK(error_log_flash_replay(&flash, capture_line, &capture) == ERROR_LOG_FLASH_NOT_READY);
    CHECK(capture.calls == 0U);
    hardware.fail_read_offset = UINT32_MAX;

    CHECK(error_log_flash_init(&flash, &driver, 0xA001U) == ERROR_LOG_FLASH_OK);
    CHECK(flash.ready && flash.capacity == SLOT_COUNT && flash.next_slot == 0U);
    CHECK(flash.boot_occupied == 0U && flash.boot_valid == 0U && flash.boot_bad == 0U);
    CHECK(error_log_flash_read_next(&flash, &cursor, &entry) == ERROR_LOG_FLASH_EMPTY);
    CHECK(entry.slot == 91U && entry.firmware_id == 92U && entry.record.param2 == 7U);
    CHECK(cursor == 0U);
    CHECK(error_log_flash_replay(&flash, NULL, &capture) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    CHECK(error_log_flash_replay(&flash, capture_line, &capture) == ERROR_LOG_FLASH_OK);
    CHECK(capture.calls == 2U);
    CHECK(strcmp(capture.output, "[ErrLog] BEGIN valid=0 occupied=0/5 bad=0\r\n"
        "[ErrLog] END shown=0 full=0\r\n") == 0);

    CHECK(error_log_flash_poll(&flash) == ERROR_LOG_FLASH_OK);
    CHECK(error_log_flash_poll(&flash) == ERROR_LOG_FLASH_EMPTY);
    CHECK(hardware.program_calls == 2U && flash.next_slot == 1U);
    CHECK(hardware.bytes[0] == 'E' && hardware.bytes[1] == 'F' &&
        hardware.bytes[2] == 'L' && hardware.bytes[3] == '1');
    CHECK(hardware.bytes[4] == 1U && hardware.bytes[5] == 0U);
    CHECK(hardware.bytes[8] == 12U && hardware.bytes[12] == 3U);
    CHECK(hardware.bytes[16] == 4U && hardware.bytes[20] == 5U);
    CHECK(hardware.bytes[24] == 6U && hardware.bytes[28] == 7U);
    CHECK(hardware.bytes[32] == 0x01U && hardware.bytes[33] == 0xA0U);
    /* Known CRC-32/ISO-HDLC test value for the preceding 36 bytes. */
    CHECK(hardware.bytes[36] == 0x57U && hardware.bytes[37] == 0xF9U &&
        hardware.bytes[38] == 0x66U && hardware.bytes[39] == 0xB2U);
    for (unsigned offset = 40U; offset < 60U; ++offset) {
        CHECK(hardware.bytes[offset] == 0xFFU);
    }
    CHECK(hardware.bytes[60] == 'C' && hardware.bytes[61] == 'O' &&
        hardware.bytes[62] == 'M' && hardware.bytes[63] == '1');

    /* Startup replay is a snapshot of rows present at init, not the live tail. */
    CHECK(error_log_flash_read_next(&flash, &cursor, &entry) == ERROR_LOG_FLASH_EMPTY);
    CHECK(error_log_flash_init(&flash, &driver, 0xB002U) == ERROR_LOG_FLASH_OK);
    CHECK(flash.boot_occupied == 1U && flash.boot_valid == 1U && flash.boot_bad == 0U);
    CHECK(error_log_flash_read_next(&flash, &cursor, &entry) == ERROR_LOG_FLASH_OK);
    CHECK(cursor == 1U && entry.slot == 1U && entry.firmware_id == 0xA001U);
    CHECK(entry.record.sequence == 1U && entry.record.tick == 12U);
    CHECK(entry.record.file_id == 3U && entry.record.line == 4U);
    CHECK(entry.record.error_id == 5U && entry.record.param1 == 6U &&
        entry.record.param2 == 7U);
}

static void test_faults_recovery_and_replay(void)
{
    fake_flash_t hardware;
    const error_log_flash_driver_t driver = blank_flash(&hardware);
    error_log_flash_t flash = {0};
    error_log_flash_entry_t entry = { .slot = 99U };
    error_log_record_t record = {51U, 52U, 53U, 54U, 55U, 0xAABBCCDDU, UINT32_MAX};
    uint32_t cursor = 0U;
    replay_capture_t capture = {0};

    CHECK(error_log_flash_init(&flash, &driver, 0xA1B2C3D4U) == ERROR_LOG_FLASH_OK);
    CHECK(error_log_flash_append(&flash, NULL) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    record.file_id = 0U;
    CHECK(error_log_flash_append(&flash, &record) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    CHECK(flash.next_slot == 0U && hardware.program_calls == 0U);
    record.file_id = 53U;
    CHECK(error_log_flash_append(&flash, &record) == ERROR_LOG_FLASH_OK);

    /* Skip a slot whose pre-write read fails, creating a real erased hole. */
    hardware.fail_read_offset = SLOT_SIZE;
    CHECK(error_log_flash_append(&flash, &record) == ERROR_LOG_FLASH_IO_ERROR);
    CHECK(flash.next_slot == 2U && hardware.program_calls == 2U);
    hardware.fail_read_offset = UINT32_MAX;
    CHECK(hardware.bytes[SLOT_SIZE] == 0xFFU);

    /* Power cut during the commit unit leaves an incomplete record. */
    hardware.fail_program_call = 4U;
    hardware.fault = PROGRAM_PARTIAL_AND_FAIL;
    CHECK(error_log_flash_append(&flash, &record) == ERROR_LOG_FLASH_IO_ERROR);
    CHECK(flash.next_slot == 3U && hardware.bytes[3U * SLOT_SIZE - 1U] == 0xFFU);

    /* A backend can report failure even if a complete commit reached the chip. */
    hardware.fail_program_call = 6U;
    hardware.fault = PROGRAM_ALL_AND_FAIL;
    record.sequence = 61U;
    CHECK(error_log_flash_append(&flash, &record) == ERROR_LOG_FLASH_IO_ERROR);
    CHECK(flash.next_slot == 4U && hardware.bytes[4U * SLOT_SIZE - 1U] == '1');

    /* A successful backend return alone must not verify a corrupted body. */
    hardware.fail_program_call = 7U;
    hardware.fault = PROGRAM_SILENT_CORRUPTION;
    CHECK(error_log_flash_append(&flash, &record) == ERROR_LOG_FLASH_IO_ERROR);
    CHECK(flash.next_slot == SLOT_COUNT && hardware.program_calls == 7U);

    uint8_t before[sizeof hardware.bytes];
    memcpy(before, hardware.bytes, sizeof before);
    CHECK(error_log_flash_append(&flash, &record) == ERROR_LOG_FLASH_FULL);
    CHECK(error_log_save2(3U, 4U, 5U, 6U, 7U) == ERROR_LOG_OK);
    CHECK(error_log_flash_poll(&flash) == ERROR_LOG_FLASH_FULL);
    CHECK(error_log_flash_poll(&flash) == ERROR_LOG_FLASH_EMPTY);
    CHECK(memcmp(before, hardware.bytes, sizeof before) == 0);

    CHECK(error_log_flash_init(&flash, &driver, 0xDDEEFF00U) == ERROR_LOG_FLASH_OK);
    CHECK(flash.boot_occupied == SLOT_COUNT && flash.boot_valid == 2U && flash.boot_bad == 2U);
    CHECK(flash.next_slot == SLOT_COUNT);
    CHECK(error_log_flash_read_next(&flash, NULL, &entry) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    CHECK(error_log_flash_read_next(&flash, &cursor, NULL) == ERROR_LOG_FLASH_INVALID_ARGUMENT);
    CHECK(error_log_flash_read_next(&flash, &cursor, &entry) == ERROR_LOG_FLASH_OK);
    CHECK(cursor == 1U && entry.slot == 1U && entry.firmware_id == 0xA1B2C3D4U);
    CHECK(entry.record.sequence == 51U && entry.record.param1 == 0xAABBCCDDU);

    hardware.fail_read_offset = 3U * SLOT_SIZE;
    CHECK(error_log_flash_read_next(&flash, &cursor, &entry) == ERROR_LOG_FLASH_IO_ERROR);
    CHECK(cursor == 1U && entry.slot == 1U);
    CHECK(error_log_flash_replay(&flash, capture_line, &capture) == ERROR_LOG_FLASH_IO_ERROR);
    CHECK(capture.calls == 2U && strstr(capture.output, "[ErrLog] END") == NULL);
    hardware.fail_read_offset = UINT32_MAX;

    CHECK(error_log_flash_read_next(&flash, &cursor, &entry) == ERROR_LOG_FLASH_OK);
    CHECK(cursor == 4U && entry.slot == 4U && entry.record.sequence == 61U);
    CHECK(error_log_flash_read_next(&flash, &cursor, &entry) == ERROR_LOG_FLASH_EMPTY);
    CHECK(cursor == SLOT_COUNT && entry.slot == 4U);

    capture = (replay_capture_t){ .fail_call = 2U };
    CHECK(error_log_flash_replay(&flash, capture_line, &capture) == ERROR_LOG_FLASH_IO_ERROR);
    CHECK(capture.calls == 2U && strcmp(capture.output,
        "[ErrLog] BEGIN valid=2 occupied=5/5 bad=2\r\n") == 0);

    capture = (replay_capture_t){0};
    CHECK(error_log_flash_replay(&flash, capture_line, &capture) == ERROR_LOG_FLASH_OK);
    CHECK(capture.calls == 4U);
    CHECK(strcmp(capture.output,
        "[ErrLog] BEGIN valid=2 occupied=5/5 bad=2\r\n"
        "[ErrLog] slot=1 fw=0xA1B2C3D4 seq=51 tick=52 file=53 line=54 err=55 "
        "p1=0xAABBCCDD p2=0xFFFFFFFF\r\n"
        "[ErrLog] slot=4 fw=0xA1B2C3D4 seq=61 tick=52 file=53 line=54 err=55 "
        "p1=0xAABBCCDD p2=0xFFFFFFFF\r\n"
        "[ErrLog] END shown=2 full=1\r\n") == 0);
    CHECK(memcmp(before, hardware.bytes, sizeof before) == 0);

    /* Clear one set bit in a committed payload. CRC must reject it after reboot. */
    hardware.bytes[24] &= 0xFEU;
    CHECK(error_log_flash_init(&flash, &driver, 0xA1B2C3D4U) == ERROR_LOG_FLASH_OK);
    CHECK(flash.boot_occupied == SLOT_COUNT && flash.boot_valid == 1U && flash.boot_bad == 3U);
}

int main(void)
{
    test_empty_and_validation();
    test_faults_recovery_and_replay();
    puts("PASS: Flash append, recovery, and startup replay");
    return EXIT_SUCCESS;
}
