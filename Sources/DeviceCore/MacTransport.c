#include "MacTransport.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDLib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    IOHIDManagerRef manager;
    CFSetRef devices;
    IOHIDDeviceRef device;
    CFRunLoopRef loop;
    bool opened, scheduled, waiting, failed, received;
    const char *errorCode;
    uint8_t inputBuffer[SL_FRAME_SIZE];
    SLReply reply;
} MacContext;
static bool number_property(IOHIDDeviceRef device, CFStringRef key, long *out) {
    CFTypeRef p = IOHIDDeviceGetProperty(device, key);
    return p && CFGetTypeID(p) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)p, kCFNumberLongType, out);
}
static bool get_identity(IOHIDDeviceRef device, SLDevice *out) {
    memset(out, 0, sizeof(*out));
    CFTypeRef serial = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDSerialNumberKey));
    return number_property(device, CFSTR(kIOHIDVendorIDKey), &out->vendor) &&
        number_property(device, CFSTR(kIOHIDProductIDKey), &out->product) &&
        number_property(device, CFSTR(kIOHIDPrimaryUsagePageKey), &out->usagePage) &&
        number_property(device, CFSTR(kIOHIDMaxInputReportSizeKey), &out->inputSize) &&
        number_property(device, CFSTR(kIOHIDMaxOutputReportSizeKey), &out->outputSize) &&
        serial && CFGetTypeID(serial) == CFStringGetTypeID() &&
        CFStringGetCString((CFStringRef)serial, out->serial, sizeof(out->serial), kCFStringEncodingUTF8);
}
static bool enumerate(void *context, SLDevice *out, size_t *count, const char **errorCode) {
    MacContext *c = context;
    if (c->manager) { *errorCode = "session_already_used"; return false; }
    c->manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!c->manager) { *errorCode = "manager_create_failed"; return false; }
    int vendor = 0x0a41, product = 0x7004;
    CFNumberRef vendorNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
    CFNumberRef productNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &product);
    if (!vendorNumber || !productNumber) {
        if (vendorNumber) CFRelease(vendorNumber);
        if (productNumber) CFRelease(productNumber);
        *errorCode = "allocation_failed"; return false;
    }
    const void *keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey)};
    const void *values[] = {vendorNumber, productNumber};
    CFDictionaryRef match = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(vendorNumber); CFRelease(productNumber);
    if (!match) { *errorCode = "allocation_failed"; return false; }
    IOHIDManagerSetDeviceMatching(c->manager, match); CFRelease(match);
    c->devices = IOHIDManagerCopyDevices(c->manager);
    *count = c->devices ? (size_t)CFSetGetCount(c->devices) : 0;
    if (*count == 1) {
        CFSetGetValues(c->devices, (const void **)&c->device);
        if (!get_identity(c->device, out)) { *errorCode = "missing_device_identity"; return false; }
    }
    return true;
}
static void input_callback(void *context, IOReturn result, void *sender,
                           IOHIDReportType type, uint32_t reportID,
                           uint8_t *report, CFIndex length) {
    (void)sender;
    MacContext *c = context;
    if (!c->waiting || c->received || c->failed) {
        c->failed = true; c->errorCode = "unsolicited_or_duplicate_input"; return;
    }
    if (result != kIOReturnSuccess) { c->failed = true; c->errorCode = "input_callback_error"; return; }
    c->received = true; c->reply.received = true;
    c->reply.reportType = (unsigned)type; c->reply.reportID = reportID;
    c->reply.length = length >= 0 ? (size_t)length : 0;
    if (report && length > 0) {
        size_t copied = (size_t)length < SL_FRAME_SIZE ? (size_t)length : SL_FRAME_SIZE;
        memcpy(c->reply.bytes, report, copied);
    }
    if (!report || type != kIOHIDReportTypeInput || length != SL_FRAME_SIZE) {
        c->failed = true; c->errorCode = "unexpected_reply_type_or_length";
    }
}
static bool open_device(void *context, const SLDevice *expected, const char **errorCode) {
    MacContext *c = context;
    if (!c->device || c->opened) { *errorCode = "invalid_device_session"; return false; }
    IOReturn result = IOHIDDeviceOpen(c->device, kIOHIDOptionsTypeSeizeDevice);
    if (result != kIOReturnSuccess) { *errorCode = "exclusive_open_failed"; return false; }
    c->opened = true;
    SLDevice current = {0};
    if (!get_identity(c->device, &current) || current.vendor != expected->vendor || current.product != expected->product ||
        current.usagePage != expected->usagePage || current.inputSize != expected->inputSize ||
        current.outputSize != expected->outputSize || strcmp(current.serial, expected->serial) != 0) {
        *errorCode = "device_changed_during_open"; return false;
    }
    IOHIDDeviceRegisterInputReportCallback(c->device, c->inputBuffer, sizeof(c->inputBuffer), input_callback, c);
    c->loop = CFRunLoopGetCurrent();
    IOHIDDeviceScheduleWithRunLoop(c->device, c->loop, kCFRunLoopDefaultMode);
    c->scheduled = true;
    /* Any stale input aborts before the first request. */
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    if (c->failed) { *errorCode = c->errorCode; return false; }
    return true;
}
static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}
static void exchange(void *context, const uint8_t request[SL_FRAME_SIZE], SLReply *reply) {
    MacContext *c = context;
    memset(reply, 0, sizeof(*reply));
    if (!c->opened || !c->scheduled || c->failed) {
        reply->errorCode = c->errorCode ? c->errorCode : "invalid_device_session"; return;
    }
    memset(&c->reply, 0, sizeof(c->reply));
    c->waiting = true; c->received = false;
    c->reply.sent = true;
    IOReturn sent = IOHIDDeviceSetReport(c->device, kIOHIDReportTypeOutput, 1, request, SL_FRAME_SIZE);
    if (sent != kIOReturnSuccess) {
        c->failed = true; c->errorCode = "output_report_failed";
    } else {
        double deadline = monotonic_seconds() + 5.0;
        while (!c->received && !c->failed) {
            double remaining = deadline - monotonic_seconds();
            if (remaining <= 0) break;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, remaining < 0.02 ? remaining : 0.02, true);
        }
        if (!c->received && !c->failed) { c->failed = true; c->errorCode = "reply_timeout"; }
    }
    c->waiting = false;
    if (!c->failed) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    c->reply.transportOK = !c->failed;
    c->reply.errorCode = c->errorCode;
    *reply = c->reply;
}
static void close_device(void *context) {
    MacContext *c = context;
    if (c->scheduled) {
        IOHIDDeviceUnscheduleFromRunLoop(c->device, c->loop, kCFRunLoopDefaultMode);
        IOHIDDeviceRegisterInputReportCallback(c->device, c->inputBuffer, sizeof(c->inputBuffer), NULL, NULL);
        c->scheduled = false;
    }
    if (c->opened) { IOHIDDeviceClose(c->device, kIOHIDOptionsTypeNone); c->opened = false; }
}
bool sl_mac_create(SLTransport *t) {
    MacContext *c = calloc(1, sizeof(*c));
    if (!c) return false;
    *t = (SLTransport){.context = c, .enumerate = enumerate, .open = open_device, .exchange = exchange, .close = close_device};
    return true;
}
void sl_mac_destroy(SLTransport *t) {
    MacContext *c = t ? t->context : NULL;
    if (!c) return;
    close_device(c);
    if (c->devices) CFRelease(c->devices);
    if (c->manager) CFRelease(c->manager);
    free(c); memset(t, 0, sizeof(*t));
}
