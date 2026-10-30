/*
 * rhv-info - talks to the loaded RyzenHV kext.
 *
 *   rhv-info                 CPU / SVM summary (same as `info`)
 *   rhv-info info
 *   rhv-info log [-f]        dump the kernel log ring, -f keeps following it
 *   rhv-info stats           event counters
 *   rhv-info level <0-5>     set kernel log level (root)
 *   rhv-info selftest        verify SVM state on every CPU (root, needs rhv_enable_svm=1)
 */
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../include/RyzenHVShared.h"

static io_connect_t connect_kext(void)
{
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching(RHV_SERVICE_NAME));
    if (!svc) {
        fprintf(stderr, "RyzenHV service not found. Is the kext loaded? (see docs/RYZENTOSH.md)\n");
        exit(1);
    }

    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
        exit(1);
    }
    return conn;
}

static void print_features(uint32_t f)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } kFeatures[] = {
        { RHV_SVM_FEAT_NP, "NPT" },
        { RHV_SVM_FEAT_LBRV, "LbrVirt" },
        { RHV_SVM_FEAT_SVML, "SVMLock" },
        { RHV_SVM_FEAT_NRIPS, "NRIPS" },
        { RHV_SVM_FEAT_TSCRATE, "TscRateMsr" },
        { RHV_SVM_FEAT_VMCB_CLEAN, "VmcbClean" },
        { RHV_SVM_FEAT_FLUSH_BY_ASID, "FlushByAsid" },
        { RHV_SVM_FEAT_DECODE_ASSISTS, "DecodeAssists" },
        { RHV_SVM_FEAT_PAUSE_FILTER, "PauseFilter" },
        { RHV_SVM_FEAT_PAUSE_THRESH, "PauseFilterThreshold" },
        { RHV_SVM_FEAT_AVIC, "AVIC" },
        { RHV_SVM_FEAT_VVMSAVE, "vVMSAVE/VMLOAD" },
        { RHV_SVM_FEAT_VGIF, "vGIF" },
    };

    printf("  features     : 0x%08x\n", f);
    for (size_t i = 0; i < sizeof(kFeatures) / sizeof(kFeatures[0]); i++)
        printf("    [%c] %s\n", (f & kFeatures[i].bit) ? 'x' : ' ', kFeatures[i].name);
}

static int cmd_info(io_connect_t conn)
{
    RHVInfo info;
    memset(&info, 0, sizeof(info));
    size_t outSize = sizeof(info);
    kern_return_t kr = IOConnectCallStructMethod(conn, kRHVMethodGetInfo, NULL, 0, &info, &outSize);
    if (kr != KERN_SUCCESS || outSize != sizeof(info)) {
        fprintf(stderr, "GetInfo failed: 0x%x (size %zu, expected %zu)\n", kr, outSize,
                sizeof(info));
        return 1;
    }

    if (info.abiVersion != RHV_ABI_VERSION)
        fprintf(stderr, "warning: kext ABI %u, tool ABI %u (rebuild one of them)\n",
                info.abiVersion, RHV_ABI_VERSION);

    printf("RyzenHV kext (ABI %u)\n", info.abiVersion);
    printf("  CPU          : %s family 0x%x model 0x%x stepping %u\n",
           info.vendorIsAmd ? "AMD" : "non-AMD", info.cpuFamily, info.cpuModel, info.cpuStepping);
    printf("  logical CPUs : %u (SVM usable on %u, active on %u)\n", info.nCpus,
           info.nCpusSvmUsable, info.nCpusSvmActive);
    printf("  SVM          : %s\n", info.svmSupported ? "supported" : "NOT supported");
    printf("  BIOS disable : %s\n", info.svmDisabledByBios ? "YES (enable 'SVM Mode' in BIOS)" : "no");
    printf("  VM_CR.LOCK   : %u\n", info.svmLocked);
    printf("  SVM revision : %u\n", info.svmRevision);
    printf("  ASIDs        : %u\n", info.numAsids);
    print_features(info.svmFeatures);
    return 0;
}

static int cmd_log(io_connect_t conn, int follow)
{
    static RHVLogChunk chunk;
    uint64_t cursor = 0;

    for (;;) {
        size_t outSize = sizeof(chunk);
        kern_return_t kr = IOConnectCallMethod(conn, kRHVMethodReadLog, &cursor, 1, NULL, 0, NULL,
                                               NULL, &chunk, &outSize);
        if (kr != KERN_SUCCESS || outSize != sizeof(chunk)) {
            fprintf(stderr, "ReadLog failed: 0x%x (size %zu, expected %zu)\n", kr, outSize,
                    sizeof(chunk));
            return 1;
        }

        if (chunk.dropped)
            printf("[... %llu bytes lost, ring wrapped before they were read ...]\n",
                   (unsigned long long)chunk.dropped);
        if (chunk.length) {
            fwrite(chunk.data, 1, chunk.length, stdout);
            fflush(stdout);
        }
        cursor = chunk.nextCursor;

        if (chunk.length == 0) {
            if (!follow)
                return 0;
            usleep(200 * 1000);
        }
    }
}

static int cmd_stats(io_connect_t conn)
{
    RHVStats s;
    memset(&s, 0, sizeof(s));
    size_t outSize = sizeof(s);
    kern_return_t kr = IOConnectCallStructMethod(conn, kRHVMethodGetStats, NULL, 0, &s, &outSize);
    if (kr != KERN_SUCCESS || outSize != sizeof(s)) {
        fprintf(stderr, "GetStats failed: 0x%x (size %zu, expected %zu)\n", kr, outSize, sizeof(s));
        return 1;
    }

#define P(f) printf("  %-16s: %llu\n", #f, (unsigned long long)s.f)
    puts("RyzenHV counters");
    P(probeRuns);
    P(enableCalls);
    P(enableCpusOk);
    P(enableFailures);
    P(disableCalls);
    P(sleepEvents);
    P(wakeEvents);
    P(clientOpens);
    P(clientCalls);
    P(assertFailures);
    P(selftestRuns);
    P(selftestCpusOk);
    P(logBytesTotal);
    P(logLevel);
    P(vmCreates);
    P(vmRuns);
    P(vmExits);
    P(vmInvalidExits);
    P(vmMaps);
    P(vmMapFailures);
    P(vmUnmaps);
    P(vmProtects);
    P(vmNptFaults);
    P(vmPagesMapped);
    P(vmFpuMismatches);
#undef P
    return 0;
}

static int cmd_level(io_connect_t conn, const char *arg)
{
    char *end = NULL;
    unsigned long level = strtoul(arg, &end, 10);
    if (!*arg || *end || level > RHV_LOG_TRACE) {
        fprintf(stderr, "level must be 0..%d (none, error, warn, info, debug, trace)\n",
                RHV_LOG_TRACE);
        return 2;
    }

    uint64_t in = level;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRHVMethodSetLogLevel, &in, 1, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "SetLogLevel failed: 0x%x%s\n", kr,
                getuid() ? " (run with sudo)" : "");
        return 1;
    }
    printf("kernel log level is now %lu\n", level);
    return 0;
}

static int cmd_hv(io_connect_t conn, int argc, char **argv)
{
    uint64_t in[2] = { kRHVHvGet, 0 }, out[2] = { 0, 0 };
    if (argc > 2 && !strcmp(argv[2], "mode") && argc > 3) {
        in[0] = kRHVHvMode;
        in[1] = strtoul(argv[3], NULL, 10);
    } else if (argc > 2 && !strcmp(argv[2], "sysctl") && argc > 3) {
        in[0] = kRHVHvSysctl;
        in[1] = strtoul(argv[3], NULL, 10);
    } else if (argc > 2 && !strcmp(argv[2], "tpr") && argc > 3) {
        in[0] = kRHVHvTpr;
        in[1] = strtoul(argv[3], NULL, 10);
    } else if (argc > 3 && !strcmp(argv[2], "svmlimit")) {
        /* debugging: the SVM code forgets the CPUs from index N up, like a kext that started before they were online */
        in[0] = kRHVHvSvmLimit;
        in[1] = strtoul(argv[3], NULL, 10);
    } else if (argc > 2 && !strcmp(argv[2], "rescan")) {
        in[0] = kRHVHvSvmRescan;
    } else if (argc > 2 && strcmp(argv[2], "status")) {
        fprintf(stderr, "usage: rhv-info hv [status | mode 0|1|2 | sysctl 0|1 | tpr 0|1 | svmlimit N | rescan]\n");
        return 2;
    }
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRHVMethodHvControl, in, 2, out, &(uint32_t){ 2 });
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "HvControl failed: 0x%x%s\n", kr, getuid() ? " (run with sudo)" : "");
        return 1;
    }
    if (in[0] == kRHVHvSvmRescan)
        printf("rescan: SVM turned on for %llu more CPUs (see `rhv-info` for the totals)\n", (unsigned long long)out[1]);
    else if (in[0] == kRHVHvSvmLimit)
        printf("the SVM code now knows %llu CPUs (see `rhv-info`); a VM start or `rhv-info hv rescan` brings the others back\n", (unsigned long long)in[1]);
    else
    printf("hv front end: %s, mode %llu, kern.hv_support %s, TPR shadow %s\n", (out[0] & kRHVHvStateRegistered) ? "registered" : "not registered",
           (unsigned long long)out[1], (out[0] & kRHVHvStateSysctl) ? "replaced (shows 1)" : "untouched",
           (out[0] & kRHVHvStateTpr) ? "offered" : "not offered");
    return 0;
}

static int cmd_selftest(io_connect_t conn)
{
    uint64_t out[2] = { 0, 0 };
    uint32_t outCnt = 2;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRHVMethodSelfTest, NULL, 0, out, &outCnt);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "SelfTest failed: 0x%x%s\n", kr, getuid() ? " (run with sudo)" : "");
        return 1;
    }

    if (out[1] == 0) {
        puts("selftest: SVM is not active (boot with rhv_enable_svm=1)");
        return 1;
    }
    printf("selftest: %llu of %llu CPUs ok\n", (unsigned long long)out[0],
           (unsigned long long)out[1]);
    puts("details: rhv-info log");
    return out[0] == out[1] ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "info";

    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        puts("usage: rhv-info [info | log [-f] | stats | level <0-5> | selftest | hv [status | mode 0|1|2 | sysctl 0|1 | tpr 0|1]]");
        return 0;
    }

    io_connect_t conn = connect_kext();
    int rc;

    if (!strcmp(cmd, "info"))
        rc = cmd_info(conn);
    else if (!strcmp(cmd, "log"))
        rc = cmd_log(conn, argc > 2 && !strcmp(argv[2], "-f"));
    else if (!strcmp(cmd, "stats"))
        rc = cmd_stats(conn);
    else if (!strcmp(cmd, "level") && argc > 2)
        rc = cmd_level(conn, argv[2]);
    else if (!strcmp(cmd, "selftest"))
        rc = cmd_selftest(conn);
    else if (!strcmp(cmd, "hv"))
        rc = cmd_hv(conn, argc, argv);
    else {
        fprintf(stderr, "unknown command '%s', try: rhv-info help\n", cmd);
        rc = 2;
    }

    IOServiceClose(conn);
    return rc;
}
