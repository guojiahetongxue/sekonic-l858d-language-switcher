#include "LanguageCore.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    SLDevice device;
    size_t count, calls, opens, closes;
    bool enumerationOK, openOK;
    char frames[12][7];
    SLReply replies[12];
} Mock;
static const char *const changeFrames[] = {
    "G00001", "G00006", "Z9606F", "Z00121", "Z00002", "Z00023",
    "Z00002", "W0000F", "Z00121", "Z00002", "G00006", "G00001"
};
static const char *const changeValues[] = {
    "0017", "0002", "0000", "0012", "0001", "0002",
    "0002", "0003", "0012", "0002", "0002", "0017"
};
static bool enumerate(void *context, SLDevice *device, size_t *count, const char **error) {
    Mock *m = context; *device = m->device; *count = m->count;
    if (!m->enumerationOK) *error = "mock_enumeration_failure";
    return m->enumerationOK;
}
static bool open_device(void *context, const SLDevice *device, const char **error) {
    Mock *m = context; ++m->opens; assert(strcmp(device->serial, m->device.serial) == 0);
    if (!m->openOK) *error = "exclusive_open_failed";
    return m->openOK;
}
static void exchange(void *context, const uint8_t request[7], SLReply *reply) {
    Mock *m = context;
    assert(m->calls < 12); assert(request[0] == 1);
    assert(memcmp(request + 1, m->frames[m->calls], 6) == 0);
    *reply = m->replies[m->calls++];
}
static void close_device(void *context) { ++((Mock *)context)->closes; }
static void set_value(Mock *m, size_t index, const char *value) { memcpy(m->replies[index].bytes + 3, value, 4); }
static const char *testFirmware = "0017";
static Mock fixture(void) {
    Mock m = {0};
    m.device = (SLDevice){.vendor = 0x0a41, .product = 0x7004, .usagePage = 65440, .inputSize = 7, .outputSize = 36};
    strcpy(m.device.serial, "JY10-ABC12345"); /* Deliberately not the original test device. */
    m.count = 1; m.enumerationOK = true; m.openOK = true;
    for (size_t i = 0; i < 12; ++i) {
        strcpy(m.frames[i], changeFrames[i]);
        m.replies[i] = (SLReply){.transportOK = true, .received = true, .sent = true, .reportID = 1, .reportType = 0, .length = 7};
        m.replies[i].bytes[0] = 1; m.replies[i].bytes[1] = (uint8_t)changeFrames[i][0];
        m.replies[i].bytes[2] = '0'; set_value(&m, i, changeValues[i]);
    }
    set_value(&m, 0, testFirmware);
    set_value(&m, 11, testFirmware);
    return m;
}
static SLOperation operation_for_language(int language) {
    const SLOperation operations[] = {SL_SET_ENGLISH, SL_SET_JAPANESE, SL_SET_CHINESE};
    assert(language >= 0 && language <= 2); return operations[language];
}
static Mock profile_fixture(const char *firmware, const char *region, int before, int target) {
    Mock m = fixture();
    const char *values[] = {"0000", "0001", "0002"};
    const char *setters[] = {"Z00003", "Z00013", "Z00023"};
    assert(before >= 0 && before <= 2 && target >= 0 && target <= 2);
    set_value(&m, 0, firmware); set_value(&m, 11, firmware);
    set_value(&m, 1, region); set_value(&m, 10, region);
    set_value(&m, 4, values[before]); set_value(&m, 5, values[target]);
    set_value(&m, 6, values[target]); set_value(&m, 9, values[target]);
    strcpy(m.frames[5], setters[target]);
    return m;
}
static SLResult run(Mock *m, SLOperation op, const char *serial) {
    SLTransport t = {.context = m, .enumerate = enumerate, .open = open_device, .exchange = exchange, .close = close_device};
    SLResult r; sl_run(&t, op, serial, &r); return r;
}
static void clean_no_write(const SLResult *r) {
    assert(!r->setterAttempted); assert(!r->saveAttempted); assert(!r->saveAcknowledged); assert(!r->stateMayHaveChanged);
}
static void expect_error(const SLResult *r, const char *code) {
    assert(!r->success); assert(!r->languageWriteSupported);
    assert(strcmp(r->errorCode, code) == 0); assert(sl_exit_status(r) != 0);
}
int main(void) {
    unsigned cases = 0;
    const char *versions[] = {"0017", "0018", "0019"};
    const size_t versionCount = sizeof(versions) / sizeof(versions[0]);
    for (size_t version = 0; version < versionCount; ++version) {
        testFirmware = versions[version];
        const char *expectedVerification = "hardware_verified";
        Mock m = fixture(); SLResult r = run(&m, SL_INSPECT, NULL);
        assert(r.success && strcmp(r.outcome, "inspected") == 0 && r.language == 1 && m.calls == 5);
        assert(m.opens == 1 && m.closes == 1);
        assert(strcmp(r.firmware, testFirmware) == 0 && strcmp(r.verificationLevel, expectedVerification) == 0);
        assert(strstr(r.message, "待完成") == NULL);
        clean_no_write(&r); ++cases;

        m = fixture(); r = run(&m, SL_SET_CHINESE, m.device.serial);
        assert(r.success && strcmp(r.outcome, "changed") == 0 && r.language == 2 && m.calls == 12);
        assert(r.setterAttempted && r.saveAttempted && r.saveAcknowledged && r.stateMayHaveChanged); ++cases;

        m = fixture(); strcpy(m.frames[5], "Z00013");
        set_value(&m, 4, "0002"); set_value(&m, 5, "0001"); set_value(&m, 6, "0001"); set_value(&m, 9, "0001");
        r = run(&m, SL_SET_JAPANESE, m.device.serial);
        assert(r.success && r.language == 1 && r.targetLanguage == 1 && m.calls == 12); ++cases;

        m = fixture(); set_value(&m, 4, "0002"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        assert(r.success && strcmp(r.outcome, "already_target") == 0 && m.calls == 5); clean_no_write(&r); ++cases;
        m = fixture(); r = run(&m, SL_SET_JAPANESE, m.device.serial);
        assert(r.success && strcmp(r.outcome, "already_target") == 0 && m.calls == 5); clean_no_write(&r); ++cases;

        m = fixture(); set_value(&m, 0, "0016"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "unsupported_firmware"); assert(strcmp(r.outcome, "unsupported") == 0 && m.calls == 1);
        assert(strcmp(r.firmware, "0016") == 0 && r.verificationLevel == NULL); clean_no_write(&r); ++cases;
        m = fixture(); set_value(&m, 1, "0004"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "unsupported_region"); assert(m.calls == 2); clean_no_write(&r); ++cases;
        m = fixture(); set_value(&m, 4, "0003"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "unsupported_language"); assert(m.calls == 5); clean_no_write(&r); ++cases;
        m = fixture(); r = run(&m, SL_SET_CHINESE, "JY10-SOMEONE-ELSE");
        expect_error(&r, "serial_mismatch"); assert(m.opens == 0 && m.calls == 0); clean_no_write(&r); ++cases;
        m = fixture(); r = run(&m, SL_SET_CHINESE, "JY10-ABC\n123");
        expect_error(&r, "invalid_serial_argument"); assert(m.opens == 0 && m.calls == 0); clean_no_write(&r); ++cases;
        m = fixture(); r = run(&m, SL_SET_CHINESE, NULL);
        expect_error(&r, "invalid_serial_argument"); assert(m.calls == 0); ++cases;
        m = fixture(); m.count = 2; r = run(&m, SL_INSPECT, NULL);
        expect_error(&r, "multiple_devices"); assert(m.opens == 0 && m.calls == 0); clean_no_write(&r); ++cases;
        m = fixture(); m.count = 0; r = run(&m, SL_INSPECT, NULL);
        expect_error(&r, "device_not_found"); assert(m.opens == 0 && m.calls == 0); ++cases;
        m = fixture(); m.openOK = false; r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "exclusive_open_failed"); assert(m.calls == 0); clean_no_write(&r); ++cases;
        m = fixture(); m.enumerationOK = false; r = run(&m, SL_INSPECT, NULL);
        expect_error(&r, "mock_enumeration_failure"); assert(m.opens == 0); ++cases;

        /* Every identity guard blocks before opening, independent of serial pin. */
        for (unsigned field = 0; field < 6; ++field) {
            m = fixture();
            if (field == 0) m.device.vendor++;
            if (field == 1) m.device.product++;
            if (field == 2) m.device.usagePage++;
            if (field == 3) m.device.inputSize++;
            if (field == 4) m.device.outputSize++;
            if (field == 5) memset(m.device.serial, 'x', sizeof(m.device.serial));
            r = run(&m, SL_INSPECT, NULL); expect_error(&r, "device_identity_mismatch");
            assert(m.opens == 0 && m.calls == 0); clean_no_write(&r); ++cases;
        }
        /* Timeout at each of the twelve stages stops exactly there, without retry.
         * Attempt flags reflect actual submission, including an uncertain setter. */
        for (size_t stage = 0; stage < 12; ++stage) {
            m = fixture(); m.replies[stage].transportOK = false; m.replies[stage].received = false;
            m.replies[stage].errorCode = "reply_timeout";
            r = run(&m, SL_SET_CHINESE, m.device.serial); expect_error(&r, "reply_timeout");
            assert(m.calls == stage + 1 && m.closes == 1);
            assert(r.setterAttempted == (stage >= 5)); assert(r.saveAttempted == (stage >= 7));
            assert(r.saveAcknowledged == (stage > 7)); assert(r.stateMayHaveChanged == (stage >= 5)); ++cases;
        }
        /* Strict response fields, including the previously audited high nibble. */
        for (unsigned corruption = 0; corruption < 8; ++corruption) {
            m = fixture();
            if (corruption == 0) m.replies[0].bytes[2] = '1';
            if (corruption == 1) m.replies[0].bytes[1] = 'W';
            if (corruption == 2) m.replies[0].reportID = 2;
            if (corruption == 3) m.replies[0].reportType = 1;
            if (corruption == 4) m.replies[0].length = 6;
            if (corruption == 5) m.replies[0].bytes[3] = 'x';
            if (corruption == 6) set_value(&m, 0, "FFFC");
            if (corruption == 7) { m.replies[0].reportID = 3; m.replies[0].bytes[0] = 3; }
            r = run(&m, SL_SET_CHINESE, m.device.serial); assert(!r.success && m.calls == 1); clean_no_write(&r); ++cases;
        }
        m = fixture(); for (size_t i = 0; i < 5; ++i) { m.replies[i].reportID = 2; m.replies[i].bytes[0] = 2; }
        r = run(&m, SL_INSPECT, NULL); assert(r.success); ++cases;

        m = fixture(); set_value(&m, 2, "0001"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "unexpected_reply_value"); assert(m.calls == 3); clean_no_write(&r); ++cases;
        m = fixture(); set_value(&m, 3, "0011"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "unexpected_reply_value"); assert(m.calls == 4); clean_no_write(&r); ++cases;
        m = fixture(); set_value(&m, 5, "0001"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "unexpected_reply_value"); assert(r.setterAttempted && !r.saveAttempted && r.language == 1 && m.calls == 6); ++cases;
        m = fixture(); set_value(&m, 6, "0001"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "language_readback_mismatch"); assert(!r.saveAttempted && r.setterAttempted && m.calls == 7); ++cases;
        m = fixture(); set_value(&m, 7, "0000"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "unexpected_reply_value"); assert(r.saveAttempted && !r.saveAcknowledged && r.language == 2 && m.calls == 8); ++cases;
        m = fixture(); m.replies[7].transportOK = false; m.replies[7].errorCode = "unsolicited_or_duplicate_input";
        r = run(&m, SL_SET_CHINESE, m.device.serial); expect_error(&r, "unsolicited_or_duplicate_input");
        assert(r.saveAcknowledged && r.saveAttempted && m.calls == 8); ++cases;
        m = fixture(); m.replies[5] = (SLReply){.errorCode = "unsolicited_or_duplicate_input"};
        r = run(&m, SL_SET_CHINESE, m.device.serial); expect_error(&r, "unsolicited_or_duplicate_input");
        assert(!r.setterAttempted && !r.stateMayHaveChanged); ++cases;
        m = fixture(); set_value(&m, 10, "0001"); r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "unexpected_reply_value"); assert(r.saveAcknowledged && m.calls == 11); ++cases;

        /* The final version must equal this session's initial version. Merely
         * finding another supported version is not an acceptable postcondition. */
        m = fixture(); set_value(&m, 11, versions[(version + 1) % versionCount]);
        r = run(&m, SL_SET_CHINESE, m.device.serial);
        expect_error(&r, "unexpected_reply_value"); assert(r.saveAcknowledged && m.calls == 12);
        assert(strcmp(r.firmware, testFirmware) == 0); ++cases;

        /* An unreviewed newer version is rejected before service unlock. */
        m = fixture(); set_value(&m, 0, "0020"); r = run(&m, SL_INSPECT, NULL);
        expect_error(&r, "unsupported_firmware"); assert(m.calls == 1 && r.verificationLevel == NULL);
        clean_no_write(&r); ++cases;

        m = fixture(); r = run(&m, SL_SET_CHINESE, m.device.serial);
        FILE *json = tmpfile(); assert(json); sl_print_json(json, &r); rewind(json);
        char output[8192] = {0}; assert(fread(output, 1, sizeof(output) - 1, json) > 0); fclose(json);
        assert(strstr(output, "\"success\":true") && strstr(output, "\"saveAcknowledged\":true"));
        assert(strstr(output, "已切换为中文") && strstr(output, "\"request\":\"W0000F\""));
        assert(strstr(output, "\"verificationLevel\":") && strstr(output, expectedVerification)); ++cases;
        printf("PASS: firmware %s profile and rejection matrix.\n", testFirmware);
    }
    /* The complete current-language / requested-language matrix for each
     * explicitly reviewed firmware+region pair. These are simulated transport
     * cases, not claims of hardware validation in the additional regions. */
    const char *regions[] = {"0000", "0001", "0002", "0003"};
    for (size_t version = 0; version < versionCount; ++version) {
        for (size_t region = 0; region < 4; ++region) {
            for (int before = 0; before <= 2; ++before) {
                Mock m = profile_fixture(versions[version], regions[region], before, 0);
                SLResult r = run(&m, SL_INSPECT, NULL);
                assert(r.success && r.languageWriteSupported && r.language == before && m.calls == 5);
                assert(strcmp(r.verificationLevel, region == 2 ? "hardware_verified" : "static_verified") == 0);
                clean_no_write(&r); ++cases;
                for (int target = 0; target <= 2; ++target) {
                    m = profile_fixture(versions[version], regions[region], before, target);
                    r = run(&m, operation_for_language(target), m.device.serial);
                    assert(r.success && r.languageWriteSupported && r.language == target && r.targetLanguage == target);
                    assert(strcmp(r.firmware, versions[version]) == 0 && strcmp(r.region, regions[region]) == 0);
                    assert(strcmp(r.verificationLevel, region == 2 ? "hardware_verified" : "static_verified") == 0);
                    if (before == target) {
                        assert(strcmp(r.outcome, "already_target") == 0 && m.calls == 5); clean_no_write(&r);
                    } else {
                        assert(strcmp(r.outcome, "changed") == 0 && m.calls == 12);
                        assert(r.setterAttempted && r.saveAttempted && r.saveAcknowledged && r.stateMayHaveChanged);
                    }
                    ++cases;
                }
            }
            for (int target = 0; target <= 2; ++target) {
                int before = (target + 1) % 3;
                for (size_t stage = 0; stage < 12; ++stage) {
                    Mock m = profile_fixture(versions[version], regions[region], before, target);
                    m.replies[stage].transportOK = false; m.replies[stage].received = false;
                    m.replies[stage].errorCode = "reply_timeout";
                    SLResult r = run(&m, operation_for_language(target), m.device.serial);
                    expect_error(&r, "reply_timeout"); assert(m.calls == stage + 1 && m.closes == 1);
                    assert(r.targetLanguage == target && r.setterAttempted == (stage >= 5));
                    assert(r.saveAttempted == (stage >= 7) && r.saveAcknowledged == (stage > 7));
                    assert(r.stateMayHaveChanged == (stage >= 5)); ++cases;
                }
                Mock m = profile_fixture(versions[version], regions[region], before, target);
                m.replies[5].bytes[2] = '1';
                SLResult r = run(&m, operation_for_language(target), m.device.serial);
                expect_error(&r, "unexpected_reply_header");
                assert(r.setterAttempted && !r.saveAttempted && m.calls == 6); ++cases;

                m = profile_fixture(versions[version], regions[region], before, target);
                set_value(&m, 7, "0000");
                r = run(&m, operation_for_language(target), m.device.serial);
                expect_error(&r, "unexpected_reply_value");
                assert(r.setterAttempted && r.saveAttempted && !r.saveAcknowledged && m.calls == 8); ++cases;
            }
            Mock m = profile_fixture(versions[version], regions[region], 1, 0);
            set_value(&m, 4, "0003");
            SLResult r = run(&m, SL_SET_ENGLISH, m.device.serial);
            expect_error(&r, "unsupported_language"); assert(m.calls == 5); clean_no_write(&r); ++cases;

            m = profile_fixture(versions[version], regions[region], 1, 0);
            r = run(&m, SL_SET_ENGLISH, "JY10-CHANGED-DEVICE");
            expect_error(&r, "serial_mismatch"); assert(m.calls == 0 && m.opens == 0); clean_no_write(&r); ++cases;

            /* A different allowed region is still a failed postcondition. */
            for (size_t otherRegion = 0; otherRegion < 4; ++otherRegion) {
                if (otherRegion == region) continue;
                m = profile_fixture(versions[version], regions[region], 1, 0);
                set_value(&m, 10, regions[otherRegion]);
                r = run(&m, SL_SET_ENGLISH, m.device.serial);
                expect_error(&r, "unexpected_reply_value");
                assert(r.saveAcknowledged && m.calls == 11 && strcmp(r.region, regions[region]) == 0); ++cases;
            }
            m = profile_fixture(versions[version], regions[region], 1, 0);
            set_value(&m, 11, versions[(version + 1) % versionCount]);
            r = run(&m, SL_SET_ENGLISH, m.device.serial);
            expect_error(&r, "unexpected_reply_value"); assert(r.saveAcknowledged && m.calls == 12); ++cases;
        }
    }
    for (size_t version = 0; version < versionCount; ++version) {
        const char *unknownRegions[] = {"0004", "0007", "000F", "0100"};
        for (size_t i = 0; i < 4; ++i) {
            Mock m = profile_fixture(versions[version], unknownRegions[i], 1, 0);
            SLResult r = run(&m, SL_SET_ENGLISH, m.device.serial);
            expect_error(&r, "unsupported_region"); assert(m.calls == 2 && !r.verificationLevel);
            clean_no_write(&r); ++cases;
        }
    }
    Mock m = profile_fixture("0019", "0000", 1, 0);
    SLResult r = run(&m, SL_SET_ENGLISH, m.device.serial);
    FILE *json = tmpfile(); assert(json); sl_print_json(json, &r); rewind(json);
    char output[8192] = {0}; assert(fread(output, 1, sizeof(output) - 1, json) > 0); fclose(json);
    assert(strstr(output, "\"languageWriteSupported\":true,\"supportedLanguages\":[0,1,2]"));
    assert(strstr(output, "已切换为英文") && strstr(output, "\"request\":\"Z00003\"")); ++cases;
    m = profile_fixture("0019", "0004", 1, 0); r = run(&m, SL_SET_ENGLISH, m.device.serial);
    json = tmpfile(); assert(json); sl_print_json(json, &r); rewind(json); memset(output, 0, sizeof(output));
    assert(fread(output, 1, sizeof(output) - 1, json) > 0); fclose(json);
    assert(strstr(output, "\"languageWriteSupported\":false,\"supportedLanguages\":[]")); ++cases;
    /* Unsupported versions must stop after the firmware query, before the
     * region query, service unlock or any setter/save. Cover inspect and each
     * target independently; a static historical audit does not allow writes. */
    const char *unsupportedVersions[] = {
        "0000", "0009", "0010", "0011", "0012", "0013", "0014", "0015", "0016",
        "0020", "001A", "1234", "9999"
    };
    const SLOperation operations[] = {SL_INSPECT, SL_SET_ENGLISH, SL_SET_JAPANESE, SL_SET_CHINESE};
    for (size_t version = 0; version < sizeof(unsupportedVersions) / sizeof(unsupportedVersions[0]); ++version) {
        for (size_t op = 0; op < sizeof(operations) / sizeof(operations[0]); ++op) {
            m = profile_fixture(unsupportedVersions[version], "0002", 1, 0);
            r = run(&m, operations[op], op ? m.device.serial : NULL);
            expect_error(&r, "unsupported_firmware");
            assert(strcmp(r.outcome, "unsupported") == 0 && r.verificationLevel == NULL);
            assert(m.calls == 1 && m.opens == 1 && m.closes == 1 && r.eventCount == 1);
            assert(strcmp(r.events[0].request, "G00001") == 0 && r.region[0] == 0 && r.language == -1);
            assert(strstr(r.message, "17、18、19") && strstr(r.message, "世光官方软件"));
            assert(strstr(r.message, "Data Transfer Software") && strstr(r.message, "升级到 19 版"));
            assert(strstr(r.message, "检查是否可升级") && strstr(r.message, "如支持"));
            assert(strstr(r.message, "再切换语言") && strstr(r.message, "未修改设备"));
            clean_no_write(&r); ++cases;
        }
    }
    /* A final version equal to any other allowed profile is still a failure.
     * Cover both directions between all three versions and all target frames. */
    for (size_t version = 0; version < versionCount; ++version) {
        for (size_t region = 0; region < 4; ++region) {
            for (int target = 0; target <= 2; ++target) {
                for (size_t otherVersion = 0; otherVersion < versionCount; ++otherVersion) {
                    if (otherVersion == version) continue;
                    m = profile_fixture(versions[version], regions[region], (target + 1) % 3, target);
                    set_value(&m, 11, versions[otherVersion]);
                    r = run(&m, operation_for_language(target), m.device.serial);
                    expect_error(&r, "unexpected_reply_value");
                    assert(m.calls == 12 && r.saveAcknowledged && r.setterAttempted && r.saveAttempted);
                    assert(strcmp(r.firmware, versions[version]) == 0 && strcmp(r.region, regions[region]) == 0);
                    ++cases;
                }
                /* The firmware's actual failed-save result is W0000F. */
                m = profile_fixture(versions[version], regions[region], (target + 1) % 3, target);
                set_value(&m, 7, "000F");
                r = run(&m, operation_for_language(target), m.device.serial);
                expect_error(&r, "unexpected_reply_value");
                assert(m.calls == 8 && r.setterAttempted && r.saveAttempted && !r.saveAcknowledged);
                assert(r.stateMayHaveChanged); ++cases;
            }
        }
    }
    printf("PASS: all 12 firmware/region profiles, 3 languages, unsupported-version upgrade guidance and failure safeguards.\n");
    printf("PASS: %u offline core scenarios; no IOKit transport linked or USB accessed.\n", cases);
    return 0;
}
