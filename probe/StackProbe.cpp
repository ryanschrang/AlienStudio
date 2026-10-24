// Temporary CI probe: how much stack the SEH dispatcher has left on a stack overflow, per runner CPU.
#include <windows.h>
#include <intrin.h>
#include <malloc.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
static volatile int sink;
static ULONG_PTR lo, hi;
static volatile uintptr_t headroom, faultSp;
static int filt(unsigned long code, EXCEPTION_POINTERS* info) {
    volatile char here = 0;
    headroom = (uintptr_t)&here - lo;
    faultSp = info->ContextRecord->Rsp - lo;
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
struct Recurse { static int go(int depth) { volatile char pad[512]; pad[0] = (char)depth; return go(depth + 1) + pad[0]; } };
static int run() {
    __try { sink = Recurse::go(0); return 1; }
    __except (filt(GetExceptionCode(), GetExceptionInformation())) { return 100 + _resetstkoflw(); }
}
int main(int argc, char**) {
    int r[4]; char brand[49] = {};
    for (int i = 0; i < 3; ++i) { __cpuid(r, 0x80000002 + i); memcpy(brand + 16 * i, r, 16); }
    __cpuidex(r, 0xD, 0);
    GetCurrentThreadStackLimits(&lo, &hi);
    if (argc > 1) { ULONG want = 65536; SetThreadStackGuarantee(&want); }
    ULONG g = 0; SetThreadStackGuarantee(&g);
    DWORD len = 0; InitializeContext(nullptr, CONTEXT_ALL | CONTEXT_XSTATE, nullptr, &len);
    printf("cpu=[%s] xsaveBytes(cpuid)=%d osXState=0x%llx contextLen=%lu guarantee=%lu\n", brand, r[1],
           (unsigned long long)GetEnabledXStateFeatures(), len, g);
    fflush(stdout);
    for (int i = 0; i < 3; ++i) {
        int k = run();
        printf("overflow %d: result=%d filterHeadroom=%llu faultRspAboveLimit=%llu\n", i, k, (unsigned long long)headroom, (unsigned long long)faultSp);
        fflush(stdout);
    }
}
