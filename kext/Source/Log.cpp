#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

#include "../../include/RHVRing.h"
#include "KernelDecls.hpp"
#include "Log.hpp"

namespace rhv {

RHVStats gStats;

namespace log {
namespace {

const uint32_t kRingSize = 64 * 1024;
const uint32_t kLineMax  = 256;

char          gBuf[kRingSize];
RHVRing       gRing;
IOSimpleLock *gLock;
uint64_t      gStartTicks;

#ifdef RHV_DEBUG
volatile uint32_t gLevel = RHV_LOG_DEBUG;
#else
volatile uint32_t gLevel = RHV_LOG_INFO;
#endif
uint32_t gIoLogLevel = RHV_LOG_INFO;

const char kLevelChar[] = { '-', 'E', 'W', 'I', 'D', 'T' };

void ringAppend(const char *line, uint32_t len)
{
    if (!gLock)
        return;
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(gLock);
    rhv_ring_write(&gRing, line, len);
    IOSimpleLockUnlockEnableInterrupt(gLock, st);
}

void vwrite(uint32_t level, bool sync, const char *fmt, va_list ap)
{
    if (!sync && level > gLevel)
        return;
    if (level > RHV_LOG_TRACE)
        level = RHV_LOG_TRACE;

    char line[kLineMax];

    /* mach_absolute_time() is nanoseconds on Intel-style timebases. */
    uint64_t us = (mach_absolute_time() - gStartTicks) / 1000;
    int n = snprintf(line, sizeof(line), "[%5llu.%06llu] %c cpu%d ",
                     (unsigned long long)(us / 1000000), (unsigned long long)(us % 1000000),
                     kLevelChar[level], cpu_number());
    if (n < 0 || n >= (int)sizeof(line) - 2)
        return;

    int m = vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    if (m < 0)
        m = 0;
    int len = n + m;
    if (len > (int)sizeof(line) - 2)
        len = (int)sizeof(line) - 2;   /* truncated */
    if (len == 0 || line[len - 1] != '\n')
        line[len++] = '\n';
    line[len] = '\0';

    ringAppend(line, (uint32_t)len);
    OSAddAtomic64(len, (volatile SInt64 *)&gStats.logBytesTotal);

    if (sync || level <= gIoLogLevel)
        IOLog("RyzenHV: %s", line);
}

} // namespace

void init()
{
    if (!gLock) {
        gLock = IOSimpleLockAlloc();
        rhv_ring_init(&gRing, gBuf, kRingSize);
        gStartTicks = mach_absolute_time();
    }

    uint32_t v = 0;
    if (PE_parse_boot_argn("rhv_loglevel", &v, sizeof(v)) && v <= RHV_LOG_TRACE)
        gLevel = v;
    if (PE_parse_boot_argn("rhv_iolog", &v, sizeof(v)) && v <= RHV_LOG_TRACE)
        gIoLogLevel = v;
}

void write(uint32_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwrite(level, false, fmt, ap);
    va_end(ap);
}

void writeSync(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwrite(RHV_LOG_INFO, true, fmt, ap);
    va_end(ap);
}

void assertFailed(const char *expr, const char *file, int line)
{
    RHV_ERR("ASSERT FAILED: %s (%s:%d)", expr, file, line);
    OSIncrementAtomic64((volatile SInt64 *)&gStats.assertFailures);
}

void read(uint64_t cursor, RHVLogChunk *out)
{
    bzero(out, sizeof(*out));
    if (!gLock)
        return;

    IOInterruptState st = IOSimpleLockLockDisableInterrupt(gLock);
    out->length = rhv_ring_read(&gRing, cursor, out->data, RHV_LOG_CHUNK_DATA, &out->nextCursor,
                                &out->dropped);
    IOSimpleLockUnlockEnableInterrupt(gLock, st);
}

void setLevel(uint32_t level)
{
    if (level <= RHV_LOG_TRACE)
        gLevel = level;
}

uint32_t level()
{
    return gLevel;
}

} // namespace log
} // namespace rhv
