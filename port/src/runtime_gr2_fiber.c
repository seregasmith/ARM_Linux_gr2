/* sceFiber on hosts whose guest CPU is not the host's (aarch64: FEXCore). The game's code runs in
 * the guest CPU with one host stack per thread, so a fiber is only a guest execution context: a
 * stack (the game's context memory) and the registers a SysV call preserves. Run, Switch and
 * ReturnToThread are guest calls of host functions; each one saves the calling context's registers
 * and makes the call return into another context instead (guest_cpu_swap_context), where the
 * pending Run, Switch or ReturnToThread call returns 0, or, the first time, the fiber's entry
 * function starts on its own stack. No host stack or JIT state changes hands. The bookkeeping
 * (arguments delivered through argOnRun/argOnReturn, the running checks) is runtime_gr2.c's
 * x86-64 implementation's. */
#define _GNU_SOURCE
#include "runtime_gr2_fiber.h"
#if defined(GR2_FIBER_GUEST_CONTEXT)
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct HostFiber {
    GuestRegs regs;          /* where a suspended fiber continues (saved at its Switch/ReturnToThread) */
    uintptr_t entry;
    uint64_t arg_on_initialize;
    uint64_t *resume_out;    /* where the fiber's own Switch/ReturnToThread wants its next argOnRun */
    uint64_t *stack_top;     /* 16-byte aligned */
    void *own_stack;         /* when the game gave no context memory */
    size_t own_size;
    int started, running;
    char name[32];
} HostFiber;
/* The guest SceFiber (opaque, at least 16 bytes here) holds the magic and the host object. */
typedef struct { uint64_t magic; HostFiber *host; } GuestFiber;
static __thread struct {
    GuestRegs thread_regs;   /* the thread's own context while one of its fibers runs */
    HostFiber *current;
    uint64_t *thread_return_out;
} fiber_tls;

static HostFiber *fiber_of(void *guest) {
    GuestFiber *g=guest;
    return g && g->magic==FIBER_MAGIC ? g->host : NULL;
}
/* The return address of a fiber's entry function: entry functions never return. */
static ABI void fiber_entry_returned(void) {
    HostFiber *f=fiber_tls.current;
    fprintf(stderr,"STOP: fiber %s returned from its entry function\n",f ? f->name : "?");
    fflush(NULL);
    abort();
}
static void swap(GuestRegs *save,const GuestRegs *load,const char *what) {
    if (!guest_cpu_swap_context(save,load)) {
        fprintf(stderr,"STOP: %s: not called by guest code\n",what);
        fflush(NULL);
        exit(21);
    }
}
/* The context `f` continues in: its entry function the first time (on the fiber's stack, entered
 * by the host call's return: [rsp] = entry, then entry's own return address), else where it
 * stopped, its pending Switch/ReturnToThread delivering `arg` through argOnRun. */
static void fiber_enter(HostFiber *f,uint64_t arg,GuestRegs *load) {
    if (!f->started) {
        memset(load,0,sizeof(*load));
        uint64_t *top=f->stack_top;
        top[-1]=guest_cpu_host_function((void *)fiber_entry_returned);
        top[-2]=f->entry;
        load->gpr[GUEST_RSP]=(uint64_t)(uintptr_t)(top-2);
        load->gpr[GUEST_RDI]=f->arg_on_initialize;
        load->gpr[GUEST_RSI]=arg;
        load->rip=f->entry;
        f->started=1;
    } else {
        *load=f->regs;
        if (f->resume_out) *f->resume_out=arg;
    }
    f->running=1;
    fiber_tls.current=f;
}

int32_t gr2_fiber_initialize(void *guest,const char *name,uintptr_t entry,uint64_t arg_on_initialize,
                             void *context,uint64_t context_size,const void *option,uint32_t build_version) {
    (void)option; (void)build_version;
    if (!guest || !entry) return FIBER_ERR_NULL;
    HostFiber *f=calloc(1,sizeof(*f));
    if (!f) return FIBER_ERR_INVALID;
    f->entry=entry; f->arg_on_initialize=arg_on_initialize;
    snprintf(f->name,sizeof(f->name),"%s",name ? name : "");
    unsigned char *stack=context;
    size_t size=(size_t)context_size;
    if (!stack || size<16384) {
        /* Below 1 TiB like the game's own stacks: the game packs pointers into 40 bits. */
        size=size<65536 ? 65536 : size;
        stack=runtime_low_map(size,PROT_READ|PROT_WRITE);
        if (!stack) { free(f); return FIBER_ERR_INVALID; }
        f->own_stack=stack; f->own_size=size;
    }
    f->stack_top=(uint64_t *)(((uintptr_t)stack+size)&~(uintptr_t)15);
    ((GuestFiber *)guest)->magic=FIBER_MAGIC;
    ((GuestFiber *)guest)->host=f;
    return 0;
}
int32_t gr2_fiber_finalize(void *guest) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    if (f->running) return FIBER_ERR_STATE;
    ((GuestFiber *)guest)->magic=0;
    if (f->own_stack) munmap(f->own_stack,f->own_size); /* the context memory belongs to the game */
    free(f);
    return 0;
}
int32_t gr2_fiber_run(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_return) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    if (fiber_tls.current) return FIBER_ERR_PERMISSION; /* only a thread runs a fiber */
    if (f->running) return FIBER_ERR_STATE;
    GuestRegs load;
    fiber_tls.thread_return_out=arg_on_return;
    fiber_enter(f,arg_on_run_to,&load);
    swap(&fiber_tls.thread_regs,&load,"sceFiberRun");
    return 0; /* seen by the fiber: its pending Switch/ReturnToThread returns 0 */
}
int32_t gr2_fiber_switch(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_run) {
    HostFiber *to=fiber_of(guest), *from=fiber_tls.current;
    if (!to) return FIBER_ERR_INVALID;
    if (!from) return FIBER_ERR_PERMISSION;
    if (to->running) return FIBER_ERR_STATE;
    from->resume_out=arg_on_run; from->running=0;
    GuestRegs load;
    fiber_enter(to,arg_on_run_to,&load);
    swap(&from->regs,&load,"sceFiberSwitch");
    return 0;
}
int32_t gr2_fiber_return_to_thread(uint64_t arg_on_return,uint64_t *arg_on_run) {
    HostFiber *from=fiber_tls.current;
    if (!from) return FIBER_ERR_PERMISSION;
    if (fiber_tls.thread_return_out) *fiber_tls.thread_return_out=arg_on_return;
    from->resume_out=arg_on_run; from->running=0;
    fiber_tls.current=NULL;
    swap(&from->regs,&fiber_tls.thread_regs,"sceFiberReturnToThread");
    return 0; /* seen by the thread: its sceFiberRun returns 0 */
}
#else
typedef int runtime_gr2_fiber_unused; /* x86-64 and Windows: runtime_gr2.c switches host contexts */
#endif
