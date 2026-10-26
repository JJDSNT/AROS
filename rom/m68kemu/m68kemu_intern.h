/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)
    m68kemu.library internals — full containment memory model
*/
#ifndef M68KEMU_INTERN_H
#define M68KEMU_INTERN_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/bptr.h>

struct M68KEmuLibBase
{
    struct Library          lib;
    struct ExecBase        *sysBase;
    struct DosLibrary      *dosBase;
};

#define M68KEMU_MEM_SIZE        (32 * 1024 * 1024)
#define M68KEMU_VECTORS_BASE    0x000000
#define M68KEMU_VECTORS_SIZE    0x000400
#define M68KEMU_LIBBASE_REGION  0x000400
#define M68KEMU_LIBBASE_SIZE    0x010000
#define M68KEMU_HEAP_BASE       0x010400
#define M68KEMU_STACK_SIZE      (64 * 1024)

/* Chip RAM window shared with Rigel DMA. Only used when a context enables
   the split Chip/Fast heap (Rigel mode); otherwise there is one heap. */
#define M68KEMU_CHIP_RAM_SIZE   (512UL * 1024UL)

/* Classic I/O space (CIAs at $BFxxxx, custom chips at $DFFxxx). With Rigel
   it is forwarded to the chipset and kept out of the heap. */
#define M68KEMU_IO_START        0x00A00000UL
#define M68KEMU_IO_END          0x00E00000UL

#ifndef M68KEMU_RUN_RIGEL
#define M68KEMU_RUN_RIGEL       (1UL << 0)
#endif
#ifndef M68KEMU_RUN_DISPLAY
#define M68KEMU_RUN_DISPLAY     (1UL << 1)
#endif

/* Exit code of a program stopped by its presenter (RETURN_WARN) */
#define M68KEMU_EXIT_STOPPED    5

struct M68KEmuContext;

/* A completed chipset frame. Pixels are host-endian ULONG 0x00RRGGBB
   (Rigel ISSUE-0010). */
struct M68KEmuFrame
{
    const ULONG *pixels;
    ULONG   width;
    ULONG   height;
    ULONG   pitch;          /* bytes between rows */
    ULONG   number;         /* Rigel frame count, low 32 bits */
    ULONG   frame_us;       /* duration of one frame of chipset time */
};

/* Receives every completed frame. present() returns FALSE to stop the
   program. destroy() runs when M68KEmu_Execute() returns, in the task that
   ran the program, or with the context if it never ran; it must clear the
   presenter. */
struct M68KEmuPresenter
{
    BOOL  (*present)(struct M68KEmuContext *ctx, const struct M68KEmuFrame *frame);
    void  (*destroy)(struct M68KEmuContext *ctx);
    APTR    data;
};

struct M68KHeapBlock
{
    ULONG size;
    ULONG next;
};

struct M68KThunkEntry;

#define M68KEMU_MAX_LIBS        32
#define M68KEMU_MAX_LVO         512

struct M68KFakeLibBase
{
    ULONG   m68k_addr;          /* library base address in m68k space */
    ULONG   jt_start;           /* first byte of jump table (lowest addr) */
    UWORD   lib_id;
    UWORD   num_vectors;
    UWORD   pos_size;
    const struct M68KThunkEntry *thunks;
    ULONG   num_thunks;
    const struct M68KThunkEntry *gen_thunks;
    ULONG   num_gen_thunks;
    char    name[32];           /* library name for OpenLibrary matching */
};

/* Map library names to fake base addresses for OpenLibrary */
#define M68KEMU_MAX_LIBMAP 16
struct M68KLibMapEntry
{
    char    name[32];
    ULONG   m68k_base;          /* fake base in containment space */
};

/* Contained Exec interrupt state (AddIntServer/SetIntVector) */
#define M68KEMU_INT_COUNT       16
#define M68KEMU_INT_MAX_SERVERS 16

/* All-zero is the initial state: idle levels, interrupts enabled */
struct M68KIntLevelState
{
    UBYTE   active;         /* interrupt bit + 1 in progress, 0 = idle */
    UBYTE   next_server;    /* next server of a chain to call */
};

struct M68KIntState
{
    ULONG   vectors[M68KEMU_INT_COUNT];     /* SetIntVector nodes */
    ULONG   servers[M68KEMU_INT_COUNT][M68KEMU_INT_MAX_SERVERS];
    UBYTE   num_servers[M68KEMU_INT_COUNT];
    struct M68KIntLevelState level[8];
    ULONG   dispatch_base;  /* private fake library for the dispatchers */
    ULONG   disable_depth;  /* nested Disable() calls, 0 = enabled */
};

struct M68KEmuContext
{
    UBYTE  *mem;
    ULONG   mem_size;

    ULONG   heap_start;
    ULONG   heap_end;
    ULONG   heap_free;          /* free list: Fast heap, or the only heap */

    /* Split Chip/Fast heap. chip_end == 0 means a single unsplit heap and
       MEMF_CHIP/MEMF_FAST are ignored (legacy behavior). */
    ULONG   chip_end;
    ULONG   chip_free;          /* free list: Chip heap [heap_start, chip_end) */

    ULONG   entry_point;
    ULONG   seg_start;

    ULONG   stack_top;
    ULONG   stack_bottom;

    struct M68KFakeLibBase libs[M68KEMU_MAX_LIBS];
    UWORD   num_libs;

    /* Library name -> fake base mapping (for m68k OpenLibrary) */
    struct M68KLibMapEntry libmap[M68KEMU_MAX_LIBMAP];
    UWORD   num_libmap;

    struct M68KEmuLibBase *emuBase;

    ULONG   m68k_argptr;
    ULONG   m68k_argsize;
    ULONG   m68k_sysbase;

    BOOL    running;
    BOOL    stopped;            /* ended early by the presenter */
    LONG    exit_code;
    /* Shadow struct mapping: m68k addr <-> native ptr */
    #define M68KEMU_MAX_SHADOWS 256
    struct { ULONG m68k_addr; void *native_ptr; UWORD type; } shadow_map[M68KEMU_MAX_SHADOWS];
    UWORD num_shadows;

    /* Program name for GetProgramName */
    char    program_name[256];

    /* Supervisor() redirect: user function address, 0 = none */
    ULONG   sv_redirect;

    /* Custom chip beam position counter (PAL: 313 lines × 227 clocks) */
    UWORD   beam_v;     /* vertical position 0-312 */
    UBYTE   beam_h;     /* horizontal position 0-226 */
    UBYTE   beam_lof;   /* long frame flag (toggled each frame) */

    struct M68KIntState ints;

#ifdef M68KEMU_WITH_RIGEL
    APTR    rigel;
    struct M68KEmuPresenter presenter;

    /* Host input: keys wait here and reach Rigel one per frame */
    #define M68KEMU_KEY_QUEUE 32
    UBYTE   key_queue[M68KEMU_KEY_QUEUE];   /* raw key, bit 7 = up */
    UBYTE   key_head;
    UBYTE   key_count;
    UBYTE   mouse_x;                        /* port 0 quadrature counters */
    UBYTE   mouse_y;
#endif
};

static inline void *m68k_to_host(struct M68KEmuContext *ctx, ULONG addr)
{
    if (addr == 0 || addr >= ctx->mem_size) return NULL;
    return ctx->mem + addr;
}

/* Shadow-aware pointer resolution: shadow lookup first, then containment */
static inline void *m68k_to_host_or_shadow(struct M68KEmuContext *ctx, ULONG addr)
{
    if (addr == 0) return NULL;
    /* Check shadow table first */
    for (UWORD i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == addr)
            return ctx->shadow_map[i].native_ptr;
    /* Fall back to containment memory */
    if (addr >= ctx->mem_size) return NULL;
    return ctx->mem + addr;
}

static inline ULONG host_to_m68k(struct M68KEmuContext *ctx, void *ptr)
{
    if (ptr == NULL) return 0;
    return (ULONG)((UBYTE *)ptr - ctx->mem);
}

/* TODO: bounds checks here guard all containment memory access paths */
static inline UWORD m68k_read16(struct M68KEmuContext *ctx, ULONG addr)
{
    if (addr + 1 >= ctx->mem_size) return 0;
    UBYTE *p = ctx->mem + addr;
    return (UWORD)((p[0] << 8) | p[1]);
}

static inline ULONG m68k_read32(struct M68KEmuContext *ctx, ULONG addr)
{
    if (addr + 3 >= ctx->mem_size) return 0;
    UBYTE *p = ctx->mem + addr;
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8)  |  (ULONG)p[3];
}

static inline void m68k_write16(struct M68KEmuContext *ctx, ULONG addr, UWORD val)
{
    if (addr + 1 >= ctx->mem_size) return;
    UBYTE *p = ctx->mem + addr;
    p[0] = (UBYTE)(val >> 8);
    p[1] = (UBYTE)(val);
}

static inline void m68k_write32(struct M68KEmuContext *ctx, ULONG addr, ULONG val)
{
    if (addr + 3 >= ctx->mem_size) return;
    UBYTE *p = ctx->mem + addr;
    p[0] = (UBYTE)(val >> 24);
    p[1] = (UBYTE)(val >> 16);
    p[2] = (UBYTE)(val >> 8);
    p[3] = (UBYTE)(val);
}

#ifdef __cplusplus
extern "C" {
#endif

struct M68KEmuContext *M68KEmu_CreateContext(struct M68KEmuLibBase *base);
void   M68KEmu_DestroyContext(struct M68KEmuContext *ctx);
LONG   M68KEmu_Execute(struct M68KEmuContext *ctx);
ULONG  M68KEmu_HeapAlloc(struct M68KEmuContext *ctx, ULONG size, ULONG requirements);
void   M68KEmu_HeapFree(struct M68KEmuContext *ctx, ULONG addr, ULONG size);
BOOL   M68KEmu_HeapSplitChip(struct M68KEmuContext *ctx, ULONG chip_end);
ULONG  M68KEmu_HeapAvail(struct M68KEmuContext *ctx, ULONG requirements);
ULONG  M68KEmu_HeapTypeOf(struct M68KEmuContext *ctx, ULONG addr);
ULONG  M68KEmu_LoadHunks(struct M68KEmuContext *ctx, BPTR segList);
ULONG  M68KEmu_LoadHunksFromMemory(struct M68KEmuContext *ctx, UBYTE *data, LONG size);
ULONG  M68KEmu_LoadM68KLibrary(struct M68KEmuContext *ctx, const char *name);
ULONG  M68KEmu_SetupLibBase(struct M68KEmuContext *ctx, UWORD lib_id,
                             const char *name, UWORD num_vectors,
                             const struct M68KThunkEntry *thunks, ULONG num_thunks,
                             const struct M68KThunkEntry *gen_thunks, ULONG num_gen_thunks);

/* Contained Exec interrupt API (m68kemu_interrupts.c) */
BOOL   M68KEmu_IntInstall(struct M68KEmuContext *ctx, ULONG rte_addr);
void   M68KEmu_AddIntServer(struct M68KEmuContext *ctx, ULONG intNum, ULONG irq);
void   M68KEmu_RemIntServer(struct M68KEmuContext *ctx, ULONG intNum, ULONG irq);
ULONG  M68KEmu_SetIntVector(struct M68KEmuContext *ctx, ULONG intNum, ULONG irq);
BOOL   M68KEmu_Disable(struct M68KEmuContext *ctx);
BOOL   M68KEmu_Enable(struct M68KEmuContext *ctx);

/* Dispatcher stubs live in the user-vector area after the LINEA RTE and
   the Supervisor() trampoline (0x100-0x10B); one per level 1-6. */
#define M68KEMU_INT_STUBS       0x000110UL
#define M68KEMU_INT_STUB_SIZE   44UL

/* Lookup fake base by library name (used by OpenLibrary thunk) */
ULONG  M68KEmu_FindLibBase(struct M68KEmuContext *ctx, const char *name);

/* Open/register a library by name, return fake base (no cpu needed) */
ULONG  M68KEmu_OpenLibrary(struct M68KEmuContext *ctx, const char *name);

/* Scan containment memory for C startup auto-open tables and fill them */
void   M68KEmu_AutoOpenLibs(struct M68KEmuContext *ctx);

#ifdef M68KEMU_WITH_RIGEL
BOOL   M68KEmu_RigelInit(struct M68KEmuContext *ctx);
void   M68KEmu_RigelDestroy(struct M68KEmuContext *ctx);
BOOL   M68KEmu_DisplayAttach(struct M68KEmuContext *ctx);
void   M68KEmu_RigelInputKey(struct M68KEmuContext *ctx, UBYTE rawkey);
void   M68KEmu_RigelInputMouse(struct M68KEmuContext *ctx, LONG dx, LONG dy);
void   M68KEmu_RigelInputButtons(struct M68KEmuContext *ctx, BOOL left, BOOL right);
void   M68KEmu_RigelIntState(struct M68KEmuContext *ctx, UWORD *intena, UWORD *intreq);
BOOL   M68KEmu_RigelRead(struct M68KEmuContext *ctx, ULONG addr,
                         UBYTE size, ULONG *value);
BOOL   M68KEmu_RigelWrite(struct M68KEmuContext *ctx, ULONG addr,
                          UBYTE size, ULONG value);
void   M68KEmu_RigelStepCPU(struct M68KEmuContext *ctx, unsigned long long cycles);
UBYTE  M68KEmu_RigelIPL(const struct M68KEmuContext *ctx);
#endif

#ifdef __cplusplus
}
#endif

#endif
