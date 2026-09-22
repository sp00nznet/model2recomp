/*
 * Function dispatch table.
 *
 * Hash table mapping i960 addresses to native function pointers.
 * Uses open addressing with linear probing.
 */

#include "model2recomp/func_table.h"
#include "model2recomp/i960.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* Power of two, and it must be larger than the number of functions a game
 * registers - open addressing with linear probing, so a full table refuses.
 *
 * 8192 was chosen against Virtua Cop's 2,300, and fourteen of the thirty-five
 * sets in the corpus exceed it: Power Sled 21,131, Dynamite Cop 17,878,
 * Dynamite Baseball 17,678. Those games register the first 8,192 and every
 * dispatch to the rest misses, which looks like the game losing its way rather
 * than like a table that is too small. It is a real defect and the message
 * below names it.
 *
 * It is *not* fixed by raising this number, which was tried at 65536. Power
 * Sled starts drawing, and Virtua Cop 2 segfaults: with a complete table the
 * dispatches that used to miss now resolve, and some of them resolve to
 * functions the lifter produced from data rather than code. Truncation was
 * accidentally shielding them. Raising this is the second half of a fix whose
 * first half is the lifter not emitting those functions - see
 * docs/technical/porting-targets.md. */
#define TABLE_SIZE 8192
#define TABLE_MASK (TABLE_SIZE - 1)

/* The recursion guard, and it is a guard rather than a model of anything: the
 * i960's own call depth is bounded by its stack, not by a counter here.
 *
 * 500 was too low, and not because games nest that deep. The lifter sometimes
 * splits a routine into one-instruction fragments that tail-call each other,
 * and a loop in that shape is recursion - so a fill loop of a few thousand
 * iterations exhausts the counter and is abandoned part-way. Twenty of the
 * thirty-five sets in the corpus were hitting this; Manx TT hit it 3,570 times
 * in one run, and Virtua Cop 2's colour-ramp fill stopped after 228 of 24,576
 * entries because of it.
 *
 * It is still host stack, so it is not free: 200,000 overflows a default 1 MB
 * thread stack and kills the process before the first field. The corpus
 * launcher links with a 256 MB stack reserve (CMakeLists) to give this room,
 * and MODEL2_CALLDEPTH overrides the cap for anything that needs more. */
#define MAX_CALL_DEPTH_DEFAULT 20000
/* How many *distinct* addresses the dispatch-miss log reports.
 *
 * It used to be twenty occurrences rather than twenty addresses, and a game
 * that misses one address inside a loop fills the log with twenty copies of it
 * and never mentions the rest. tools/corpus.py discover harvests its entry
 * hints from this log, so it was being handed one address per round when there
 * were dozens. MODEL2_MISSLOG overrides the cap. */
#define MAX_MISS_LOG   256

unsigned long g_dispatches;
static uint32_t s_ring[32];
static int s_ring_pos;

/* MODEL2_PROFILE counts dispatches per function and prints the busiest at
 * exit: when the game stops making progress, the loop it is stuck in is at
 * the top, and diffing that against a run that does progress names the
 * function that is waiting. */
static uint32_t s_prof_addr[8192];
static uint32_t s_prof_hits[8192];

static void prof_dump(void)
{
    for (int i = 0; i < 8192; i++)
        for (int j = i + 1; j < 8192; j++)
            if (s_prof_hits[j] > s_prof_hits[i]) {
                uint32_t t = s_prof_hits[i]; s_prof_hits[i] = s_prof_hits[j]; s_prof_hits[j] = t;
                t = s_prof_addr[i]; s_prof_addr[i] = s_prof_addr[j]; s_prof_addr[j] = t;
            }
    int top = 25;
    { const char *e = getenv("MODEL2_PROFILE");
      if (e && atoi(e) > 1) top = atoi(e); }
    for (int i = 0; i < top && i < 8192 && s_prof_hits[i]; i++)
        fprintf(stderr, "[prof] %08X %u\n", s_prof_addr[i], s_prof_hits[i]);
}

/* A crashing game never reaches atexit, which is exactly when you most want
 * the ring. Catch the fault, print it, and let the default handler finish the
 * job - lifted code can and does dereference a bad address, and "segmentation
 * fault" on its own says nothing about which of eleven thousand functions did
 * it. */
void func_table_dump_ring(void);

static void ring_on_fault(int sig)
{
    fprintf(stderr, "\n[func_table] signal %d - last dispatches:\n", sig);
    func_table_dump_ring();
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* MODEL2_RING dumps the last 32 dispatches at exit: when the game stops
 * making progress, this names the loop it is stuck in. */
void func_table_dump_ring(void)
{
    fprintf(stderr, "[ring]");
    for (int i = 0; i < 32; i++)
        fprintf(stderr, " %08X", s_ring[(s_ring_pos + i) & 31]);
    fprintf(stderr, "\n");
}

typedef struct {
    uint32_t    addr;
    i960_func_t func;
    bool        occupied;
} table_entry_t;

static table_entry_t s_table[TABLE_SIZE];
static int s_call_depth = 0;

static int func_table_max_depth(void)
{
    static int cap = -1;
    if (cap < 0) {
        const char *e = getenv("MODEL2_CALLDEPTH");
        cap = (e && atoi(e) > 0) ? atoi(e) : MAX_CALL_DEPTH_DEFAULT;
    }
    return cap;
}
uint32_t g_cur_func = 0;   /* debug: MODEL2_WATCH */
static int s_miss_count = 0;
static uint32_t s_miss_seen[MAX_MISS_LOG];

/* True the first time this address misses, false afterwards. */
static bool miss_is_new(uint32_t addr)
{
    for (int i = 0; i < s_miss_count; i++)
        if (s_miss_seen[i] == addr) return false;
    static int cap = -1;
    if (cap < 0) {
        const char *e = getenv("MODEL2_MISSLOG");
        cap = (e && atoi(e) > 0) ? atoi(e) : MAX_MISS_LOG;
        if (cap > MAX_MISS_LOG) cap = MAX_MISS_LOG;
    }
    if (s_miss_count >= cap) return false;
    s_miss_seen[s_miss_count++] = addr;
    return true;
}

static inline uint32_t hash_addr(uint32_t addr)
{
    /* MurmurHash-like mixing */
    addr ^= addr >> 16;
    addr *= 0x45d9f3b;
    addr ^= addr >> 16;
    return addr & TABLE_MASK;
}

void func_table_init(void)
{
    memset(s_table, 0, sizeof(s_table));
    s_call_depth = 0;
    s_miss_count = 0;
    if (getenv("MODEL2_RING")) atexit(func_table_dump_ring);
    /* Always, not only under MODEL2_RING: a crash is the case where this
     * costs nothing and is the only record of what happened. */
    signal(SIGSEGV, ring_on_fault);
    signal(SIGABRT, ring_on_fault);
    if (getenv("MODEL2_PROFILE")) atexit(prof_dump);
    printf("[func_table] Initialized (%d slots)\n", TABLE_SIZE);
}

void func_table_register(uint32_t i960_addr, i960_func_t func)
{
    uint32_t idx = hash_addr(i960_addr);

    for (int i = 0; i < TABLE_SIZE; i++) {
        uint32_t probe = (idx + i) & TABLE_MASK;
        if (!s_table[probe].occupied || s_table[probe].addr == i960_addr) {
            s_table[probe].addr = i960_addr;
            s_table[probe].func = func;
            s_table[probe].occupied = true;
            return;
        }
    }

    /* Once, not once per function: a game that overflows does so thousands of
     * times, and the first line - the one that says what happened - scrolls
     * away behind the rest. */
    static bool told;
    if (!told) {
        told = true;
        fprintf(stderr, "[func_table] ERROR: table full at %d slots, starting "
                "with 0x%08X - raise TABLE_SIZE\n", TABLE_SIZE, i960_addr);
    }
}

i960_func_t func_table_lookup(uint32_t i960_addr)
{
    uint32_t idx = hash_addr(i960_addr);

    for (int i = 0; i < TABLE_SIZE; i++) {
        uint32_t probe = (idx + i) & TABLE_MASK;
        if (!s_table[probe].occupied)
            return NULL;
        if (s_table[probe].addr == i960_addr)
            return s_table[probe].func;
    }

    return NULL;
}

/* MODEL2_TRACE=N prints the first N dispatches, indented by call depth.
 * The last line before a hang names the function that is spinning. */
static long s_trace_left = -1;

bool func_table_call(uint32_t i960_addr)
{
    if (s_trace_left < 0) {
        const char *e = getenv("MODEL2_TRACE");
        s_trace_left = e ? strtol(e, NULL, 10) : 0;
    }
    if (s_trace_left > 0) {
        s_trace_left--;
        printf("%*s0x%08X\n", s_call_depth, "", i960_addr);
        fflush(stdout);
    }

    i960_func_t func = func_table_lookup(i960_addr);
    if (!func) {
        if (getenv("MODEL2_MISSFROM")) {
            static int b = 60;
            if (b > 0) {
                b--;
                fprintf(stderr, "[miss] 0x%08X from 0x%08X\n",
                        i960_addr, g_cur_func);
            }
        } else if (miss_is_new(i960_addr)) {
            printf("[func_table] MISS: no function at 0x%08X\n", i960_addr);
        }
        return false;
    }

    if (s_call_depth >= func_table_max_depth()) {
        fprintf(stderr, "[func_table] ERROR: Max call depth (%d) exceeded at 0x%08X\n",
                func_table_max_depth(), i960_addr);
        return false;
    }

    s_call_depth++;
    g_dispatches++;
    {
        /* MODEL2_CALLERS=0xADDR counts who dispatches to one function. */
        static uint32_t s_watch_fn = 1;
        if (s_watch_fn == 1) {
            const char *e = getenv("MODEL2_CALLERS");
            s_watch_fn = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
        }
        uint32_t key = (s_watch_fn && i960_addr == s_watch_fn)
                     ? g_cur_func : i960_addr;
        if (s_watch_fn && i960_addr != s_watch_fn)
            key = 0;
        uint32_t h = hash_addr(key);
        while (s_prof_addr[h] && s_prof_addr[h] != key)
            h = (h + 1) & TABLE_MASK;
        s_prof_addr[h] = key;
        s_prof_hits[h]++;
    }
    s_ring[s_ring_pos] = i960_addr; s_ring_pos = (s_ring_pos + 1) & 31;
    uint32_t prev = g_cur_func; g_cur_func = i960_addr;
    uint32_t sp_in = g_i960.r[1];

    func();

    /*
     * MODEL2_LEAK names functions that return with the guest stack higher than
     * they found it. Only the innermost is reported, since a leak shows up in
     * every caller above it too; that innermost one is the function whose
     * generated C has a path that never reaches its "ret".
     *
     * Function discovery is what produces them: a data table following a "ret"
     * looks like an entry point, splits the real function in two, and a path
     * through the far half falls off the end. One leaked frame per field is
     * enough for the stack to climb into the PRCB within a minute.
     */
    static int s_child_leaked;
    int child_leaked = s_child_leaked;
    s_child_leaked = 0;
    if (g_i960.r[1] > sp_in) {
        s_child_leaked = 1;
        static int budget = 40;
        if (!child_leaked && budget > 0 && getenv("MODEL2_LEAK")) {
            budget--;
            fprintf(stderr, "[leak] %08X left sp %08X -> %08X\n",
                    i960_addr, sp_in, g_i960.r[1]);
            if (getenv("MODEL2_LEAKPATH")) {
                fprintf(stderr, "  path:");
                for (int i = 0; i < 24; i++)
                    fprintf(stderr, " %08X",
                            s_ring[(s_ring_pos + i) & 31]);
                fprintf(stderr, "\n");
            }
        }
    }

    g_cur_func = prev;
    s_call_depth--;
    return true;
}
