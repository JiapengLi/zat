/*
 * Copyright 2026 Jiapeng Li <main@jiapeng.me>
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ZAT_H__
#define __ZAT_H__
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ZAT_ARG_MAX
#define ZAT_ARG_MAX 8
#endif

#ifndef ZAT_EOL
#define ZAT_EOL "\r\n"
#endif

#define ZAT_N(a) (sizeof(a) / sizeof((a)[0]))
#define ZAT_CMD(n, f) { (n), (f) }
#define ZAT_FMT(n, f) { (n), (f) }

enum {
    ZAT_OK     = 0,
    ZAT_ERROR  = -1,
    ZAT_EINVAL = -2,
    ZAT_ENOSPC = -3,
    ZAT_EIO    = -4,
    ZAT_EFMT   = -5,
};

typedef struct zat zat_t;

typedef enum {
    ZAT_EXEC,
    ZAT_CAPS,
    ZAT_GET,
    ZAT_SET,
} zat_op_t;

typedef int (*zat_write_fn)(void *arg, const void *buf, size_t len);
typedef int (*zat_cmd_fn)(zat_t *zat, zat_op_t op, int argc, char **argv);
typedef int (*zat_fmt_fn)(zat_t *zat, const void *obj, const char *opt, size_t optlen);

typedef struct {
    const char *name;
    zat_cmd_fn fn;
} zat_cmd_t;

typedef struct {
    const char *name;
    zat_fmt_fn fn;
} zat_fmt_t;

struct zat {
    char *buf;
    size_t cap;
    size_t len;

    const zat_cmd_t *cmds;
    size_t ncmd;

    const zat_fmt_t *fmts;
    size_t nfmt;

    zat_write_fn write;
    void *arg;

    uint8_t state;
};

int zat_init(zat_t *zat, char *buf, size_t size, zat_write_fn write, void *arg);

void zat_cmds(zat_t *zat, const zat_cmd_t *cmds, size_t count);
void zat_fmts(zat_t *zat, const zat_fmt_t *fmts, size_t count);

void *zat_arg(zat_t *zat);

int zat_parse_i32(const char *text, int32_t min, int32_t max, int32_t *value);
int zat_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value);
int zat_parse_bool(const char *text, int *value);
int zat_parse_hex(const char *text, void *buf, size_t capacity, size_t *length);

int zat_feed(zat_t *zat, const void *buf, size_t len);

int zat_write(zat_t *zat, const void *buf, size_t len);
int zat_printf(zat_t *zat, const char *fmt, ...);
int zat_vprintf(zat_t *zat, const char *fmt, va_list ap);
int zat_println(zat_t *zat, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
