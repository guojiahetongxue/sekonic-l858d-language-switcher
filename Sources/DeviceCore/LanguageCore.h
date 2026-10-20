#ifndef SEKONIC_LANGUAGE_CORE_H
#define SEKONIC_LANGUAGE_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { SL_SERIAL_CAPACITY = 128, SL_EVENT_CAPACITY = 12, SL_FRAME_SIZE = 7 };
typedef enum { SL_INSPECT, SL_SET_CHINESE, SL_SET_JAPANESE, SL_SET_ENGLISH } SLOperation;
typedef struct {
    long vendor, product, usagePage, inputSize, outputSize;
    char serial[SL_SERIAL_CAPACITY];
} SLDevice;
typedef struct {
    bool transportOK, received, sent;
    uint32_t reportID;
    unsigned reportType;
    size_t length;
    uint8_t bytes[SL_FRAME_SIZE];
    const char *errorCode;
} SLReply;
/* enumerate reports the count of all VID/PID matches, including invalid ones.
 * Only the sole match can be opened. open must obtain exclusive ownership.
 * exchange and open latch errors: a session never recovers or retries. */
typedef struct {
    void *context;
    bool (*enumerate)(void *, SLDevice *, size_t *, const char **);
    bool (*open)(void *, const SLDevice *, const char **);
    void (*exchange)(void *, const uint8_t[SL_FRAME_SIZE], SLReply *);
    void (*close)(void *);
} SLTransport;
typedef struct {
    char request[7], reply[7], replyHex[15], value[5];
    bool replyPresent, validReply, success, sent;
    const char *errorCode;
} SLEvent;
typedef struct {
    bool success;
    const char *outcome, *errorCode, *message, *verificationLevel;
    char serial[SL_SERIAL_CAPACITY], firmware[5], region[5];
    int language, targetLanguage;
    bool setterAttempted, saveAttempted, saveAcknowledged, stateMayHaveChanged;
    bool languageWriteSupported;
    size_t eventCount;
    SLEvent events[SL_EVENT_CAPACITY];
} SLResult;
bool sl_safe_serial(const char *serial);
void sl_initialize_result(SLResult *result, SLOperation operation);
void sl_run(SLTransport *transport, SLOperation operation,
            const char *expectedSerial, SLResult *result);
void sl_print_json(FILE *stream, const SLResult *result);
int sl_exit_status(const SLResult *result);
#endif
