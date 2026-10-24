/*
 * SPDX-License-Identifier: MIT
 */

#ifndef BITOSCOPE_PLUGIN_COMMON_H
#define BITOSCOPE_PLUGIN_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BITOSCOPE_PLUGIN_SDK_VERSION "0.14.0"

#define BITOSCOPE_OK 0
#define BITOSCOPE_ERROR (-1)

int bitoscope_add_abbreviation(const char* upper, const char* shown, int also_word);

#define BITOSCOPE_LOG_DEBUG 1
#define BITOSCOPE_LOG_INFO 2
#define BITOSCOPE_LOG_WARNING 3
#define BITOSCOPE_LOG_ERROR 4

#if defined(__GNUC__) || defined(__clang__)
#define BITOSCOPE_PRINTF_FORMAT(f, a) __attribute__((format(printf, f, a)))
#else
#define BITOSCOPE_PRINTF_FORMAT(f, a)
#endif

void bitoscope_log(int level, const char* format, ...) BITOSCOPE_PRINTF_FORMAT(2, 3);

typedef struct bitoscope_reply bitoscope_reply;

int bitoscope_reply_message(bitoscope_reply* reply, const char* message);

#ifdef __cplusplus
}
#endif

#endif
