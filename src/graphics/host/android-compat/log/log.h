#pragma once
#include <android/log.h>
/* liblog's platform macro expressed through its public NDK entry point. */
#define LOG_PRI(priority, tag, ...) __android_log_print(priority, tag, __VA_ARGS__)
