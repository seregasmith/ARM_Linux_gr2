/* The guest-context fibers of hosts whose guest CPU is not the host's (src/runtime_gr2_fiber.c),
 * run on an x86-64 host: a stand-in for guest_cpu.h's crossing (the FEXCore one in cpu/) takes
 * the guest registers at each call of a host function into a state block, calls the host function
 * on a host stack of its own, stores RAX and loads every register back from the block before the
 * stub's `ret`, so guest_cpu_swap_context works as with FEXCore. The "guest" fibers are C code. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "runtime.h"
#include "guest_cpu.h"
#include "runtime_gr2_fiber.h"

#if !defined(__x86_64__) || !defined(GR2_FIBER_GUEST_CONTEXT)
#error "x86-64 only, built with -DGR2_FIBER_GUEST_CONTEXT=1"
#endif

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

/* ---- The crossing (mock_dispatch): what HandleSyscall/CallHost and the JIT dispatcher do ---- */
uint64_t mock_state[16];           /* guest registers, x86 encoding order (GuestRegs.gpr) */
unsigned char mock_in_call;
unsigned char mock_host_stack[1 << 16] __attribute__((aligned(16)));
__asm__(".intel_syntax noprefix\n"
        ".text\n.globl mock_dispatch\nmock_dispatch:\n" /* r11 = host function, [rsp] = return address */
        "  mov [rip+mock_state+0], rax\n  mov [rip+mock_state+8], rcx\n"
        "  mov [rip+mock_state+16], rdx\n  mov [rip+mock_state+24], rbx\n"
        "  mov [rip+mock_state+32], rsp\n  mov [rip+mock_state+40], rbp\n"
        "  mov [rip+mock_state+48], rsi\n  mov [rip+mock_state+56], rdi\n"
        "  mov [rip+mock_state+64], r8\n  mov [rip+mock_state+72], r9\n"
        "  mov [rip+mock_state+80], r10\n  mov [rip+mock_state+88], r11\n"
        "  mov [rip+mock_state+96], r12\n  mov [rip+mock_state+104], r13\n"
        "  mov [rip+mock_state+112], r14\n  mov [rip+mock_state+120], r15\n"
        /* The host function on its own stack, with the guest's first two stack arguments. */
        "  mov rax, rsp\n  lea rsp, [rip+mock_host_stack+65536]\n"
        "  push qword ptr [rax+16]\n  push qword ptr [rax+8]\n"
        "  mov byte ptr [rip+mock_in_call], 1\n  call r11\n  mov byte ptr [rip+mock_in_call], 0\n"
        "  mov [rip+mock_state+0], rax\n"
        "  mov rcx, [rip+mock_state+8]\n  mov rdx, [rip+mock_state+16]\n"
        "  mov rbx, [rip+mock_state+24]\n  mov rbp, [rip+mock_state+40]\n"
        "  mov rsi, [rip+mock_state+48]\n  mov rdi, [rip+mock_state+56]\n"
        "  mov r8, [rip+mock_state+64]\n  mov r9, [rip+mock_state+72]\n"
        "  mov r10, [rip+mock_state+80]\n  mov r11, [rip+mock_state+88]\n"
        "  mov r12, [rip+mock_state+96]\n  mov r13, [rip+mock_state+104]\n"
        "  mov r14, [rip+mock_state+112]\n  mov r15, [rip+mock_state+120]\n"
        "  mov rsp, [rip+mock_state+32]\n  mov rax, [rip+mock_state+0]\n"
        "  ret\n"
        /* call_canaries(fn, a, b, c, ok): fn(a, b, c) with known values in the callee-saved
         * registers; *ok = whether they came back unchanged. */
        ".globl call_canaries\ncall_canaries:\n"
        "  push rbx\n  push rbp\n  push r12\n  push r13\n  push r14\n  push r15\n  push r8\n"
        "  mov rax, rdi\n  mov rdi, rsi\n  mov rsi, rdx\n  mov rdx, rcx\n"
        "  movabs rbx, 0x1111111111111111\n  movabs rbp, 0x2222222222222222\n"
        "  movabs r12, 0x3333333333333333\n  movabs r13, 0x4444444444444444\n"
        "  movabs r14, 0x5555555555555555\n  movabs r15, 0x6666666666666666\n"
        "  call rax\n"
        "  xor ecx, ecx\n"
        "  movabs r11, 0x1111111111111111\n  cmp rbx, r11\n  setne dl\n  or cl, dl\n"
        "  movabs r11, 0x2222222222222222\n  cmp rbp, r11\n  setne dl\n  or cl, dl\n"
        "  movabs r11, 0x3333333333333333\n  cmp r12, r11\n  setne dl\n  or cl, dl\n"
        "  movabs r11, 0x4444444444444444\n  cmp r13, r11\n  setne dl\n  or cl, dl\n"
        "  movabs r11, 0x5555555555555555\n  cmp r14, r11\n  setne dl\n  or cl, dl\n"
        "  movabs r11, 0x6666666666666666\n  cmp r15, r11\n  setne dl\n  or cl, dl\n"
        "  xor cl, 1\n  pop r8\n  movzx ecx, cl\n  mov [r8], ecx\n"
        "  pop r15\n  pop r14\n  pop r13\n  pop r12\n  pop rbp\n  pop rbx\n  ret\n"
        ".att_syntax prefix\n");
void mock_dispatch(void);
uint64_t call_canaries(uintptr_t fn, uint64_t a, uint64_t b, uint64_t c, int *ok);

uintptr_t guest_cpu_host_function(void *fn) {
    static unsigned char *stubs;
    static size_t used;
    if (!stubs) stubs = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *p = stubs + used;
    used += 32;
    const uintptr_t dispatch = (uintptr_t)mock_dispatch, target = (uintptr_t)fn;
    p[0] = 0x49; p[1] = 0xbb; memcpy(p + 2, &target, 8);    /* movabs r11, fn */
    p[10] = 0x49; p[11] = 0xba; memcpy(p + 12, &dispatch, 8); /* movabs r10, mock_dispatch */
    p[20] = 0x41; p[21] = 0xff; p[22] = 0xe2;                   /* jmp r10 */
    return (uintptr_t)p;
}
int guest_cpu_swap_context(GuestRegs *save, const GuestRegs *load) {
    if (!mock_in_call) return 0;
    if (save) {
        memcpy(save->gpr, mock_state, sizeof(save->gpr));
        save->rip = *(const uint64_t *)(uintptr_t)mock_state[GUEST_RSP];
    }
    memcpy(mock_state, load->gpr, sizeof(load->gpr));
    return 1;
}
void *runtime_low_map(size_t size, int prot) {
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* ---- The exports as the game reaches them: through host function stubs ---- */
typedef int32_t (*InitFn)(void *, const char *, uintptr_t, uint64_t, void *, uint64_t, const void *, uint32_t);
typedef int32_t (*FinFn)(void *);
typedef int32_t (*RunFn)(void *, uint64_t, uint64_t *);
typedef int32_t (*RetFn)(uint64_t, uint64_t *);
static InitFn fiber_init;
static FinFn fiber_fin;
static RunFn fiber_run, fiber_switch;
static RetFn fiber_return;

static uint64_t fiber_a[2], fiber_b[2]; /* the game's SceFiber objects */
static unsigned char *stack_a;
#define STACK_A (256 * 1024)
static char trace[512];
static size_t trace_len;
static void note(const char *format, unsigned long long a, unsigned long long b) {
    trace_len += (size_t)snprintf(trace + trace_len, sizeof(trace) - trace_len, format, a, b);
}
static int on_stack_a(const void *p) { return (const unsigned char *)p >= stack_a && (const unsigned char *)p < stack_a + STACK_A; }

static void entry_a(uint64_t init, uint64_t run) {
    volatile int local = 0;
    note("A%llu:%llu ", init, run);
    CHECK(on_stack_a((const void *)&local));
    uint64_t got = 0;
    CHECK(fiber_switch(fiber_a, 1, &got) == FIBER_ERR_STATE); /* itself: running */
    CHECK(fiber_run(fiber_b, 1, &got) == FIBER_ERR_PERMISSION); /* a fiber does not Run */
    CHECK(fiber_switch(fiber_b, 20, &got) == 0);
    note("A<%llu ", got, 0);
    CHECK(on_stack_a((const void *)&local));
    CHECK(fiber_return(40, &got) == 0);
    note("A<<%llu ", got, 0);
    CHECK(fiber_return(60, &got) == 0);
    note("A<<<%llu ", got, 0);
    /* Callee-saved registers survive a round trip through the thread and back. */
    int ok = 0;
    CHECK(call_canaries((uintptr_t)fiber_return, 80, (uint64_t)(uintptr_t)&got, 0, &ok) == 0);
    CHECK(ok);
    note("A<<<<%llu ", got, 0);
    fiber_return(100, &got);
    puts("FAIL: fiber A resumed after its last return");
    exit(1);
}
static void entry_b(uint64_t init, uint64_t run) {
    volatile int local = 0;
    note("B%llu:%llu ", init, run);
    CHECK(!on_stack_a((const void *)&local));
    uint64_t got = 0;
    CHECK(fiber_switch(fiber_a, 30, &got) == 0);
    puts("FAIL: fiber B resumed");
    exit(1);
}

int main(void) {
    fiber_init = (InitFn)guest_cpu_host_function((void *)gr2_fiber_initialize);
    fiber_fin = (FinFn)guest_cpu_host_function((void *)gr2_fiber_finalize);
    fiber_run = (RunFn)guest_cpu_host_function((void *)gr2_fiber_run);
    fiber_switch = (RunFn)guest_cpu_host_function((void *)gr2_fiber_switch);
    fiber_return = (RetFn)guest_cpu_host_function((void *)gr2_fiber_return_to_thread);
    stack_a = runtime_low_map(STACK_A, PROT_READ | PROT_WRITE);

    CHECK(fiber_init(fiber_a, "A", (uintptr_t)entry_a, 1, stack_a, STACK_A, NULL, 0) == 0);
    CHECK(fiber_init(fiber_b, "B", (uintptr_t)entry_b, 2, NULL, 0, NULL, 0) == 0); /* own stack */
    uint64_t got = 0, bad[2] = {0, 0};
    CHECK(fiber_run(bad, 0, &got) == FIBER_ERR_INVALID);
    CHECK(fiber_switch(fiber_a, 0, &got) == FIBER_ERR_PERMISSION); /* the thread does not Switch */
    CHECK(fiber_return(0, &got) == FIBER_ERR_PERMISSION);
    /* A starts, switches to B (which starts), B switches back to A, A returns 40 to the thread. */
    CHECK(fiber_run(fiber_a, 10, &got) == 0);
    CHECK(got == 40);
    CHECK(fiber_run(fiber_a, 50, &got) == 0); /* A's ReturnToThread gets 50 */
    CHECK(got == 60);
    int ok = 0;
    CHECK(call_canaries((uintptr_t)fiber_run, (uint64_t)(uintptr_t)fiber_a, 70, (uint64_t)(uintptr_t)&got, &ok) == 0);
    CHECK(ok && got == 80);
    CHECK(fiber_run(fiber_a, 90, &got) == 0); /* A checks its own registers, returns 100 */
    CHECK(got == 100);
    CHECK(fiber_fin(fiber_b) == 0); /* suspended in its Switch: not running */
    CHECK(fiber_fin(fiber_b) == FIBER_ERR_INVALID);
    CHECK(fiber_fin(fiber_a) == 0);
    printf("fibers: %s\n", trace);
    CHECK(!strcmp(trace, "A1:10 B2:20 A<30 A<<50 A<<<70 A<<<<90 "));
    printf("guest-context fiber test: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
