/**
 * @file cli.c
 * @brief Command line tokenizer, registry and dispatcher.
 */
#include "cli.h"

#ifdef ENABLE_DEBUG_CLI

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const cli_cmd_t *cmds;
    size_t n;
} cli_table_t;

static cli_table_t s_tables[CLI_MAX_TABLES];
static size_t s_num_tables;

void cli_register(const cli_cmd_t *cmds, size_t n)
{
    if (s_num_tables < CLI_MAX_TABLES) {
        s_tables[s_num_tables].cmds = cmds;
        s_tables[s_num_tables].n = n;
        s_num_tables++;
    }
}

void cli_printf(cli_out_t *o, const char *fmt, ...)
{
    if (o->len + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (w > 0) {
        o->len += (size_t)w;
        if (o->len >= o->cap) o->len = o->cap - 1;
    }
}

static const cli_cmd_t *lookup(const char *name)
{
    for (size_t t = 0; t < s_num_tables; t++) {
        for (size_t i = 0; i < s_tables[t].n; i++) {
            if (strcmp(s_tables[t].cmds[i].name, name) == 0) return &s_tables[t].cmds[i];
        }
    }
    return NULL;
}

static void print_help(cli_out_t *out)
{
    cli_printf(out, "Commands (args: <required> [optional]):\n");
    for (size_t t = 0; t < s_num_tables; t++) {
        for (size_t i = 0; i < s_tables[t].n; i++) {
            const cli_cmd_t *c = &s_tables[t].cmds[i];
            char syn[48];
            snprintf(syn, sizeof(syn), "%s %s", c->name, c->args ? c->args : "");
            cli_printf(out, "  %-30s %s\n", syn, c->help ? c->help : "");
        }
    }
}

size_t cli_execute(const char *line, char *outbuf, size_t cap)
{
    cli_out_t out = {.buf = outbuf, .cap = cap, .len = 0};
    if (cap == 0) return 0;
    outbuf[0] = '\0';

    static char work[256];
    strncpy(work, line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *argv[CLI_MAX_ARGS];
    int argc = 0;
    char *p = work;
    while (*p && argc < CLI_MAX_ARGS) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') *p++ = '\0';
        if (!*p) break;
        if (*p == '"') {            /* quoted argument */
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        }
    }
    if (argc == 0) return 0;

    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
        print_help(&out);
        return out.len;
    }
    const cli_cmd_t *c = lookup(argv[0]);
    if (!c) {
        cli_printf(&out, "unknown command '%s' (try 'help')\n", argv[0]);
        return out.len;
    }
    int r = c->fn(argc, argv, &out);
    if (r != 0) {
        cli_printf(&out, "error %d. usage: %s %s\n", r, c->name, c->args ? c->args : "");
    }
    return out.len;
}

#endif /* ENABLE_DEBUG_CLI */
