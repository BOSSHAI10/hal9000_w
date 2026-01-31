#include "test_common.h"
#include "test_thread.h"
#include "test_timer.h"
#include "test_priority_scheduler.h"
#include "test_priority_donation.h"
#include "cal_annotate.h"
#include "mutex.h"
#include "log.h"

typedef struct _TEST_THREAD_INFO { PTHREAD Thread; STATUS Status; } TEST_THREAD_INFO, *PTEST_THREAD_INFO;
extern STATUS MS_ABI FibonacciThreadFunc(IN_OPT PVOID Context);
typedef struct _FIB_DATA { int n; int result; } FIB_DATA, *PFIB_DATA;

FUNC_ThreadStart TestThreadYield;
FUNC_ThreadStart TestMutexes;
FUNC_ThreadStart TestCpuIntense;

static void (CDECL _ThreadPrepareFibonacci)(OUT_PTR PVOID* Context, IN DWORD NumberOfThreads, IN PVOID PrepareContext) {
    PFIB_DATA pFib; UNREFERENCED_PARAMETER(NumberOfThreads);
    pFib = ExAllocatePoolWithTag(PoolAllocatePanicIfFail | PoolAllocateZeroMemory, sizeof(FIB_DATA), HEAP_TEST_TAG, 0);
    pFib->n = (int)(QWORD)PrepareContext; *Context = pFib;
}

static void (CDECL _ThreadPostFinishFibonacci)(IN PVOID Context, IN DWORD NumberOfThreads) {
    PFIB_DATA pFib = (PFIB_DATA)Context; UNREFERENCED_PARAMETER(NumberOfThreads);
    if (pFib) Log(LogLevelError, "\n[RESULT] Fibonacci calculation finished. Result: %u\n", pFib->result);
}

static void (CDECL _ThreadTestPassContext)(OUT_PTR PVOID* Context, IN DWORD NumberOfThreads, IN PVOID PrepareContext) {
    PVOID pNew; UNREFERENCED_PARAMETER(NumberOfThreads);
    pNew = ExAllocatePoolWithTag(PoolAllocatePanicIfFail, sizeof(PVOID), HEAP_TEST_TAG, 0);
    memcpy(pNew, &PrepareContext, sizeof(PVOID)); *Context = pNew;
}

const THREAD_TEST THREADS_TEST[] = {
    { "Fibonacci", FibonacciThreadFunc, _ThreadPrepareFibonacci, (PVOID)5, NULL, _ThreadPostFinishFibonacci, ThreadPriorityDefault, FALSE, TRUE, FALSE },
    { "ThreadYield", TestThreadYield, NULL, NULL, NULL, NULL, ThreadPriorityDefault, FALSE, FALSE, FALSE }
};

const DWORD THREADS_TOTAL_NO_OF_TESTS = ARRAYSIZE(THREADS_TEST);

void TestThreadFunctionality(IN THREAD_TEST* ThreadTest, IN_OPT PVOID ContextForTestFunction, IN DWORD NumberOfThreads) {
    DWORD i; STATUS status; char name[MAX_PATH]; PTEST_THREAD_INFO info; PVOID pCtx = NULL;
    THREAD_PRIORITY prio = ThreadTest->BasePriority; BYTE incr = ThreadTest->IncrementPriorities ? 1 : 0;
    DWORD no = ThreadTest->IgnoreThreadCount ? 1 : NumberOfThreads;
    UNREFERENCED_PARAMETER(ContextForTestFunction);
    Log(LogLevelError, "[TEST] STARTING: %s\n", ThreadTest->TestName);
    info = ExAllocatePoolWithTag(PoolAllocatePanicIfFail | PoolAllocateZeroMemory, sizeof(TEST_THREAD_INFO) * no, HEAP_TEST_TAG, 0);
    if (ThreadTest->ThreadPrepareFunction) ThreadTest->ThreadPrepareFunction(&pCtx, no, ThreadTest->PrepareFunctionContext);
    for (i = 0; i < no; ++i) {
        snprintf(name, MAX_PATH, "%s-%02x", ThreadTest->TestName, i);
        status = ThreadCreate(name, prio, ThreadTest->ThreadFunction, ThreadTest->ArrayOfContexts ? ((PVOID*)pCtx)[i] : pCtx, &info[i].Thread);
        prio = (prio + incr) % ThreadPriorityReserved;
    }
    for (i = 0; i < no; ++i) { ThreadWaitForTermination(info[i].Thread, &info[i].Status); ThreadCloseHandle(info[i].Thread); }
    if (ThreadTest->ThreadPostFinishFunction) ThreadTest->ThreadPostFinishFunction(pCtx, no);
    Log(LogLevelError, "[TEST] FINISHED: %s\n", ThreadTest->TestName);
    ExFreePoolWithTag(info, HEAP_TEST_TAG);
}

void TestAllThreadFunctionalities(IN DWORD NumberOfThreads) {
    for (DWORD i = 0; i < THREADS_TOTAL_NO_OF_TESTS; ++i) TestThreadFunctionality((THREAD_TEST*)&THREADS_TEST[i], NULL, NumberOfThreads);
}

STATUS (CDECL MS_ABI TestThreadYield)(IN_OPT PVOID Context) {
    UNREFERENCED_PARAMETER(Context);
    for (DWORD i = 0; i < 5; ++i) { Log(LogLevelError, "[YIELD] iteration %d\n", i); ThreadYield(); }
    return STATUS_SUCCESS;
}
STATUS (CDECL MS_ABI TestMutexes)(IN_OPT PVOID Context) { UNREFERENCED_PARAMETER(Context); return STATUS_SUCCESS; }
STATUS (CDECL MS_ABI TestCpuIntense)(IN_OPT PVOID Context) { UNREFERENCED_PARAMETER(Context); return STATUS_SUCCESS; }