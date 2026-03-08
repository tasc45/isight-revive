#ifndef ISIGHTREVIVE_LOGGING_H
#define ISIGHTREVIVE_LOGGING_H

#include <os/log.h>

#define ISR_LOG_SUBSYSTEM "com.isightrevive.dal"

static inline os_log_t ISRLog() {
    static os_log_t log = nullptr;
    if (!log) {
        log = os_log_create(ISR_LOG_SUBSYSTEM, "plugin");
    }
    return log;
}

#define ISR_LOG(fmt, ...) os_log(ISRLog(), fmt, ##__VA_ARGS__)
#define ISR_LOG_ERROR(fmt, ...) os_log_error(ISRLog(), fmt, ##__VA_ARGS__)
#define ISR_LOG_DEBUG(fmt, ...) os_log_debug(ISRLog(), fmt, ##__VA_ARGS__)
#define ISR_LOG_INFO(fmt, ...) os_log_info(ISRLog(), fmt, ##__VA_ARGS__)

#endif // ISIGHTREVIVE_LOGGING_H
