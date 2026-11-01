/**
 * @file self_diag.h
 * @brief Self diagnostics: memory, stacks, CPU load, flash and code benchmarks.
 *
 * Compiled in only when ENABLE_SELF_DIAGNOSTICS is defined; otherwise every call
 * below becomes an empty inline function, so instrumented code costs nothing.
 *
 * Report format (one metric per line): "group\tname\tvalue\tunit\n".
 */
#ifndef SELF_DIAG_H
#define SELF_DIAG_H

#include <stdint.h>
#include <stddef.h>
#include "build_config.h"
#include "hw_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Execution-time tracker for a code section (CPU cycles). */
typedef struct diag_bench {
    const char *group;
    const char *name;
    uint32_t t0;
    uint32_t last;
    uint32_t max;
    uint32_t count;
    uint64_t total;
    struct diag_bench *next;
} diag_bench_t;

/** Writer used by report providers. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} diag_writer_t;

typedef void (*diag_provider_t)(diag_writer_t *w);

#ifdef ENABLE_SELF_DIAGNOSTICS

#define DIAG_BENCH_INIT(group_, name_)  {.group = (group_), .name = (name_)}

void diag_init(void);

/** Add a benchmark to the report (call once). */
void diag_bench_register(diag_bench_t *b);

static inline void diag_bench_begin(diag_bench_t *b)
{
    b->t0 = hw_cycles();
}

static inline void diag_bench_end(diag_bench_t *b)
{
    uint32_t d = hw_cycles() - b->t0;
    b->last = d;
    b->total += d;
    b->count++;
    if (d > b->max) b->max = d;
}

/** Record an externally measured duration [cycles]. */
static inline void diag_bench_add(diag_bench_t *b, uint32_t cycles)
{
    b->last = cycles;
    b->total += cycles;
    b->count++;
    if (cycles > b->max) b->max = cycles;
}

/** Register a callback that appends module specific metrics to the report. */
void diag_register_provider(diag_provider_t fn);

/** Append one metric line. */
void diag_write(diag_writer_t *w, const char *group, const char *name, const char *unit,
                const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/** Produce the full report. Returns length. */
size_t diag_report(char *out, size_t cap);

/** Reset max trackers and benchmark statistics. */
void diag_reset(void);

#else /* !ENABLE_SELF_DIAGNOSTICS */

#define DIAG_BENCH_INIT(group_, name_)  {0}
static inline void diag_init(void) {}
static inline void diag_bench_register(diag_bench_t *b) { (void)b; }
static inline void diag_bench_begin(diag_bench_t *b) { (void)b; }
static inline void diag_bench_end(diag_bench_t *b) { (void)b; }
static inline void diag_bench_add(diag_bench_t *b, uint32_t c) { (void)b; (void)c; }
static inline void diag_register_provider(diag_provider_t fn) { (void)fn; }
static inline size_t diag_report(char *out, size_t cap) { if (cap) out[0] = '\0'; return 0; }
static inline void diag_reset(void) {}

#endif

#ifdef __cplusplus
}
#endif

#endif /* SELF_DIAG_H */
