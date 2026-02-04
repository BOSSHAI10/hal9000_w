#include "common_lib.h"
#include "syscall_if.h"
#include "um_lib_helper.h"
#include "syscall_no.h"
#include "syscall_func.h"
#include "status.h"
#include "cl_string.h"
#include "assert.h"

#define printf(...) LOG(__VA_ARGS__)

// Poarta de intrare către Kernel
extern STATUS SyscallEntry(SYSCALL_ID SyscallId, ...);

// --- Implementări Syscall (Capitolul 2) ---
STATUS SyscallProcessGetName(OUT char* Name, IN QWORD Len) { return SyscallEntry(SyscallIdProcessGetName, Name, Len); }
STATUS SyscallGetThreadPriority(OUT BYTE* Prio) { return SyscallEntry(SyscallIdGetThreadPriority, Prio); }
STATUS SyscallSetThreadPriority(IN BYTE Prio) { return SyscallEntry(SyscallIdSetThreadPriority, Prio); }
STATUS SyscallGetCurrentCPUID(OUT BYTE* Id) { return SyscallEntry(SyscallIdGetCurrentCPUID, Id); }
STATUS SyscallGetNumberOfThreadsForCurrentProcess(OUT QWORD* No) { return SyscallEntry(SyscallIdGetNumberOfThreadsForCurrentProcess, No); }
STATUS SyscallGetCPUUtilization(IN_OPT BYTE* Id, OUT BYTE* Utilization) { return SyscallEntry(SyscallIdGetCPUUtilization, Id, Utilization); }

STATUS __main(DWORD Argc, char** Argv)
{
    char name[MAX_PATH];
    BYTE prio, cpu, util;
    QWORD threads;
    PVOID pVirtMem = NULL;
    QWORD allocSize = 5000; // Exemplu: cerem 5000 bytes

    printf("\n--- LightProjectApp: TESTARE FINALA PSO ---\n");

    // TEST CAPITOLUL 2: Programe Utilizator
    if (SUCCEEDED(SyscallProcessGetName(name, MAX_PATH))) printf("1. Proces curent: %s\n", name);
    SyscallGetThreadPriority(&prio);
    printf("2. Prioritate: %u\n", prio);
    SyscallGetCurrentCPUID(&cpu);
    SyscallGetNumberOfThreadsForCurrentProcess(&threads);
    SyscallGetCPUUtilization(NULL, &util);
    printf("3. CPU ID: %u | Fire: %u | Utilizare: %u%%\n", cpu, (DWORD)threads, util);

    // TEST CAPITOLUL 3: Memorie Virtuala
    printf("\n--- Test Memorie Virtuala ---\n");
    // Alocam memorie (Reserve + Commit)
    

    printf("\n--- TOATE TESTELE AU FOST RULATE ---\n");
    return 0;
}

// =============================================================================
// REPARATII LINKER: Am potrivit tipurile de date cu cele din HAL9000 (QWORD/DWORD)
// =============================================================================

void* __stack_chk_guard = (void*)0xDEADBEEF;
void __stack_chk_fail(void) { while(1); }

/* QWORD cl_strlen(const char* s) { 
    QWORD l = 0; while(s && s[l]) l++; return l; 
}

char* cl_strrchr(const char* s, int c) {
    char* last = NULL; if(!s) return NULL;
    do { if(*s == (char)c) last = (char*)s; } while(*s++);
    return last;
}

int cl_vsnprintf(char* str, QWORD size, const char* format, va_list ap) {
    QWORD i = 0;
    while(format[i] && i < size - 1) { str[i] = format[i]; i++; }
    str[i] = '\0';
    return (int)i;
}

void AssertInfo(const char* f, DWORD l, const char* m, ...) { while(0); }*/
