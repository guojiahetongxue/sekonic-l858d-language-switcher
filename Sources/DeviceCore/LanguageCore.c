#include "LanguageCore.h"
#include <string.h>

/* These are the only device commands reachable by this program. The language
 * setter changes RAM; W0000F saves the existing SETT block. Neither command
 * changes the firmware, region, calibration, USER, or PRF blocks. */
static const uint8_t firmwareFrame[7] = {1,'G','0','0','0','0','1'};
static const uint8_t regionFrame[7] = {1,'G','0','0','0','0','6'};
static const uint8_t unlockFrame[7] = {1,'Z','9','6','0','6','F'};
static const uint8_t selectFrame[7] = {1,'Z','0','0','1','2','1'};
static const uint8_t readFrame[7] = {1,'Z','0','0','0','0','2'};
static const uint8_t englishFrame[7] = {1,'Z','0','0','0','0','3'};
static const uint8_t chineseFrame[7] = {1,'Z','0','0','0','2','3'};
static const uint8_t japaneseFrame[7] = {1,'Z','0','0','0','1','3'};
static const uint8_t saveFrame[7] = {1,'W','0','0','0','0','F'};

/* Exact firmware/region pairs, not a numeric range or permission to change
 * region. The same official image's language getter/setter and SETT-only save
 * have no region condition. Evidence for these region codes in all three images:
 * 0: product-info chooses L-858D-U (V17 FFFA174D, V18 FFFA1FE9, V19 FFFA1FF2).
 * 1: explicit radio case (V17 FFF99BCC, V18/V19 FFF9A462).
 * 2/3: Japanese/Chinese defaults (V17 FFF893D2, V18/V19 FFF89BF6).
 * Radio handling is evidence only; this program never calls its write path.
 * Chinese persistence was verified after restart on V17/V19 using the same
 * single Japanese/region-2 unit. Japanese save/readback passed on V19 without
 * a separate restart. V18 on that same unit passed English setter/save and
 * later independent readback; explicit V18 English power-cycle confirmation
 * is absent. Existing Chinese survived the downgrade to V18, but fresh V18
 * Chinese/Japanese writes were deferred. V17 also retained English after
 * downgrade and restart; a fresh English setter was not tested in that run.
 * Other regions have static evidence only. hardware_verified describes a
 * firmware/region pair with hardware observations, not proof of every target
 * language, direction, power cycle, hardware batch, or individual device. */
typedef struct {
    const char *firmware, *region, *verificationLevel;
} SLFirmwareProfile;
static const SLFirmwareProfile firmwareProfiles[] = {
    {"0017", "0000", "static_verified"},
    {"0017", "0001", "static_verified"},
    {"0017", "0002", "hardware_verified"},
    {"0017", "0003", "static_verified"},
    {"0018", "0000", "static_verified"},
    {"0018", "0001", "static_verified"},
    {"0018", "0002", "hardware_verified"},
    {"0018", "0003", "static_verified"},
    {"0019", "0000", "static_verified"},
    {"0019", "0001", "static_verified"},
    {"0019", "0002", "hardware_verified"},
    {"0019", "0003", "static_verified"}
};
static bool known_firmware(const char *firmware) {
    for (size_t i = 0; i < sizeof(firmwareProfiles) / sizeof(firmwareProfiles[0]); ++i) {
        if (strcmp(firmware, firmwareProfiles[i].firmware) == 0) return true;
    }
    return false;
}
static const SLFirmwareProfile *find_profile(const char *firmware, const char *region) {
    for (size_t i = 0; i < sizeof(firmwareProfiles) / sizeof(firmwareProfiles[0]); ++i) {
        if (strcmp(firmware, firmwareProfiles[i].firmware) == 0 &&
            strcmp(region, firmwareProfiles[i].region) == 0) return &firmwareProfiles[i];
    }
    return NULL;
}
typedef struct {
    SLOperation operation;
    int language;
    const uint8_t *frame;
    const char *expected, *alreadyMessage, *changedMessage;
} SLLanguageOperation;
static const SLLanguageOperation languageOperations[] = {
    {SL_SET_ENGLISH, 0, englishFrame, "0000", "当前已是英文，无需重复写入。",
     "已切换为英文并收到保存确认。请拔掉 USB，关机后重新开机检查。"},
    {SL_SET_JAPANESE, 1, japaneseFrame, "0001", "当前已是日文，无需重复写入。",
     "已切换为日文并收到保存确认。请拔掉 USB，关机后重新开机检查。"},
    {SL_SET_CHINESE, 2, chineseFrame, "0002", "当前已是中文，无需重复写入。",
     "已切换为中文并收到保存确认。请拔掉 USB，关机后重新开机检查。"}
};
static const SLLanguageOperation *find_language_operation(SLOperation operation) {
    for (size_t i = 0; i < sizeof(languageOperations) / sizeof(languageOperations[0]); ++i) {
        if (operation == languageOperations[i].operation) return &languageOperations[i];
    }
    return NULL;
}

bool sl_safe_serial(const char *serial) {
    if (!serial || !*serial) return false;
    size_t i = 0;
    for (; i < SL_SERIAL_CAPACITY && serial[i]; ++i) {
        unsigned char c = (unsigned char)serial[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return i > 0 && i < SL_SERIAL_CAPACITY;
}
void sl_initialize_result(SLResult *r, SLOperation operation) {
    memset(r, 0, sizeof(*r));
    r->outcome = "error";
    r->language = -1;
    const SLLanguageOperation *target = find_language_operation(operation);
    r->targetLanguage = target ? target->language : -1;
    r->message = "操作未完成。";
}
static void error(SLResult *r, const char *code, const char *message) {
    r->success = false;
    r->languageWriteSupported = false;
    r->outcome = "error";
    r->errorCode = code ? code : "transport_error";
    r->message = message;
    r->stateMayHaveChanged = r->setterAttempted;
    if (r->setterAttempted) {
        r->message = r->saveAcknowledged
            ? "设备已确认保存，但后续核验未完成。请拔线重启并检查菜单语言；不要直接重复操作。"
            : r->saveAttempted
                ? "已发送语言修改和保存命令，但未确认保存成功。请拔线重启并检查菜单语言；不要直接重复操作。"
                : "已发送语言修改命令，但尚未完成保存。当前语言可能已改变，请拔线重启后重新检测。";
    }
}
static void unsupported(SLResult *r, const char *code, const char *message) {
    error(r, code, message);
    r->outcome = "unsupported";
}
static bool is_hex(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }
static bool valid_reply(const uint8_t *request, const SLReply *reply, const char **code) {
    if (!reply->received) { *code = "missing_reply"; return false; }
    if (reply->reportType != 0 || reply->length != 7) { *code = "unexpected_reply_type_or_length"; return false; }
    if ((reply->reportID != 1 && reply->reportID != 2) || reply->bytes[0] != reply->reportID ||
        reply->bytes[1] != request[1] || reply->bytes[2] != '0') {
        *code = "unexpected_reply_header"; return false;
    }
    for (size_t i = 3; i < 7; ++i) if (!is_hex(reply->bytes[i])) { *code = "non_hex_reply"; return false; }
    if (reply->bytes[3] == 'F' && reply->bytes[4] == 'F' && reply->bytes[5] == 'F' && reply->bytes[6] >= 'C') {
        *code = "device_rejected_command"; return false;
    }
    return true;
}
static bool query(SLTransport *t, SLResult *r, const uint8_t *frame, const char *expected, char value[5]) {
    if (r->eventCount >= SL_EVENT_CAPACITY) { error(r, "event_capacity", "内部步骤超出限制，已停止。"); return false; }
    SLReply reply = {0};
    SLEvent *e = &r->events[r->eventCount++];
    memcpy(e->request, frame + 1, 6);
    t->exchange(t->context, frame, &reply);
    e->sent = reply.sent;
    if (reply.sent && (frame == englishFrame || frame == chineseFrame || frame == japaneseFrame)) {
        r->setterAttempted = true; r->stateMayHaveChanged = true;
    }
    if (reply.sent && frame == saveFrame) r->saveAttempted = true;
    e->replyPresent = reply.received;
    if (reply.received) {
        size_t available = reply.length < 7 ? reply.length : 7;
        for (size_t i = 0; i < available; ++i) snprintf(e->replyHex + i * 2, 3, "%02x", reply.bytes[i]);
        if (available == 7) {
            for (size_t i = 0; i < 6; ++i) {
                uint8_t c = reply.bytes[i + 1];
                e->reply[i] = c >= 32 && c <= 126 ? (char)c : '?';
            }
        }
    }
    const char *validationCode = NULL;
    e->validReply = valid_reply(frame, &reply, &validationCode);
    if (e->validReply) {
        memcpy(e->value, reply.bytes + 3, 4);
        if (value) memcpy(value, e->value, 5);
        /* Preserve an actually received valid synchronous save acknowledgment,
         * even if an unsolicited extra report subsequently aborts the session. */
        if (frame == saveFrame && strcmp(e->value, "0003") == 0) r->saveAcknowledged = true;
    }
    if (!reply.transportOK || !e->validReply) {
        e->errorCode = reply.errorCode ? reply.errorCode : validationCode;
        error(r, e->errorCode, "设备通信异常，已停止。请关闭世光软件，检查连接后重新检测。");
        return false;
    }
    if (expected && strcmp(e->value, expected) != 0) {
        e->errorCode = "unexpected_reply_value";
        error(r, e->errorCode, "设备返回值与已验证流程不符，已停止。");
        return false;
    }
    e->success = true;
    return true;
}
static bool read_language(SLTransport *t, SLResult *r, int expected) {
    char value[5] = {0};
    if (!query(t, r, readFrame, NULL, value)) return false;
    if (strcmp(value, "0000") != 0 && strcmp(value, "0001") != 0 && strcmp(value, "0002") != 0) {
        r->events[r->eventCount - 1].success = false;
        r->events[r->eventCount - 1].errorCode = "unsupported_language";
        if (!r->setterAttempted) unsupported(r, "unsupported_language", "当前语言状态尚未纳入此版本支持范围，未修改设备。");
        else error(r, "unexpected_language", "语言核验未通过。");
        return false;
    }
    r->language = value[3] - '0';
    if (expected >= 0 && r->language != expected) {
        r->events[r->eventCount - 1].success = false;
        r->events[r->eventCount - 1].errorCode = "language_readback_mismatch";
        error(r, "language_readback_mismatch", "语言读回结果不一致，已停止。"); return false;
    }
    return true;
}
void sl_run(SLTransport *t, SLOperation operation, const char *expectedSerial, SLResult *r) {
    sl_initialize_result(r, operation);
    const SLLanguageOperation *target = find_language_operation(operation);
    if (operation != SL_INSPECT && !target) {
        error(r, "invalid_operation", "不支持此操作。"); return;
    }
    if (operation != SL_INSPECT && !sl_safe_serial(expectedSerial)) {
        error(r, "invalid_serial_argument", "必须指定刚刚检测到的有效序列号。"); return;
    }
    SLDevice device = {0}; size_t count = 0; const char *code = NULL;
    if (!t || !t->enumerate || !t->open || !t->exchange || !t->close) {
        error(r, "invalid_transport", "无法初始化设备连接。"); return;
    }
    if (!t->enumerate(t->context, &device, &count, &code)) { error(r, code, "无法读取 USB 设备列表。"); return; }
    if (count != 1) {
        error(r, count == 0 ? "device_not_found" : "multiple_devices",
              count == 0 ? "未找到 L-858D，请用数据线连接并开机。" : "检测到多台 L-858D，请只连接一台后再试。"); return;
    }
    if (device.vendor != 0x0a41 || device.product != 0x7004 || device.usagePage != 65440 ||
        device.inputSize != 7 || device.outputSize != 36 || !sl_safe_serial(device.serial)) {
        unsupported(r, "device_identity_mismatch", "设备识别信息不符合已验证的 L-858D，未修改设备。"); return;
    }
    memcpy(r->serial, device.serial, strlen(device.serial) + 1);
    if (operation != SL_INSPECT && strcmp(expectedSerial, device.serial) != 0) {
        error(r, "serial_mismatch", "连接的测光表已更换，请重新检测后再切换。"); return;
    }
    if (!t->open(t->context, &device, &code)) {
        error(r, code, "无法独占连接测光表，请退出世光软件和其他测光表工具后重试。"); return;
    }
    if (!query(t, r, firmwareFrame, NULL, r->firmware)) goto done;
    if (!known_firmware(r->firmware)) {
        unsupported(r, "unsupported_firmware", "仅支持 17、18、19 版固件。请先使用世光官方软件（Data Transfer Software）检查是否可升级到 19 版；如支持，请完成升级后再切换语言。本次未修改设备。"); goto done;
    }
    if (!query(t, r, regionFrame, NULL, r->region)) goto done;
    const SLFirmwareProfile *profile = find_profile(r->firmware, r->region);
    if (!profile) {
        unsupported(r, "unsupported_region", "此版本仅支持已审核的地区代码 0000、0001、0002、0003，未修改设备。"); goto done;
    }
    r->verificationLevel = profile->verificationLevel;
    if (!query(t, r, unlockFrame, "0000", NULL) || !query(t, r, selectFrame, "0012", NULL) ||
        !read_language(t, r, -1)) goto done;
    r->languageWriteSupported = true;
    if (operation == SL_INSPECT) {
        r->success = true; r->outcome = "inspected";
        r->message = "设备检查通过，可以切换中文、英文或日文。";
        goto done;
    }
    if (r->language == r->targetLanguage) {
        r->success = true; r->outcome = "already_target";
        r->message = target->alreadyMessage;
        goto done;
    }
    if (!query(t, r, target->frame, target->expected, NULL)) goto done;
    if (!read_language(t, r, r->targetLanguage)) goto done;
    if (!query(t, r, saveFrame, "0003", NULL)) goto done;
    if (!query(t, r, selectFrame, "0012", NULL) || !read_language(t, r, r->targetLanguage) ||
        !query(t, r, regionFrame, r->region, NULL) || !query(t, r, firmwareFrame, r->firmware, NULL)) goto done;
    r->success = true; r->outcome = "changed";
    r->message = target->changedMessage;
done:
    t->close(t->context);
}
static void json_string(FILE *s, const char *str) {
    if (!str) { fputs("null", s); return; }
    fputc('"', s);
    for (const unsigned char *p = (const unsigned char *)str; *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', s); fputc(*p, s); }
        else if (*p < 0x20) fprintf(s, "\\u%04x", *p);
        else fputc(*p, s); /* Preserve UTF-8 Chinese. */
    }
    fputc('"', s);
}
static const char *boolean(bool value) { return value ? "true" : "false"; }
void sl_print_json(FILE *s, const SLResult *r) {
    fprintf(s, "{\"success\":%s,\"outcome\":", boolean(r->success)); json_string(s, r->outcome);
    fputs(",\"serial\":", s); json_string(s, *r->serial ? r->serial : NULL);
    fputs(",\"firmware\":", s); json_string(s, *r->firmware ? r->firmware : NULL);
    fputs(",\"region\":", s); json_string(s, *r->region ? r->region : NULL);
    fputs(",\"verificationLevel\":", s); json_string(s, r->verificationLevel);
    fprintf(s, ",\"languageWriteSupported\":%s,\"supportedLanguages\":%s", boolean(r->languageWriteSupported),
            r->languageWriteSupported ? "[0,1,2]" : "[]");
    fputs(",\"language\":", s); if (r->language < 0) fputs("null", s); else fprintf(s, "%d", r->language);
    fputs(",\"targetLanguage\":", s); if (r->targetLanguage < 0) fputs("null", s); else fprintf(s, "%d", r->targetLanguage);
    fprintf(s, ",\"setterAttempted\":%s,\"saveAttempted\":%s,\"saveAcknowledged\":%s,\"stateMayHaveChanged\":%s,\"errorCode\":",
        boolean(r->setterAttempted), boolean(r->saveAttempted), boolean(r->saveAcknowledged), boolean(r->stateMayHaveChanged));
    json_string(s, r->errorCode); fputs(",\"message\":", s); json_string(s, r->message); fputs(",\"events\":[", s);
    for (size_t i = 0; i < r->eventCount; ++i) {
        const SLEvent *e = &r->events[i]; if (i) fputc(',', s);
        fputs("{\"request\":", s); json_string(s, e->request);
        fputs(",\"reply\":", s); json_string(s, e->replyPresent ? e->reply : NULL);
        fputs(",\"replyHex\":", s); json_string(s, e->replyPresent ? e->replyHex : NULL);
        fputs(",\"value\":", s); json_string(s, e->validReply ? e->value : NULL);
        fprintf(s, ",\"sent\":%s,\"success\":%s,\"errorCode\":", boolean(e->sent), boolean(e->success)); json_string(s, e->errorCode); fputc('}', s);
    }
    fputs("]}\n", s);
}
int sl_exit_status(const SLResult *r) { return r->success ? 0 : strcmp(r->outcome, "unsupported") == 0 ? 3 : 2; }
