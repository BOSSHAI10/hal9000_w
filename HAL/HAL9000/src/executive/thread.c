#include "HAL9000.h"
#include "thread_internal.h"
#include "synch.h"
#include "cpumu.h"
#include "ex_event.h"
#include "core.h"
#include "vmm.h"
#include "exe_loader.h"
#include "process_internal.h"
#include "isr.h"
#include "gdtmu.h"
#include "pe_exports.h"
#include "cal_annotate.h"
#include "cal_atomic.h"
#include "cal_assembly.h"
#include "cal_seh.h"
#include "log.h"

#define TID_INCREMENT 8 

extern void MS_ABI ThreadStart();
typedef void (CDECL MS_ABI FUNC_ThreadSwitch)(OUT_PTR PVOID* OldStack, IN PVOID NewStack);
extern FUNC_ThreadSwitch ThreadSwitch;

typedef struct _THREAD_SYSTEM_DATA {
    LOCK AllThreadsLock;
    _Guarded_by_(AllThreadsLock) LIST_ENTRY AllThreadsList;
    LOCK ReadyThreadsLock;
    _Guarded_by_(ReadyThreadsLock) LIST_ENTRY ReadyThreadsList;
} THREAD_SYSTEM_DATA, *PTHREAD_SYSTEM_DATA;

static THREAD_SYSTEM_DATA m_threadSystemData;

ALWAYS_INLINE static TID _ThreadSystemGetNextTid(void) {
    static volatile TID __currentTid = 0;
    // Returnează 8, 16, 24...
    return AtomicExchangeAdd64(&__currentTid, TID_INCREMENT) + 8;
}

static STATUS _ThreadInit(IN_Z char* Name, IN THREAD_PRIORITY Priority, OUT_PTR PTHREAD* Thread, IN BOOLEAN AllocateKernelStack);
static STATUS _ThreadSetupInitialState(IN PTHREAD Thread, IN PVOID StartFunction, IN QWORD Arg1, IN QWORD Arg2, IN BOOLEAN KernelStack);
static STATUS _ThreadSetupMainThreadUserStack(IN PVOID InitialStack, OUT PVOID* ResultingStack, IN PPROCESS Process);
static void _ThreadSchedule(void);
void MS_ABI ThreadCleanupPostSchedule(void);
PTHREAD _ThreadGetReadyThread(void);
static void _ThreadReference(INOUT PTHREAD Thread);
static void _ThreadDereference(INOUT PTHREAD Thread);
static FUNC_FreeFunction _ThreadDestroy;
static MS_ABI void _ThreadKernelFunction(IN PFUNC_ThreadStart Function, IN_OPT PVOID Context);

// --- Implementare Fibonacci ---
typedef struct _FIB_DATA { int n; int result; } FIB_DATA, *PFIB_DATA;
STATUS MS_ABI FibonacciThreadFunc(IN_OPT PVOID Context) {
    PFIB_DATA data = (PFIB_DATA)Context;
    if (!data) return STATUS_INVALID_PARAMETER1;
    if (data->n <= 1) { data->result = 1; return STATUS_SUCCESS; }
    FIB_DATA d1 = { data->n - 1, 0 }, d2 = { data->n - 2, 0 };
    PTHREAD t1 = NULL, t2 = NULL; STATUS st;
    ThreadCreate("Fib1", ThreadPriorityDefault, FibonacciThreadFunc, &d1, &t1);
    ThreadCreate("Fib2", ThreadPriorityDefault, FibonacciThreadFunc, &d2, &t2);
    if (t1) { ThreadWaitForTermination(t1, &st); ThreadCloseHandle(t1); }
    if (t2) { ThreadWaitForTermination(t2, &st); ThreadCloseHandle(t2); }
    data->result = d1.result + d2.result;
    return STATUS_SUCCESS;
}

void _No_competing_thread_ ThreadSystemPreinit(void) {
    memzero(&m_threadSystemData, sizeof(THREAD_SYSTEM_DATA));
    InitializeListHead(&m_threadSystemData.AllThreadsList);
    LockInit(&m_threadSystemData.AllThreadsLock);
    InitializeListHead(&m_threadSystemData.ReadyThreadsList);
    LockInit(&m_threadSystemData.ReadyThreadsLock);
}

void ThreadSystemGetThreadList(OUT PLOCK* ListLock, OUT PLIST_ENTRY* ListHead) {
    *ListLock = &m_threadSystemData.AllThreadsLock;
    *ListHead = &m_threadSystemData.AllThreadsList; 
}

STATUS ThreadSystemInitMainForCurrentCPU(void) {
    STATUS status; char name[MAX_PATH]; PTHREAD pT = NULL;
    snprintf(name, MAX_PATH, "main-%02x", GetCurrentPcpu()->ApicId);
    status = _ThreadInit(name, ThreadPriorityDefault, &pT, FALSE);
    if (!SUCCEEDED(status)) return status;
    pT->InitialStackBase = GetCurrentPcpu()->StackTop;
    pT->StackSize = GetCurrentPcpu()->StackSize;
    pT->State = ThreadStateRunning;
    SetCurrentThread(pT);
    if (ProcessRetrieveSystemProcess()) ProcessInsertThreadInList(ProcessRetrieveSystemProcess(), pT);
    return status;
}

static STATUS (CDECL MS_ABI _IdleThread)(IN_OPT PVOID Context);
STATUS ThreadSystemInitIdleForCurrentCPU(void) {
    EX_EVENT idleStarted; STATUS status; char idleName[MAX_PATH]; PTHREAD idleT;
    status = ExEventInit(&idleStarted, ExEventTypeSynchronization, FALSE);
    if (!SUCCEEDED(status)) return status;
    snprintf(idleName, MAX_PATH, "idle-%02x", GetCurrentPcpu()->ApicId);
    status = ThreadCreate(idleName, ThreadPriorityDefault, _IdleThread, &idleStarted, &idleT);
    if (!SUCCEEDED(status)) return status;
    ThreadCloseHandle(idleT);
    CpuIntrEnable();
    ExEventWaitForSignal(&idleStarted);
    ExEventDestroy(&idleStarted);
    return status;
}

STATUS ThreadCreateEx(IN_Z char* Name, IN THREAD_PRIORITY Priority, IN PFUNC_ThreadStart Function, IN_OPT PVOID Context, OUT_PTR PTHREAD* Thread, INOUT struct _PROCESS* Process) {
    STATUS status; PTHREAD pT = NULL; PVOID pStart = NULL; QWORD arg1 = 0, arg2 = 0;
    if (!Name || !Function || !Thread || !Process) return STATUS_INVALID_PARAMETER1;
    status = _ThreadInit(Name, Priority, &pT, TRUE);
    if (!SUCCEEDED(status)) return status;

    ProcessInsertThreadInList(Process, pT);
    pT->ParentThread = GetCurrentThread();
    if (pT->ParentThread) {
        _ThreadReference(pT->ParentThread);
        pT->ParentThread->ChildThreadsCreated++;
        AtomicIncrement32((volatile int32_t*)&pT->ParentThread->NumberOfActiveChildren);
    }
    _ThreadReference(pT);

    PVOID entry; ExecutableLoaderGetEntryPoint(Process->LoaderContext, &entry);
    if (!Process->PagingData->Data.KernelSpace) {
        pT->UserStack = MmuAllocStack(STACK_DEFAULT_SIZE, TRUE, FALSE, Process);
        if (!pT->UserStack) return STATUS_MEMORY_CANNOT_BE_COMMITED;
        if (Function == entry) {
            _ThreadSetupMainThreadUserStack(pT->UserStack, &pT->UserStack, Process);
            pStart = entry; arg1 = Process->NumberOfArguments; arg2 = (QWORD)PtrOffset(pT->UserStack, SHADOW_STACK_SIZE + sizeof(PVOID));
        } else {
            pT->UserStack = (PVOID)PtrDiff(pT->UserStack, SHADOW_STACK_SIZE + sizeof(PVOID));
            pStart = (PVOID)Function; arg1 = (QWORD)Context; arg2 = 0;
        }
    } else { pStart = _ThreadKernelFunction; arg1 = (QWORD)Function; arg2 = (QWORD)Context; }
    _ThreadSetupInitialState(pT, pStart, arg1, arg2, Process->PagingData->Data.KernelSpace);
    if (!GetCurrentPcpu()->ThreadData.IdleThread) { pT->State = ThreadStateReady; GetCurrentPcpu()->ThreadData.IdleThread = pT; }
    else { ThreadUnblock(pT); }
    *Thread = pT; return status;
}

STATUS ThreadCreate(IN_Z char* Name, IN THREAD_PRIORITY Priority, IN PFUNC_ThreadStart Function, IN_OPT PVOID Context, OUT_PTR PTHREAD* Thread) {
    return ThreadCreateEx(Name, Priority, Function, Context, Thread, ProcessRetrieveSystemProcess());
}

void ThreadTick(void) {
    PPCPU pCpu = GetCurrentPcpu(); PTHREAD pT = GetCurrentThread();
    if (pCpu->ThreadData.IdleThread == pT) { pCpu->ThreadData.IdleTicks++; }
    else {
        pCpu->ThreadData.KernelTicks++;
        pT->TotalTicksRun++;
        if (pT->TotalTicksRun >= 16) pT->CurrentQuantum = 2;
    }
    pT->TickCountCompleted++;
    if (++pCpu->ThreadData.RunningThreadTicks >= pT->CurrentQuantum) pCpu->ThreadData.YieldOnInterruptReturn = TRUE;
}

void ThreadExit(IN STATUS ExitStatus) {
    PTHREAD pT = GetCurrentThread(); INTR_STATE old;
    if (pT->ParentThread) AtomicDecrement32((volatile int32_t*)&pT->ParentThread->NumberOfActiveChildren);
    CpuIntrDisable();
    if (LockIsOwner(&pT->BlockLock)) LockRelease(&pT->BlockLock, INTR_OFF);
    pT->State = ThreadStateDying; pT->ExitStatus = ExitStatus;
    ExEventSignal(&pT->TerminationEvt); ProcessNotifyThreadTermination(pT);
    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &old); _ThreadSchedule(); NOT_REACHED;
}

static void _ThreadSchedule(void) {
    PTHREAD pCur = GetCurrentThread(); PTHREAD pNext = _ThreadGetReadyThread();
    PPCPU pCpu = GetCurrentPcpu(); pCpu->ThreadData.PreviousThread = pCur;
    if (pNext != pCur) {
        if (pCur->Process != pNext->Process) MmuChangeProcessSpace(pNext->Process);
        pCur->UninterruptedTicks = 0; pNext->QuantaAllocatedCount++;
        pNext->State = ThreadStateRunning; SetCurrentThread(pNext);
        ThreadSwitch(&pCur->Stack, pNext->Stack);
    } else { pCur->UninterruptedTicks++; }
    ThreadCleanupPostSchedule();
}

void MS_ABI ThreadCleanupPostSchedule(void) {
    PTHREAD prev = GetCurrentPcpu()->ThreadData.PreviousThread;
    LockRelease(&m_threadSystemData.ReadyThreadsLock, INTR_OFF);
    if (prev) { 
        if (LockIsOwner(&prev->BlockLock)) LockRelease(&prev->BlockLock, INTR_OFF); 
        else if (prev->State == ThreadStateDying) _ThreadDereference(prev); 
    }
}

PTHREAD _ThreadGetReadyThread(void) {
    PLIST_ENTRY pEntry = RemoveHeadList(&m_threadSystemData.ReadyThreadsList);
    if (pEntry == &m_threadSystemData.ReadyThreadsList) return GetCurrentPcpu()->ThreadData.IdleThread;
    return CONTAINING_RECORD(pEntry, THREAD, ReadyList);
}

static STATUS _ThreadInit(IN_Z char* Name, IN THREAD_PRIORITY Priority, OUT_PTR PTHREAD* Thread, IN BOOLEAN AllocateKernelStack) {
    STATUS status = STATUS_SUCCESS; INTR_STATE old;
    PTHREAD pT = ExAllocatePoolWithTag(PoolAllocateZeroMemory, sizeof(THREAD), HEAP_THREAD_TAG, 0);
    if (!pT) return STATUS_HEAP_INSUFFICIENT_RESOURCES;
    RfcInit(&pT->RefCnt, _ThreadDestroy, NULL); pT->Self = pT;
    ExEventInit(&pT->TerminationEvt, ExEventTypeNotification, FALSE);
    if (AllocateKernelStack) {
        pT->Stack = MmuAllocStack(STACK_DEFAULT_SIZE, TRUE, FALSE, NULL);
        pT->InitialStackBase = pT->Stack; pT->StackSize = STACK_DEFAULT_SIZE;
    }
    pT->Name = ExAllocatePoolWithTag(PoolAllocateZeroMemory, strlen(Name) + 1, HEAP_THREAD_TAG, 0);
    strcpy(pT->Name, Name);
    
    // INIȚIALIZARE LISTE (Esențial pentru a evita ASSERT list.c:63)
    InitializeListHead(&pT->AllList);
    InitializeListHead(&pT->ReadyList);
    InitializeListHead(&pT->ProcessList);

    pT->Id = _ThreadSystemGetNextTid();
    pT->State = ThreadStateBlocked; pT->Priority = Priority;
    pT->CreationCpuId = (BYTE)GetCurrentPcpu()->ApicId; pT->ParentThread = NULL;
    pT->ChildThreadsCreated = 0; pT->NumberOfActiveChildren = 0;
    pT->TotalTicksRun = 0; pT->CurrentQuantum = 4; pT->QuantaAllocatedCount = 0;
    LockInit(&pT->BlockLock);
    LockAcquire(&m_threadSystemData.AllThreadsLock, &old);
    InsertTailList(&m_threadSystemData.AllThreadsList, &pT->AllList);
    LockRelease(&m_threadSystemData.AllThreadsLock, old);
    *Thread = pT; return status;
}

void ThreadYield(void) {
    INTR_STATE dummy, old = CpuIntrDisable(); PTHREAD pT = GetCurrentThread();
    BOOLEAN forced = GetCurrentPcpu()->ThreadData.YieldOnInterruptReturn;
    GetCurrentPcpu()->ThreadData.YieldOnInterruptReturn = FALSE;
    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &dummy);
    if (pT != GetCurrentPcpu()->ThreadData.IdleThread) InsertTailList(&m_threadSystemData.ReadyThreadsList, &pT->ReadyList);
    if (!forced) pT->TickCountEarly++;
    pT->State = ThreadStateReady; _ThreadSchedule(); CpuIntrSetState(old);
}
void ThreadBlock(void) {
    INTR_STATE old; PTHREAD pC = GetCurrentThread();
    pC->TickCountEarly++; pC->State = ThreadStateBlocked;
    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &old); _ThreadSchedule();
}
void ThreadUnblock(IN PTHREAD Thread) {
    INTR_STATE old, dummy; LockAcquire(&Thread->BlockLock, &old);
    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &dummy);
    InsertTailList(&m_threadSystemData.ReadyThreadsList, &Thread->ReadyList);
    Thread->State = ThreadStateReady;
    LockRelease(&m_threadSystemData.ReadyThreadsLock, dummy); LockRelease(&Thread->BlockLock, old);
}
void ThreadWaitForTermination(IN PTHREAD Thread, OUT STATUS* ExitStatus) {
    ExEventWaitForSignal(&Thread->TerminationEvt); if (ExitStatus) *ExitStatus = Thread->ExitStatus;
}
void ThreadCloseHandle(INOUT PTHREAD Thread) { _ThreadDereference(Thread); }
TID ThreadGetId(IN_OPT PTHREAD Thread) { PTHREAD p = (Thread) ? Thread : GetCurrentThread(); return (p) ? p->Id : 0; }
void SetCurrentThread(IN PTHREAD Thread) {
    AsmWriteMsr(IA32_FS_BASE_MSR, (QWORD) Thread);
    GetCurrentPcpu()->ThreadData.CurrentThread = Thread->Self;
    if (Thread->Self) { GetCurrentPcpu()->StackTop = Thread->InitialStackBase; GetCurrentPcpu()->Tss.Rsp[0] = (QWORD) Thread->InitialStackBase; }
}
static MS_ABI void _ThreadKernelFunction(PFUNC_ThreadStart Function, PVOID Context) { STATUS st = Function(Context); ThreadExit(st); }
static STATUS (CDECL MS_ABI _IdleThread)(PVOID Context) {
    ExEventSignal((PEX_EVENT)Context);
    while (TRUE) { CpuIntrDisable(); ThreadTakeBlockLock(); ThreadBlock(); __sti_and_hlt(); }
}
void ThreadTakeBlockLock(void) { INTR_STATE dummy; LockAcquire(&GetCurrentThread()->BlockLock, &dummy); }

static void _ThreadDestroy(PVOID Object, PVOID Context) {
    PTHREAD p = (PTHREAD)CONTAINING_RECORD(Object, THREAD, RefCnt); INTR_STATE old; UNREFERENCED_PARAMETER(Context);
    LockAcquire(&m_threadSystemData.AllThreadsLock, &old);
    if (!IsListEmpty(&p->AllList)) RemoveEntryList(&p->AllList);
    LockRelease(&m_threadSystemData.AllThreadsLock, old);
    if (p->ParentThread) _ThreadDereference(p->ParentThread);
    if (p->UserStack) MmuFreeStack(p->UserStack, p->Process);
    ProcessRemoveThreadFromList(p); ExEventDestroy(&p->TerminationEvt);
    if (p->Name) ExFreePoolWithTag(p->Name, HEAP_THREAD_TAG);
    if (p->Stack) MmuFreeStack(p->Stack, NULL); ExFreePoolWithTag(p, HEAP_THREAD_TAG);
}
static void _ThreadReference(INOUT PTHREAD T) { RfcReference(&T->RefCnt); }
static void _ThreadDereference(INOUT PTHREAD T) { RfcDereference(&T->RefCnt); }
void ThreadTerminate(INOUT PTHREAD Thread) { AtomicOr32(&Thread->Flags, THREAD_FLAG_FORCE_TERMINATE_PENDING); }
const char* ThreadGetName(IN_OPT PTHREAD Thread) { PTHREAD p = (Thread) ? Thread : GetCurrentThread(); return (p) ? p->Name : ""; }
THREAD_PRIORITY ThreadGetPriority(IN_OPT PTHREAD Thread) { PTHREAD p = (Thread) ? Thread : GetCurrentThread(); return (p) ? p->Priority : 0; }
void ThreadSetPriority(IN THREAD_PRIORITY NewPriority) { GetCurrentThread()->Priority = NewPriority; }
STATUS ThreadExecuteForEachThreadEntry(IN PFUNC_ListFunction Function, IN_OPT PVOID Context) {
    STATUS status; INTR_STATE old; if (NULL == Function) return STATUS_INVALID_PARAMETER1;
    LockAcquire(&m_threadSystemData.AllThreadsLock, &old);
    status = ForEachElementExecute(&m_threadSystemData.AllThreadsList, Function, Context, FALSE);
    LockRelease(&m_threadSystemData.AllThreadsLock, old); return status;
}
BOOLEAN ThreadYieldOnInterrupt(void) { return GetCurrentPcpu()->ThreadData.YieldOnInterruptReturn; }
static STATUS _ThreadSetupInitialState(IN PTHREAD Thread, IN PVOID StartFunc, IN QWORD Arg1, IN QWORD Arg2, IN BOOLEAN KernelStack) {
    PVOID* pStack = (PVOID*)Thread->Stack; pStack = pStack - (4 + 1); pStack = (PVOID*)PtrDiff(pStack, sizeof(INTERRUPT_STACK));
    PINTERRUPT_STACK pIst = (PINTERRUPT_STACK)pStack; pIst->Rip = (QWORD)StartFunc;
    if (KernelStack) { pIst->CS = GdtMuGetCS64Supervisor(); pIst->Rsp = (QWORD)(pIst + 1); pIst->SS = GdtMuGetDS64Supervisor(); }
    else { pIst->CS = GdtMuGetCS64Usermode() | RING_THREE_PL; pIst->Rsp = (QWORD)Thread->UserStack; pIst->SS = GdtMuGetDS64Usermode() | RING_THREE_PL; }
    pIst->RFLAGS = RFLAGS_INTERRUPT_FLAG_BIT | RFLAGS_RESERVED_BIT; pStack = pStack - 1; *pStack = (PVOID)ThreadStart;
    pStack = (PVOID*)PtrDiff(pStack, sizeof(COMPLETE_PROCESSOR_STATE)); PCOMPLETE_PROCESSOR_STATE pState = (PCOMPLETE_PROCESSOR_STATE)pStack;
    memzero(pState, sizeof(COMPLETE_PROCESSOR_STATE)); pState->RegisterArea.RegisterValues[RegisterRcx] = Arg1;
    pState->RegisterArea.RegisterValues[RegisterRdx] = Arg2; Thread->Stack = pStack; return STATUS_SUCCESS;
}
static STATUS _ThreadSetupMainThreadUserStack(IN PVOID InitialStack, OUT PVOID* ResultingStack, IN PPROCESS Process) {
    UNREFERENCED_PARAMETER(Process); *ResultingStack = (PVOID)PtrDiff(InitialStack, SHADOW_STACK_SIZE + sizeof(PVOID)); return STATUS_SUCCESS;
}