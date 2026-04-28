#ifndef IRON_DEBUG_LOG_H
#define IRON_DEBUG_LOG_H

#include <stdbool.h>

void debug_log_init(void);
void debug_log_close(void);
bool debug_log_enabled(void);
void debug_log(const char *fmt, ...);

#endif
