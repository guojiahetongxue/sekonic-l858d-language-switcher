#include "LanguageCore.h"
#include "MacTransport.h"
#include <string.h>

int main(int argc, char **argv) {
    SLOperation operation = SL_INSPECT;
    const char *serial = NULL;
    bool valid = false;
    if (argc == 2 && strcmp(argv[1], "inspect") == 0) valid = true;
    else if (argc == 4 && strcmp(argv[2], "--serial") == 0 &&
             (strcmp(argv[1], "set-chinese") == 0 || strcmp(argv[1], "set-japanese") == 0 || strcmp(argv[1], "set-english") == 0)) {
        operation = strcmp(argv[1], "set-chinese") == 0 ? SL_SET_CHINESE :
                    strcmp(argv[1], "set-japanese") == 0 ? SL_SET_JAPANESE : SL_SET_ENGLISH;
        serial = argv[3]; valid = sl_safe_serial(serial);
    }
    SLResult result;
    sl_initialize_result(&result, operation);
    if (!valid) {
        result.errorCode = "invalid_arguments";
        result.message = "用法：helper inspect；helper set-chinese --serial 序列号；helper set-japanese --serial 序列号；helper set-english --serial 序列号。";
    } else {
        SLTransport transport = {0};
        if (!sl_mac_create(&transport)) {
            result.errorCode = "allocation_failed"; result.message = "无法分配设备通信资源。";
        } else {
            sl_run(&transport, operation, serial, &result);
            sl_mac_destroy(&transport);
        }
    }
    sl_print_json(stdout, &result);
    return sl_exit_status(&result);
}
