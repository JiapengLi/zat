/*
 * Copyright 2026 Jiapeng Li <main@jiapeng.me>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zat.h"

#include <limits.h>
#include <string.h>

enum {
    ZAT_STATE_LINE,
    ZAT_STATE_DISCARD,
};

typedef enum {
    ZAT_LEN_DEFAULT,
    ZAT_LEN_HH,
    ZAT_LEN_H,
    ZAT_LEN_L,
    ZAT_LEN_LL,
    ZAT_LEN_Z,
} zat_length_t;

static int zat_final(zat_t *zat, const char *response)
{
    return zat_println(zat, "%s", response);
}

static int zat_is_space(char value)
{
    return value == ' ' || value == '\t';
}

static char *zat_skip_space(char *text)
{
    while (zat_is_space(*text))
        ++text;
    return text;
}

static unsigned char zat_fold_case(unsigned char value)
{
    if (value >= 'a' && value <= 'z')
        return (unsigned char)(value - ('a' - 'A'));
    return value;
}

static int zat_name_equal(const char *name, const char *text, size_t len)
{
    size_t i;

    for (i = 0; i < len; ++i) {
        if (name[i] == '\0' || zat_fold_case((unsigned char)name[i]) != zat_fold_case((unsigned char)text[i]))
            return 0;
    }
    return name[len] == '\0';
}

static const zat_cmd_t *zat_find_cmd(const zat_t *zat, const char *name, size_t len)
{
    size_t i;

    for (i = 0; i < zat->ncmd; ++i) {
        if (zat_name_equal(zat->cmds[i].name, name, len))
            return &zat->cmds[i];
    }

    return NULL;
}

static const zat_fmt_t *zat_find_fmt(const zat_t *zat, const char *name, size_t len)
{
    size_t i;

    for (i = 0; i < zat->nfmt; ++i) {
        if (zat_name_equal(zat->fmts[i].name, name, len))
            return &zat->fmts[i];
    }

    return NULL;
}

static int zat_parse_args(char *text, int *argc, char **argv)
{
    char *p = zat_skip_space(text);
    int count = 0;

    if (*p == '\0') {
        *argc = 0;
        return ZAT_OK;
    }

    for (;;) {
        char delimiter;

        if (count >= ZAT_ARG_MAX)
            return ZAT_ENOSPC;

        if (*p == '"') {
            char *tail;

            ++p;
            argv[count++] = p;
            while (*p != '\0' && *p != '"')
                ++p;
            if (*p != '"')
                return ZAT_EINVAL;
            *p++ = '\0';
            tail = zat_skip_space(p);
            if (*tail != '\0' && *tail != ',')
                return ZAT_EINVAL;
            delimiter = *tail;
            p = tail;
        } else {
            char *tail;

            argv[count++] = p;
            while (*p != '\0' && *p != ',') {
                if (*p == '"')
                    return ZAT_EINVAL;
                ++p;
            }
            delimiter = *p;
            tail = p;
            while (tail != argv[count - 1] && zat_is_space(tail[-1]))
                --tail;
            *tail = '\0';
        }

        if (delimiter == '\0')
            break;
        p = zat_skip_space(p + 1);
    }

    *argc = count;
    return ZAT_OK;
}

static int zat_dispatch(zat_t *zat, char *line)
{
    const zat_cmd_t *cmd;
    char *name;
    char *p;
    char *tail;
    char *argv[ZAT_ARG_MAX];
    zat_op_t op;
    size_t name_len;
    int argc = 0;
    int result;

    p = zat_skip_space(line);
    if (zat_fold_case((unsigned char)p[0]) != 'A' || zat_fold_case((unsigned char)p[1]) != 'T')
        return zat_final(zat, "ERROR");
    p = zat_skip_space(p + 2);
    if (*p == '\0')
        return zat_final(zat, "OK");
    if (*p++ != '+')
        return zat_final(zat, "ERROR");

    name = zat_skip_space(p);
    p = name;
    while (*p != '\0' && *p != '?' && *p != '=' && !zat_is_space(*p))
        ++p;
    name_len = (size_t)(p - name);
    if (name_len == 0)
        return zat_final(zat, "ERROR");
    p = zat_skip_space(p);

    if (*p == '\0') {
        op = ZAT_EXEC;
    } else if (*p == '?') {
        if (*zat_skip_space(p + 1) != '\0')
            return zat_final(zat, "ERROR");
        op = ZAT_GET;
    } else if (*p == '=') {
        tail = zat_skip_space(p + 1);
        if (*tail == '?' && *zat_skip_space(tail + 1) == '\0') {
            op = ZAT_CAPS;
        } else {
            op = ZAT_SET;
            result = zat_parse_args(tail, &argc, argv);
            if (result != ZAT_OK)
                return zat_final(zat, "ERROR");
        }
    } else {
        return zat_final(zat, "ERROR");
    }

    cmd = zat_find_cmd(zat, name, name_len);
    if (cmd == NULL)
        return zat_final(zat, "ERROR");

    result = cmd->fn(zat, op, argc, argv);
    if (result == ZAT_OK)
        return zat_final(zat, "OK");
    return zat_final(zat, "ERROR");
}

static int zat_repeat(zat_t *zat, char value, size_t count)
{
    static const char spaces[] = "                                ";
    static const char zeros[] = "00000000000000000000000000000000";
    const char *block = value == '0' ? zeros : spaces;
    size_t block_size = sizeof(spaces) - 1;

    while (count != 0) {
        size_t n = count < block_size ? count : block_size;
        int result = zat_write(zat, block, n);

        if (result != ZAT_OK)
            return result;
        count -= n;
    }

    return ZAT_OK;
}

static int zat_text(zat_t *zat, const char *text, size_t len, size_t width, int left)
{
    size_t padding = width > len ? width - len : 0;
    int result;

    if (!left) {
        result = zat_repeat(zat, ' ', padding);
        if (result != ZAT_OK)
            return result;
    }

    result = zat_write(zat, text, len);
    if (result != ZAT_OK)
        return result;

    if (left)
        return zat_repeat(zat, ' ', padding);
    return ZAT_OK;
}

static int zat_number(zat_t *zat, uintmax_t value, int negative, unsigned int base, int upper, size_t width, int zero, int left, int precision_set, size_t precision, const char *prefix)
{
    char digits[sizeof(uintmax_t) * CHAR_BIT + 1];
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t prefix_len = prefix == NULL ? 0 : strlen(prefix);
    size_t digit_count = 0;
    size_t zero_count;
    size_t content_len;
    size_t padding;
    int result;

    if (value != 0 || !precision_set || precision != 0) {
        do {
            digits[digit_count++] = alphabet[value % base];
            value /= base;
        } while (value != 0);
    }

    zero_count = precision_set && precision > digit_count ? precision - digit_count : 0;
    content_len = (negative ? 1u : 0u) + prefix_len + zero_count + digit_count;
    padding = width > content_len ? width - content_len : 0;

    if (!left && (!zero || precision_set)) {
        result = zat_repeat(zat, ' ', padding);
        if (result != ZAT_OK)
            return result;
    }

    if (negative) {
        result = zat_write(zat, "-", 1);
        if (result != ZAT_OK)
            return result;
    }
    if (prefix_len != 0) {
        result = zat_write(zat, prefix, prefix_len);
        if (result != ZAT_OK)
            return result;
    }

    if (!left && zero && !precision_set)
        zero_count += padding;
    result = zat_repeat(zat, '0', zero_count);
    if (result != ZAT_OK)
        return result;

    while (digit_count != 0) {
        --digit_count;
        result = zat_write(zat, &digits[digit_count], 1);
        if (result != ZAT_OK)
            return result;
    }

    if (left)
        return zat_repeat(zat, ' ', padding);
    return ZAT_OK;
}

static int zat_parse_size(const char **fmt, size_t *value)
{
    size_t result = 0;

    while (**fmt >= '0' && **fmt <= '9') {
        unsigned int digit = (unsigned int)(**fmt - '0');

        if (result > (SIZE_MAX - digit) / 10)
            return ZAT_EFMT;
        result = result * 10 + digit;
        ++*fmt;
    }

    *value = result;
    return ZAT_OK;
}

static int zat_custom(zat_t *zat, const char **fmt, const zat_fmt_t **formatter, const char **option, size_t *option_len)
{
    const char *name = *fmt + 1;
    const char *p = name;
    const char *opt = NULL;
    size_t name_len;
    size_t opt_len = 0;

    while (*p != '\0' && *p != ':' && *p != '}')
        ++p;
    name_len = (size_t)(p - name);
    if (name_len == 0)
        return ZAT_EFMT;

    if (*p == ':') {
        opt = ++p;
        while (*p != '\0' && *p != '}')
            ++p;
        opt_len = (size_t)(p - opt);
    }
    if (*p != '}')
        return ZAT_EFMT;

    *formatter = zat_find_fmt(zat, name, name_len);
    if (*formatter == NULL)
        return ZAT_EFMT;

    *option = opt;
    *option_len = opt_len;
    *fmt = p + 1;
    return ZAT_OK;
}

static int zat_parse_uint(const char *text, uint32_t limit, uint32_t *value)
{
    uint32_t result = 0;

    if (*text == '\0')
        return ZAT_EINVAL;
    while (*text != '\0') {
        unsigned int digit;

        if (*text < '0' || *text > '9')
            return ZAT_EINVAL;
        digit = (unsigned int)(*text - '0');
        if (result > (limit - digit) / 10u)
            return ZAT_EINVAL;
        result = result * 10u + digit;
        ++text;
    }

    *value = result;
    return ZAT_OK;
}

static int zat_hex_value(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    value = (char)zat_fold_case((unsigned char)value);
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static int zat_hex_space(char value)
{
    return value == ' ' || value == '\t';
}

int zat_init(zat_t *zat, char *buf, size_t size, zat_write_fn write, void *arg)
{
    if (zat == NULL || buf == NULL || size == 0 || write == NULL)
        return ZAT_EINVAL;

    memset(zat, 0, sizeof(*zat));
    zat->buf = buf;
    zat->cap = size;
    zat->write = write;
    zat->arg = arg;
    zat->state = ZAT_STATE_LINE;
    return ZAT_OK;
}

void zat_cmds(zat_t *zat, const zat_cmd_t *cmds, size_t count)
{
    size_t i;

    if (zat == NULL)
        return;
    if (cmds == NULL)
        count = 0;
    for (i = 0; i < count; ++i) {
        if (cmds[i].name == NULL || cmds[i].name[0] == '\0' || cmds[i].fn == NULL) {
            count = 0;
            break;
        }
    }
    zat->cmds = count == 0 ? NULL : cmds;
    zat->ncmd = count;
}

void zat_fmts(zat_t *zat, const zat_fmt_t *fmts, size_t count)
{
    size_t i;

    if (zat == NULL)
        return;
    if (fmts == NULL)
        count = 0;
    for (i = 0; i < count; ++i) {
        if (fmts[i].name == NULL || fmts[i].name[0] == '\0' || fmts[i].fn == NULL) {
            count = 0;
            break;
        }
    }
    zat->fmts = count == 0 ? NULL : fmts;
    zat->nfmt = count;
}

void *zat_arg(zat_t *zat)
{
    return zat->arg;
}

int zat_parse_i32(const char *text, int32_t min, int32_t max, int32_t *value)
{
    const char *digits;
    uint32_t magnitude;
    uint32_t limit;
    int negative;
    int32_t result;

    if (text == NULL || value == NULL)
        return ZAT_EINVAL;
    negative = *text == '-';
    digits = (*text == '-' || *text == '+') ? text + 1 : text;
    limit = negative ? (uint32_t)INT32_MAX + 1u : (uint32_t)INT32_MAX;
    if (zat_parse_uint(digits, limit, &magnitude) != ZAT_OK)
        return ZAT_EINVAL;

    if (negative) {
        if (magnitude == (uint32_t)INT32_MAX + 1u)
            result = INT32_MIN;
        else
            result = -(int32_t)magnitude;
    } else {
        result = (int32_t)magnitude;
    }
    if (result < min || result > max)
        return ZAT_EINVAL;

    *value = result;
    return ZAT_OK;
}

int zat_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    uint32_t result;

    if (text == NULL || value == NULL)
        return ZAT_EINVAL;
    if (zat_parse_uint(text, UINT32_MAX, &result) != ZAT_OK)
        return ZAT_EINVAL;
    if (result < min || result > max)
        return ZAT_EINVAL;

    *value = result;
    return ZAT_OK;
}

int zat_parse_bool(const char *text, int *value)
{
    size_t len;
    int result;

    if (text == NULL || value == NULL)
        return ZAT_EINVAL;
    len = strlen(text);
    if (zat_name_equal("0", text, len) || zat_name_equal("false", text, len) || zat_name_equal("off", text, len) || zat_name_equal("disable", text, len)) {
        result = 0;
    } else if (zat_name_equal("1", text, len) || zat_name_equal("true", text, len) || zat_name_equal("on", text, len) || zat_name_equal("enable", text, len)) {
        result = 1;
    } else {
        return ZAT_EINVAL;
    }

    *value = result;
    return ZAT_OK;
}

int zat_parse_hex(const char *text, void *buf, size_t capacity, size_t *length)
{
    const char *p;
    unsigned char *output = buf;
    size_t required = 0;
    size_t output_len = 0;
    int items = 0;

    if (text == NULL || buf == NULL || length == NULL)
        return ZAT_EINVAL;

    p = text;
    for (;;) {
        size_t digits = 0;
        size_t bytes;

        while (zat_hex_space(*p))
            ++p;
        if (*p == '\0')
            break;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        while (*p != '\0' && !zat_hex_space(*p)) {
            if (zat_hex_value(*p) < 0)
                return ZAT_EINVAL;
            ++digits;
            ++p;
        }
        if (digits == 0)
            return ZAT_EINVAL;
        bytes = digits / 2u + digits % 2u;
        if (required > SIZE_MAX - bytes)
            return ZAT_ENOSPC;
        required += bytes;
        items = 1;
    }
    if (!items)
        return ZAT_EINVAL;
    if (required > capacity)
        return ZAT_ENOSPC;

    p = text;
    for (;;) {
        const char *digits_start;
        const char *q;
        size_t digits = 0;

        while (zat_hex_space(*p))
            ++p;
        if (*p == '\0')
            break;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        digits_start = p;
        while (*p != '\0' && !zat_hex_space(*p)) {
            ++digits;
            ++p;
        }
        q = digits_start;
        if (digits % 2u != 0) {
            output[output_len++] = (unsigned char)zat_hex_value(*q++);
            --digits;
        }
        while (digits != 0) {
            unsigned int high = (unsigned int)zat_hex_value(*q++);
            unsigned int low = (unsigned int)zat_hex_value(*q++);

            output[output_len++] = (unsigned char)(high << 4 | low);
            digits -= 2;
        }
    }

    *length = output_len;
    return ZAT_OK;
}

int zat_feed(zat_t *zat, const void *buf, size_t len)
{
    const unsigned char *input = buf;
    int status = ZAT_OK;
    size_t i;

    for (i = 0; i < len; ++i) {
        unsigned char byte = input[i];

        if (byte == '\r' || byte == '\n') {
            if (zat->state == ZAT_STATE_DISCARD) {
                int result = zat_final(zat, "ERROR");

                zat->state = ZAT_STATE_LINE;
                zat->len = 0;
                if (result != ZAT_OK)
                    return result;
            } else if (zat->len != 0) {
                int result;

                zat->buf[zat->len] = '\0';
                result = zat_dispatch(zat, zat->buf);
                zat->len = 0;
                if (result != ZAT_OK)
                    return result;
            }
            continue;
        }

        if (zat->state == ZAT_STATE_DISCARD)
            continue;

        if (zat->len + 1 >= zat->cap) {
            zat->state = ZAT_STATE_DISCARD;
            zat->len = 0;
            status = ZAT_ENOSPC;
            continue;
        }

        zat->buf[zat->len++] = (char)byte;
    }

    return status;
}

int zat_write(zat_t *zat, const void *buf, size_t len)
{
    if (len == 0)
        return ZAT_OK;
    return zat->write(zat->arg, buf, len) == ZAT_OK ? ZAT_OK : ZAT_EIO;
}

int zat_vprintf(zat_t *zat, const char *fmt, va_list ap)
{
    while (*fmt != '\0') {
        const char *start = fmt;
        size_t width = 0;
        size_t precision = 0;
        int precision_set = 0;
        int zero = 0;
        int left = 0;
        zat_length_t length = ZAT_LEN_DEFAULT;
        int result;
        char conversion;

        while (*fmt != '\0' && *fmt != '%')
            ++fmt;
        result = zat_write(zat, start, (size_t)(fmt - start));
        if (result != ZAT_OK || *fmt == '\0')
            return result;

        ++fmt;
        if (*fmt == '%') {
            result = zat_write(zat, "%", 1);
            if (result != ZAT_OK)
                return result;
            ++fmt;
            continue;
        }
        if (*fmt == '{') {
            const zat_fmt_t *formatter;
            const char *option;
            size_t option_len;
            const void *object;

            result = zat_custom(zat, &fmt, &formatter, &option, &option_len);
            if (result != ZAT_OK)
                return result;
            object = va_arg(ap, const void *);
            result = formatter->fn(zat, object, option, option_len);
            if (result != ZAT_OK)
                return result;
            continue;
        }

        for (;;) {
            if (*fmt == '0')
                zero = 1;
            else if (*fmt == '-')
                left = 1;
            else
                break;
            ++fmt;
        }

        result = zat_parse_size(&fmt, &width);
        if (result != ZAT_OK)
            return result;

        if (*fmt == '.') {
            ++fmt;
            precision_set = 1;
            result = zat_parse_size(&fmt, &precision);
            if (result != ZAT_OK)
                return result;
        }

        if (fmt[0] == 'h' && fmt[1] == 'h') {
            length = ZAT_LEN_HH;
            fmt += 2;
        } else if (*fmt == 'h') {
            length = ZAT_LEN_H;
            ++fmt;
        } else if (fmt[0] == 'l' && fmt[1] == 'l') {
            length = ZAT_LEN_LL;
            fmt += 2;
        } else if (*fmt == 'l') {
            length = ZAT_LEN_L;
            ++fmt;
        } else if (*fmt == 'z') {
            length = ZAT_LEN_Z;
            ++fmt;
        }

        conversion = *fmt++;
        if (conversion == '\0')
            return ZAT_EFMT;

        if (conversion == 's') {
            const char *text;
            size_t text_len = 0;

            if (length != ZAT_LEN_DEFAULT)
                return ZAT_EFMT;
            text = va_arg(ap, const char *);
            if (text == NULL)
                text = "(null)";
            while (text[text_len] != '\0' && (!precision_set || text_len < precision))
                ++text_len;
            result = zat_text(zat, text, text_len, width, left);
        } else if (conversion == 'c') {
            char value;

            if (length != ZAT_LEN_DEFAULT || precision_set)
                return ZAT_EFMT;
            value = (char)va_arg(ap, int);
            result = zat_text(zat, &value, 1, width, left);
        } else if (conversion == 'p') {
            uintptr_t value;

            if (length != ZAT_LEN_DEFAULT || precision_set)
                return ZAT_EFMT;
            value = (uintptr_t)va_arg(ap, void *);
            result = zat_number(zat, (uintmax_t)value, 0, 16, 0, width, zero, left, 0, 0, "0x");
        } else if (conversion == 'd' || conversion == 'i') {
            intmax_t value;
            uintmax_t magnitude;

            switch (length) {
            case ZAT_LEN_HH: value = (signed char)va_arg(ap, int); break;
            case ZAT_LEN_H: value = (short)va_arg(ap, int); break;
            case ZAT_LEN_L: value = va_arg(ap, long); break;
            case ZAT_LEN_LL: value = va_arg(ap, long long); break;
            case ZAT_LEN_Z: value = va_arg(ap, ptrdiff_t); break;
            default: value = va_arg(ap, int); break;
            }
            magnitude = value < 0 ? (uintmax_t)0 - (uintmax_t)value : (uintmax_t)value;
            result = zat_number(zat, magnitude, value < 0, 10, 0, width, zero, left, precision_set, precision, NULL);
        } else if (conversion == 'u' || conversion == 'x' || conversion == 'X') {
            uintmax_t value;
            unsigned int base = conversion == 'u' ? 10u : 16u;

            switch (length) {
            case ZAT_LEN_HH: value = (unsigned char)va_arg(ap, unsigned int); break;
            case ZAT_LEN_H: value = (unsigned short)va_arg(ap, unsigned int); break;
            case ZAT_LEN_L: value = va_arg(ap, unsigned long); break;
            case ZAT_LEN_LL: value = va_arg(ap, unsigned long long); break;
            case ZAT_LEN_Z: value = va_arg(ap, size_t); break;
            default: value = va_arg(ap, unsigned int); break;
            }
            result = zat_number(zat, value, 0, base, conversion == 'X', width, zero, left, precision_set, precision, NULL);
        } else {
            return ZAT_EFMT;
        }

        if (result != ZAT_OK)
            return result;
    }

    return ZAT_OK;
}

int zat_printf(zat_t *zat, const char *fmt, ...)
{
    va_list ap;
    int result;

    va_start(ap, fmt);
    result = zat_vprintf(zat, fmt, ap);
    va_end(ap);
    return result;
}

int zat_println(zat_t *zat, const char *fmt, ...)
{
    va_list ap;
    int result;

    va_start(ap, fmt);
    result = zat_vprintf(zat, fmt, ap);
    va_end(ap);
    if (result != ZAT_OK)
        return result;
    return zat_write(zat, ZAT_EOL, strlen(ZAT_EOL));
}
