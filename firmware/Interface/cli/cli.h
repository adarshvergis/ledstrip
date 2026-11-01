/**
 * @file cli.h
 * @brief Debug command line interpreter (compiled in only with ENABLE_DEBUG_CLI).
 *
 * Transport independent: the network layer passes a text line to cli_execute() and
 * sends the captured output back. Modules register command tables at start-up.
 */
#ifndef CLI_H
#define CLI_H

#include <stdint.h>
#include <stddef.h>
#include "build_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLI_MAX_ARGS        10
#define CLI_MAX_TABLES      8

/** Output sink handed to command handlers. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} cli_out_t;

typedef int (*cli_fn_t)(int argc, char **argv, cli_out_t *out);

typedef struct {
    const char *name;
    const char *args;       /**< argument synopsis, e.g. "<stage> [n]" */
    const char *help;
    cli_fn_t    fn;
} cli_cmd_t;

#ifdef ENABLE_DEBUG_CLI

/** Register a table of @p n commands (the table must stay valid). */
void cli_register(const cli_cmd_t *cmds, size_t n);

/** Execute one command line; output is written to @p out (NUL terminated). Returns length. */
size_t cli_execute(const char *line, char *out, size_t cap);

/** printf into the command output (silently truncates). */
void cli_printf(cli_out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#else

static inline void cli_register(const cli_cmd_t *cmds, size_t n) { (void)cmds; (void)n; }
static inline size_t cli_execute(const char *line, char *out, size_t cap) { (void)line; if (cap) out[0] = '\0'; return 0; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* CLI_H */
