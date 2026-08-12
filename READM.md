# ZAT

ZAT is a small C99 AT command server for MCU applications. It does not use dynamic memory, an RTOS, or a hardware-specific HAL. Each `zat_t` instance uses an RX buffer and a write callback supplied by the application.

## Command forms

```text
AT             Basic attention command
AT+CMD         ZAT_EXEC
AT+CMD=?       ZAT_CAPS
AT+CMD?        ZAT_GET
AT+CMD=...     ZAT_SET
```

A handler returning `ZAT_OK` causes ZAT to append `OK`. A negative return value causes ZAT to append `ERROR`.

Input commands may end with `CRLF` (`\r\n`), `CR` (`\r`), or `LF` (`\n`). Each terminator completes the current non-empty line. The second byte of `CRLF` is therefore treated as an empty line and ignored.

## Minimal example

```c
#include "zat/zat.h"

#include <stdio.h>
#include <string.h>

static int output_write(void *arg, const void *buf, size_t len)
{
    FILE *stream = arg;

    return fwrite(buf, 1, len, stream) == len ? ZAT_OK : ZAT_EIO;
}

static int cmd_ver(zat_t *zat, zat_op_t op, int argc, char **argv)
{
    int result;

    (void)argc;
    (void)argv;
    if (op != ZAT_EXEC)
        return ZAT_ERROR;

    result = zat_println(zat, "+VER:1.0.0");
    return result == ZAT_OK ? ZAT_OK : result;
}

int main(void)
{
    static const zat_cmd_t commands[] = {
        ZAT_CMD("VER", cmd_ver),
    };
    static const char input[] = "AT\r\nAT+VER\r\n";
    char rx_buffer[64];
    zat_t zat;

    if (zat_init(&zat, rx_buffer, sizeof(rx_buffer), output_write, stdout) != ZAT_OK)
        return 1;

    zat_cmds(&zat, commands, ZAT_N(commands));
    return zat_feed(&zat, input, strlen(input)) == ZAT_OK ? 0 : 1;
}
```

Expected output:

```text
OK
+VER:1.0.0
OK
```

On an MCU, replace `output_write` with a UART, USB CDC, FIFO, or ring-buffer writer. The callback must consume the complete buffer before returning `ZAT_OK`; partial writes are not supported.

## Argument parsing

SET arguments are passed to handlers as `argc` and `argv`. Each string is valid only during the handler call. ZAT provides strict helpers for common value types:

```c
int zat_parse_i32(const char *text, int32_t min, int32_t max, int32_t *value);
int zat_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value);
int zat_parse_bool(const char *text, int *value);
int zat_parse_hex(const char *text, void *buf, size_t capacity, size_t *length);
```

Example handler validation:

```c
int32_t offset;
uint32_t port;
int enabled;
uint8_t key[32];
size_t key_len;

if (argc != 4)
    return ZAT_ERROR;
if (zat_parse_i32(argv[0], -1000, 1000, &offset) != ZAT_OK)
    return ZAT_ERROR;
if (zat_parse_u32(argv[1], 1, 65535, &port) != ZAT_OK)
    return ZAT_ERROR;
if (zat_parse_bool(argv[2], &enabled) != ZAT_OK)
    return ZAT_ERROR;
if (zat_parse_hex(argv[3], key, sizeof(key), &key_len) != ZAT_OK)
    return ZAT_ERROR;
```

`zat_parse_bool` accepts `0/1`, `false/true`, `off/on`, and `disable/enable`, ignoring ASCII letter case.

`zat_parse_hex` accepts continuous hex and space- or tab-separated fields. Each field may start with `0x`. An odd-length field is padded with a leading zero nibble:

```text
"a 0b c d 0xFFA" -> 0a 0b 0c 0d 0f fa
"AB"             -> ab
"A B"            -> 0a 0b
```

Parse failures do not modify output values or buffers. Invalid values return `ZAT_EINVAL`; an undersized hex output buffer returns `ZAT_ENOSPC`.

## Build and test

From an MSYS2 UCRT64 shell:

```sh
make test
```

All build products are placed in `_build`. The human-review test report is written to `_build/test.log`.
