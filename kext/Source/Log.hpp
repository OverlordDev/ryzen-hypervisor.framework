/*
 * Log.hpp - kernel-side logging for the kext.
 *
 * Messages go into a 64 KiB ring buffer that userland reads with
 * `rhv-info log [-f]`, and are mirrored to IOLog (system log) up to the
 * level given by boot-arg rhv_iolog (default INFO).
 *
 * Boot-args:
 *   rhv_loglevel=N   ring level, 0..5 (none, error, warn, info, debug, trace)
 *   rhv_iolog=N      IOLog mirror level, same scale
 *
 * Rules for call sites:
 *   - never log from a mp_rendezvous action or any interrupt-disabled context
 *   - use RHV_SYNC right BEFORE something that can panic: it always reaches
 *     IOLog immediately, so the last line before a crash is visible
 */
#ifndef RYZENHV_LOG_HPP
#define RYZENHV_LOG_HPP

#include <stdint.h>
#include <libkern/OSAtomic.h>
#include "../../include/RyzenHVShared.h"

namespace rhv {

/* Defined in Log.cpp. */
extern RHVStats gStats;

namespace log {

void     init();
void     write(uint32_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void     writeSync(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void     assertFailed(const char *expr, const char *file, int line);

/* Fills a RHVLogChunk from `cursor`. `out` must point to a full RHV_LOG_CHUNK_SIZE buffer. */
void     read(uint64_t cursor, RHVLogChunk *out);

void     setLevel(uint32_t level);
uint32_t level();

} // namespace log
} // namespace rhv

/* Atomic bumps of the counters exposed by `rhv-info stats`. */
#define RHV_STAT_INC(field) OSIncrementAtomic64((volatile SInt64 *)&rhv::gStats.field)
#define RHV_STAT_ADD(field, n) OSAddAtomic64((SInt64)(n), (volatile SInt64 *)&rhv::gStats.field)

#define RHV_ERR(...)   rhv::log::write(RHV_LOG_ERROR, __VA_ARGS__)
#define RHV_WARN(...)  rhv::log::write(RHV_LOG_WARN, __VA_ARGS__)
#define RHV_INFO(...)  rhv::log::write(RHV_LOG_INFO, __VA_ARGS__)
#define RHV_DBG(...)   rhv::log::write(RHV_LOG_DEBUG, __VA_ARGS__)
#define RHV_TRACE(...) rhv::log::write(RHV_LOG_TRACE, __VA_ARGS__)
#define RHV_SYNC(...)  rhv::log::writeSync(__VA_ARGS__)

/* Does not panic: logs, counts, and lets you see it in `rhv-info stats`. */
#define RHV_ASSERT(cond)                                                    \
    do {                                                                    \
        if (!(cond))                                                        \
            rhv::log::assertFailed(#cond, __FILE__, __LINE__);              \
    } while (0)

#endif /* RYZENHV_LOG_HPP */
