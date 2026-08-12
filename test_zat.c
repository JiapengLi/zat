#include "zat/zat.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *command;
    zat_op_t op;
    int argc;
    char argv[ZAT_ARG_MAX][128];
} parse_event_t;

typedef struct {
    char output[16384];
    size_t output_len;
    size_t write_count;
    size_t fail_on_write;
    zat_op_t last_op;
    int last_argc;
    char last_argv[ZAT_ARG_MAX][128];
    int handler_calls;
    parse_event_t events[16];
    size_t event_count;
} app_t;

typedef struct {
    zat_t zat;
    char rx[128];
    app_t app;
} fixture_t;

typedef int (*test_fn)(void);

typedef struct {
    const char *command;
    zat_op_t op;
    int argc;
    const char *argv[ZAT_ARG_MAX];
} expected_event_t;

static int checks;
static int failures;

#define CHECK(expression) do { ++checks; if (!(expression)) { printf("  CHECK FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expression); return 0; } } while (0)

static void print_escaped(const void *data, size_t len)
{
    const unsigned char *bytes = data;
    size_t i;

    putchar('"');
    for (i = 0; i < len; ++i) {
        unsigned char value = bytes[i];

        if (value == '\r')
            fputs("\\r", stdout);
        else if (value == '\n')
            fputs("\\n", stdout);
        else if (value == '\t')
            fputs("\\t", stdout);
        else if (value == '\\')
            fputs("\\\\", stdout);
        else if (value == '"')
            fputs("\\\"", stdout);
        else if (value >= 0x20 && value <= 0x7e)
            putchar((int)value);
        else
            printf("\\x%02X", value);
    }
    putchar('"');
}

static int test_write(void *arg, const void *buf, size_t len)
{
    app_t *app = arg;

    ++app->write_count;
    if (app->fail_on_write != 0 && app->write_count == app->fail_on_write)
        return -1;
    if (len > sizeof(app->output) - app->output_len)
        return -1;
    memcpy(app->output + app->output_len, buf, len);
    app->output_len += len;
    return ZAT_OK;
}

static int record_call(zat_t *zat, const char *command, zat_op_t op, int argc, char **argv)
{
    app_t *app = zat_arg(zat);
    parse_event_t *event;
    int i;

    if (app->event_count == ZAT_N(app->events))
        return ZAT_ERROR;
    event = &app->events[app->event_count++];
    event->command = command;
    event->op = op;
    event->argc = argc;
    app->last_op = op;
    app->last_argc = argc;
    ++app->handler_calls;
    for (i = 0; i < argc; ++i) {
        size_t len = strlen(argv[i]);

        if (len >= sizeof(app->last_argv[i]))
            return ZAT_ERROR;
        memcpy(app->last_argv[i], argv[i], len + 1);
        memcpy(event->argv[i], argv[i], len + 1);
    }
    return ZAT_OK;
}

static int cmd_record(zat_t *zat, zat_op_t op, int argc, char **argv)
{
    return record_call(zat, "AAAAAA", op, argc, argv);
}

static int cmd_data(zat_t *zat, zat_op_t op, int argc, char **argv)
{
    int result = record_call(zat, "DATA", op, argc, argv);

    if (result != ZAT_OK)
        return result;
    if (op != ZAT_GET)
        return ZAT_ERROR;
    return zat_println(zat, "+DATA:42");
}

static int cmd_fail(zat_t *zat, zat_op_t op, int argc, char **argv)
{
    int result = record_call(zat, "FAIL", op, argc, argv);

    if (result != ZAT_OK)
        return result;
    return ZAT_ERROR;
}

static int cmd_parse(zat_t *zat, zat_op_t op, int argc, char **argv)
{
    unsigned char data[32];
    size_t data_len;
    int32_t signed_value;
    uint32_t unsigned_value;
    int bool_value;
    int result;
    size_t i;

    result = record_call(zat, "PARSE", op, argc, argv);
    if (result != ZAT_OK)
        return result;
    if (op != ZAT_SET || argc != 4)
        return ZAT_ERROR;
    if (zat_parse_i32(argv[0], -1000, 1000, &signed_value) != ZAT_OK)
        return ZAT_ERROR;
    if (zat_parse_u32(argv[1], 0, 100, &unsigned_value) != ZAT_OK)
        return ZAT_ERROR;
    if (zat_parse_bool(argv[2], &bool_value) != ZAT_OK)
        return ZAT_ERROR;
    if (zat_parse_hex(argv[3], data, sizeof(data), &data_len) != ZAT_OK)
        return ZAT_ERROR;

    result = zat_printf(zat, "+PARSE:%ld,%lu,%d,", (long)signed_value, (unsigned long)unsigned_value, bool_value);
    if (result != ZAT_OK)
        return result;
    for (i = 0; i < data_len; ++i) {
        result = zat_printf(zat, "%02X", data[i]);
        if (result != ZAT_OK)
            return result;
    }
    return zat_println(zat, "");
}

static int fmt_bytes(zat_t *zat, const void *obj, const char *opt, size_t optlen)
{
    const unsigned char *bytes = obj;

    if (opt == NULL)
        return zat_printf(zat, "%02X:%02X", bytes[0], bytes[1]);
    if (optlen == 3 && memcmp(opt, "raw", 3) == 0)
        return zat_printf(zat, "%02X%02X", bytes[0], bytes[1]);
    if (optlen == 0)
        return zat_write(zat, "empty", 5);
    return ZAT_EFMT;
}

static int fmt_ipv4(zat_t *zat, const void *obj, const char *opt, size_t optlen)
{
    const unsigned char *ip = obj;

    if (opt != NULL || optlen != 0)
        return ZAT_EFMT;
    return zat_printf(zat, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static const zat_cmd_t commands[] = {
    ZAT_CMD("AAAAAA", cmd_record),
    ZAT_CMD("DATA", cmd_data),
    ZAT_CMD("FAIL", cmd_fail),
    ZAT_CMD("PARSE", cmd_parse),
};

static const zat_fmt_t formats[] = {
    ZAT_FMT("bytes", fmt_bytes),
    ZAT_FMT("ipv4", fmt_ipv4),
};

static int fixture_init(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    if (zat_init(&fixture->zat, fixture->rx, sizeof(fixture->rx), test_write, &fixture->app) != ZAT_OK)
        return 0;
    zat_cmds(&fixture->zat, commands, ZAT_N(commands));
    zat_fmts(&fixture->zat, formats, ZAT_N(formats));
    return 1;
}

static void clear_output(fixture_t *fixture)
{
    fixture->app.output_len = 0;
    fixture->app.write_count = 0;
    fixture->app.fail_on_write = 0;
}

static int output_is(const fixture_t *fixture, const char *expected)
{
    size_t len = strlen(expected);

    return fixture->app.output_len == len && memcmp(fixture->app.output, expected, len) == 0;
}

static int feed_text(fixture_t *fixture, const char *text)
{
    return zat_feed(&fixture->zat, text, strlen(text));
}

static int call_vprintf(zat_t *zat, const char *fmt, ...)
{
    va_list ap;
    int result;

    va_start(ap, fmt);
    result = zat_vprintf(zat, fmt, ap);
    va_end(ap);
    return result;
}

static const char *op_name(zat_op_t op)
{
    switch (op) {
    case ZAT_EXEC: return "EXEC";
    case ZAT_CAPS: return "CAPS";
    case ZAT_GET: return "GET";
    case ZAT_SET: return "SET";
    default: return "UNKNOWN";
    }
}

static void print_instruction_lines(const char *stream, size_t len)
{
    size_t start = 0;
    size_t count = 0;
    size_t i;

    for (i = 0; i <= len; ++i) {
        if (i == len || stream[i] == '\r' || stream[i] == '\n') {
            if (i != start) {
                printf("     instruction[%zu] = ", count++);
                print_escaped(stream + start, i - start);
                fputc('\n', stdout);
            }
            start = i + 1;
        }
    }
    if (count == 0)
        puts("     (no complete instruction)");
}

static void print_expected_events(const expected_event_t *events, size_t count)
{
    size_t i;

    if (count == 0) {
        puts("     (no registered handler dispatch; basic AT or rejected instruction)");
        return;
    }
    for (i = 0; i < count; ++i) {
        int arg;

        printf("     event[%zu]: command=%s, op=%s, argc=%d\n", i, events[i].command, op_name(events[i].op), events[i].argc);
        for (arg = 0; arg < events[i].argc; ++arg) {
            printf("       argv[%d] = ", arg);
            print_escaped(events[i].argv[arg], strlen(events[i].argv[arg]));
            fputc('\n', stdout);
        }
    }
}

static void print_actual_events(const app_t *app)
{
    size_t i;

    if (app->event_count == 0) {
        puts("     (no registered handler dispatch; basic AT or rejected instruction)");
        return;
    }
    for (i = 0; i < app->event_count; ++i) {
        const parse_event_t *event = &app->events[i];
        int arg;

        printf("     event[%zu]: command=%s, op=%s, argc=%d\n", i, event->command, op_name(event->op), event->argc);
        for (arg = 0; arg < event->argc; ++arg) {
            printf("       argv[%d] = ", arg);
            print_escaped(event->argv[arg], strlen(event->argv[arg]));
            fputc('\n', stdout);
        }
    }
}

static int events_equal(const app_t *app, const expected_event_t *expected, size_t expected_count)
{
    size_t i;

    if (app->event_count != expected_count)
        return 0;
    for (i = 0; i < expected_count; ++i) {
        int arg;

        if (strcmp(app->events[i].command, expected[i].command) != 0 || app->events[i].op != expected[i].op || app->events[i].argc != expected[i].argc)
            return 0;
        for (arg = 0; arg < expected[i].argc; ++arg) {
            if (strcmp(app->events[i].argv[arg], expected[i].argv[arg]) != 0)
                return 0;
        }
    }
    return 1;
}

static int review_at_case(fixture_t *fixture, const char *title, const char *stream, size_t chunk_size, int expected_status, const char *expected_output, const expected_event_t *expected_events, size_t expected_event_count)
{
    static int case_number;
    size_t stream_len = strlen(stream);
    size_t expected_len = strlen(expected_output);
    size_t offset = 0;
    int actual_status = ZAT_OK;
    int parse_ok;
    int output_ok;
    int status_ok;

    clear_output(fixture);
    fixture->app.handler_calls = 0;
    fixture->app.event_count = 0;
    fixture->app.last_argc = 0;
    if (chunk_size == 0)
        chunk_size = stream_len;

    printf("\n  ------------------------------------------------------------------------\n");
    printf("  CASE %02d: %s\n", ++case_number, title);
    puts("  1. DATA STREAM");
    fputs("     full stream = ", stdout);
    print_escaped(stream, stream_len);
    fputc('\n', stdout);
    printf("     delivery = chunks up to %zu byte(s)\n", chunk_size);
    while (offset < stream_len) {
        size_t len = stream_len - offset < chunk_size ? stream_len - offset : chunk_size;
        int result;

        printf("     chunk[%zu] = ", offset / chunk_size);
        print_escaped(stream + offset, len);
        result = zat_feed(&fixture->zat, stream + offset, len);
        printf(", zat_feed => %d\n", result);
        if (actual_status == ZAT_OK && result != ZAT_OK)
            actual_status = result;
        offset += len;
    }

    puts("  2. SPLIT AT COMMANDS AND ARGUMENT PARSING");
    print_instruction_lines(stream, stream_len);
    puts("     expected parser result:");
    print_expected_events(expected_events, expected_event_count);
    puts("     actual handler result:");
    print_actual_events(&fixture->app);

    puts("  3. EXPECTED OUTPUT");
    fputs("     ", stdout);
    print_escaped(expected_output, expected_len);
    printf("\n     aggregate zat_feed status = %d\n", expected_status);

    puts("  4. ACTUAL OUTPUT");
    fputs("     ", stdout);
    print_escaped(fixture->app.output, fixture->app.output_len);
    printf("\n     aggregate zat_feed status = %d\n", actual_status);

    parse_ok = events_equal(&fixture->app, expected_events, expected_event_count);
    output_ok = fixture->app.output_len == expected_len && memcmp(fixture->app.output, expected_output, expected_len) == 0;
    status_ok = actual_status == expected_status;
    puts("  5. VERDICT");
    printf("     instruction/argument parsing: %s\n", parse_ok ? "PASS" : "FAIL");
    printf("     protocol output:             %s\n", output_ok ? "PASS" : "FAIL");
    printf("     feed return status:          %s\n", status_ok ? "PASS" : "FAIL");
    printf("     CASE RESULT:                 %s\n", parse_ok && output_ok && status_ok ? "PASS" : "FAIL");
    return parse_ok && output_ok && status_ok;
}

static int test_human_review_matrix(void)
{
    static const expected_event_t exec_event[] = {
        { "AAAAAA", ZAT_EXEC, 0, { NULL } },
    };
    static const expected_event_t read_event[] = {
        { "AAAAAA", ZAT_GET, 0, { NULL } },
    };
    static const expected_event_t test_event[] = {
        { "AAAAAA", ZAT_CAPS, 0, { NULL } },
    };
    static const expected_event_t set_event[] = {
        { "AAAAAA", ZAT_SET, 3, { "1", "2", "3" } },
    };
    static const expected_event_t quoted_event[] = {
        { "AAAAAA", ZAT_SET, 2, { "hello,world", "123" } },
    };
    static const expected_event_t max_arg_event[] = {
        { "AAAAAA", ZAT_SET, 8, { "0", "-1", "4294967295", "hello,world", " spaced value ", "", "ABCDEF", "last,param" } },
    };
    static const expected_event_t batch_events[] = {
        { "AAAAAA", ZAT_SET, 2, { "1", "two" } },
        { "AAAAAA", ZAT_GET, 0, { NULL } },
    };
    static const expected_event_t parse_event[] = {
        { "PARSE", ZAT_SET, 4, { "-123", "42", "TrUe", "a 0b c d 0xFFA" } },
    };
    static const expected_event_t parse_range_error_event[] = {
        { "PARSE", ZAT_SET, 4, { "-1001", "42", "true", "01" } },
    };
    static const expected_event_t line_ending_events[] = {
        { "AAAAAA", ZAT_GET, 0, { NULL } },
        { "AAAAAA", ZAT_CAPS, 0, { NULL } },
    };
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(review_at_case(&fixture, "basic AT", "AT\r\n", 0, ZAT_OK, "OK\r\n", NULL, 0));
    CHECK(review_at_case(&fixture, "mixed case with spaces: AT + AAAaaa", "AT + AAAaaa\r\n", 0, ZAT_OK, "OK\r\n", exec_event, ZAT_N(exec_event)));
    CHECK(review_at_case(&fixture, "GET operation with spaces", "at + aAaAaA ?\r\n", 0, ZAT_OK, "OK\r\n", read_event, ZAT_N(read_event)));
    CHECK(review_at_case(&fixture, "CAPS operation", "AT+AAAAAA=?\r\n", 0, ZAT_OK, "OK\r\n", test_event, ZAT_N(test_event)));
    CHECK(review_at_case(&fixture, "SET with three scalar arguments", "AT+AAAAAA=1,2,3\r\n", 0, ZAT_OK, "OK\r\n", set_event, ZAT_N(set_event)));
    CHECK(review_at_case(&fixture, "quoted comma and block fragmentation", "AT + AAAaaa = \"hello,world\", 123\r\n", 5, ZAT_OK, "OK\r\n", quoted_event, ZAT_N(quoted_event)));
    CHECK(review_at_case(&fixture, "maximum configured argument count with mixed values", "AT + AAAaaa = 0,-1,4294967295,\"hello,world\",\" spaced value \",,ABCDEF,\"last,param\"\r\n", 11, ZAT_OK, "OK\r\n", max_arg_event, ZAT_N(max_arg_event)));
    CHECK(review_at_case(&fixture, "multiple instructions in one fragmented stream", "AT\r\nAT + AAAaaa=1,two\r\nAT+UNKNOWN\r\nAT+AAAAAA?\r\n", 7, ZAT_OK, "OK\r\nOK\r\nERROR\r\nOK\r\n", batch_events, ZAT_N(batch_events)));
    CHECK(review_at_case(&fixture, "unknown command", "AT + UNKNOWN\r\n", 0, ZAT_OK, "ERROR\r\n", NULL, 0));
    CHECK(review_at_case(&fixture, "malformed quoted argument", "AT+AAAAAA=\"unterminated\r\n", 0, ZAT_OK, "ERROR\r\n", NULL, 0));
    CHECK(review_at_case(&fixture, "typed parsing with mixed hex fields", "AT+PARSE=-123,42,TrUe,\"a 0b c d 0xFFA\"\r\n", 8, ZAT_OK, "+PARSE:-123,42,1,0A0B0C0D0FFA\r\nOK\r\n", parse_event, ZAT_N(parse_event)));
    CHECK(review_at_case(&fixture, "typed parsing rejects an out-of-range integer", "AT+PARSE=-1001,42,true,01\r\n", 0, ZAT_OK, "ERROR\r\n", parse_range_error_event, ZAT_N(parse_range_error_event)));
    CHECK(review_at_case(&fixture, "CRLF, CR, and LF line terminators", "AT\r\nAT+AAAAAA?\rAT+AAAAAA=?\n", 3, ZAT_OK, "OK\r\nOK\r\nOK\r\n", line_ending_events, ZAT_N(line_ending_events)));
    return 1;
}

static int test_initialization(void)
{
    zat_t zat;
    char rx[8];
    app_t app;
    static const zat_cmd_t invalid_commands[] = { ZAT_CMD(NULL, cmd_record) };
    static const zat_fmt_t invalid_formats[] = { ZAT_FMT("bad", NULL) };

    memset(&app, 0, sizeof(app));
    CHECK(zat_init(NULL, rx, sizeof(rx), test_write, &app) == ZAT_EINVAL);
    CHECK(zat_init(&zat, NULL, sizeof(rx), test_write, &app) == ZAT_EINVAL);
    CHECK(zat_init(&zat, rx, 0, test_write, &app) == ZAT_EINVAL);
    CHECK(zat_init(&zat, rx, sizeof(rx), NULL, &app) == ZAT_EINVAL);
    CHECK(zat_init(&zat, rx, sizeof(rx), test_write, &app) == ZAT_OK);
    CHECK(zat.buf == rx && zat.cap == sizeof(rx) && zat.len == 0);
    CHECK(zat_arg(&zat) == &app);

    zat_cmds(&zat, invalid_commands, ZAT_N(invalid_commands));
    CHECK(zat.cmds == NULL && zat.ncmd == 0);
    zat_fmts(&zat, invalid_formats, ZAT_N(invalid_formats));
    CHECK(zat.fmts == NULL && zat.nfmt == 0);
    zat_cmds(&zat, NULL, 100);
    zat_fmts(&zat, NULL, 100);
    CHECK(zat.cmds == NULL && zat.ncmd == 0);
    CHECK(zat.fmts == NULL && zat.nfmt == 0);
    zat_cmds(NULL, commands, ZAT_N(commands));
    zat_fmts(NULL, formats, ZAT_N(formats));
    return 1;
}

static int test_basic_and_flexible_command_syntax(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(feed_text(&fixture, "AT\r\nat\n \tAt \rAT + AAAaaa\r\n") == ZAT_OK);
    CHECK(output_is(&fixture, "OK\r\nOK\r\nOK\r\nOK\r\n"));
    CHECK(fixture.app.handler_calls == 1);
    CHECK(fixture.app.last_op == ZAT_EXEC && fixture.app.last_argc == 0);
    return 1;
}

static int test_command_operations(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(feed_text(&fixture, "aT + aaaAAA\r\n") == ZAT_OK);
    CHECK(fixture.app.last_op == ZAT_EXEC && fixture.app.last_argc == 0);

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT+AAAAAA ?\r\n") == ZAT_OK);
    CHECK(fixture.app.last_op == ZAT_GET && fixture.app.last_argc == 0);

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT + Aaaaaa = ?\r\n") == ZAT_OK);
    CHECK(fixture.app.last_op == ZAT_CAPS && fixture.app.last_argc == 0);

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT+aaaaaa=1,2,3\r\n") == ZAT_OK);
    CHECK(fixture.app.last_op == ZAT_SET && fixture.app.last_argc == 3);
    CHECK(strcmp(fixture.app.last_argv[0], "1") == 0);
    CHECK(strcmp(fixture.app.last_argv[1], "2") == 0);
    CHECK(strcmp(fixture.app.last_argv[2], "3") == 0);

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT + AAAaaa = \"hello,world\", 123\r\n") == ZAT_OK);
    CHECK(fixture.app.last_op == ZAT_SET && fixture.app.last_argc == 2);
    CHECK(strcmp(fixture.app.last_argv[0], "hello,world") == 0);
    CHECK(strcmp(fixture.app.last_argv[1], "123") == 0);

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT+AAAAAA=\r\n") == ZAT_OK);
    CHECK(fixture.app.last_op == ZAT_SET && fixture.app.last_argc == 0);

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT+AAAAAA=1,\r\n") == ZAT_OK);
    CHECK(fixture.app.last_argc == 2);
    CHECK(strcmp(fixture.app.last_argv[0], "1") == 0 && strcmp(fixture.app.last_argv[1], "") == 0);
    return 1;
}

static int test_set_argument_edges(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(feed_text(&fixture, "AT+AAAAAA=  alpha , beta gamma  , \" x,y \" , \"\",,last\r\n") == ZAT_OK);
    CHECK(fixture.app.last_argc == 6);
    CHECK(strcmp(fixture.app.last_argv[0], "alpha") == 0);
    CHECK(strcmp(fixture.app.last_argv[1], "beta gamma") == 0);
    CHECK(strcmp(fixture.app.last_argv[2], " x,y ") == 0);
    CHECK(strcmp(fixture.app.last_argv[3], "") == 0);
    CHECK(strcmp(fixture.app.last_argv[4], "") == 0);
    CHECK(strcmp(fixture.app.last_argv[5], "last") == 0);

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT+AAAAAA=1,2,3,4,5,6,7,8\r\n") == ZAT_OK);
    CHECK(fixture.app.last_argc == ZAT_ARG_MAX);

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT+AAAAAA=1,2,3,4,5,6,7,8,9\r\n") == ZAT_OK);
    CHECK(output_is(&fixture, "ERROR\r\n"));
    return 1;
}

static int test_invalid_commands(void)
{
    static const char *const lines[] = {
        "A\r\n",
        "ATT\r\n",
        "AT-AAAAAA\r\n",
        "AT+\r\n",
        "AT + UNKNOWN\r\n",
        "AT+AAAAAA?tail\r\n",
        "AT+AAAAAA extra\r\n",
        "AT+AAAAAA=\"unterminated\r\n",
        "AT+AAAAAA=ab\"cd\r\n",
        "AT+AAAAAA=\"ok\"x\r\n",
    };
    fixture_t fixture;
    size_t i;

    CHECK(fixture_init(&fixture));
    for (i = 0; i < ZAT_N(lines); ++i) {
        clear_output(&fixture);
        CHECK(feed_text(&fixture, lines[i]) == ZAT_OK);
        CHECK(output_is(&fixture, "ERROR\r\n"));
    }
    return 1;
}

static int test_fragmented_and_batched_feed(void)
{
    const char fragmented[] = "AT+AAAAAA=\"hello,world\",123\r\n";
    fixture_t fixture;
    size_t i;

    CHECK(fixture_init(&fixture));
    for (i = 0; i < sizeof(fragmented) - 1; ++i)
        CHECK(zat_feed(&fixture.zat, fragmented + i, 1) == ZAT_OK);
    CHECK(output_is(&fixture, "OK\r\n"));
    CHECK(fixture.app.last_argc == 2);
    CHECK(strcmp(fixture.app.last_argv[0], "hello,world") == 0);

    clear_output(&fixture);
    CHECK(zat_feed(&fixture.zat, "AT+AAA", 6) == ZAT_OK);
    CHECK(zat_feed(&fixture.zat, "AAA?\r", 5) == ZAT_OK);
    CHECK(zat_feed(&fixture.zat, "\n", 1) == ZAT_OK);
    CHECK(output_is(&fixture, "OK\r\n"));
    CHECK(fixture.app.last_op == ZAT_GET);

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT\r\nAT+AAAAAA?\n\nAT+DATA?\rAT+FAIL\r\n") == ZAT_OK);
    CHECK(output_is(&fixture, "OK\r\nOK\r\n+DATA:42\r\nOK\r\nERROR\r\n"));
    return 1;
}

static int test_line_endings(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(feed_text(&fixture, "AT\r\nAT\rAT\n") == ZAT_OK);
    CHECK(output_is(&fixture, "OK\r\nOK\r\nOK\r\n"));

    clear_output(&fixture);
    CHECK(zat_feed(&fixture.zat, "AT", 2) == ZAT_OK);
    CHECK(zat_feed(&fixture.zat, "\r", 1) == ZAT_OK);
    CHECK(zat_feed(&fixture.zat, "\n", 1) == ZAT_OK);
    CHECK(output_is(&fixture, "OK\r\n"));
    return 1;
}

static int test_overflow_recovery(void)
{
    zat_t zat;
    app_t app;
    char small[8];

    memset(&app, 0, sizeof(app));
    CHECK(zat_init(&zat, small, sizeof(small), test_write, &app) == ZAT_OK);
    CHECK(zat_feed(&zat, "AT+AAAAAA\r\nAT\r\n", sizeof("AT+AAAAAA\r\nAT\r\n") - 1) == ZAT_ENOSPC);
    CHECK(app.output_len == strlen("ERROR\r\nOK\r\n"));
    CHECK(memcmp(app.output, "ERROR\r\nOK\r\n", app.output_len) == 0);

    app.output_len = 0;
    app.write_count = 0;
    CHECK(zat_feed(&zat, "12345678", 8) == ZAT_ENOSPC);
    CHECK(zat_feed(&zat, "discarded", 9) == ZAT_OK);
    CHECK(zat_feed(&zat, "\nAT\n", 4) == ZAT_OK);
    CHECK(app.output_len == strlen("ERROR\r\nOK\r\n"));
    CHECK(memcmp(app.output, "ERROR\r\nOK\r\n", app.output_len) == 0);
    return 1;
}

static int test_handler_results(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(feed_text(&fixture, "AT+DATA?\r\n") == ZAT_OK);
    CHECK(output_is(&fixture, "+DATA:42\r\nOK\r\n"));

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT+DATA\r\n") == ZAT_OK);
    CHECK(output_is(&fixture, "ERROR\r\n"));

    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT+FAIL\r\n") == ZAT_OK);
    CHECK(output_is(&fixture, "ERROR\r\n"));
    return 1;
}

static int test_raw_output_and_lines(void)
{
    static const unsigned char raw[] = { 'A', 0, 'B' };
    fixture_t fixture;
    size_t writes;

    CHECK(fixture_init(&fixture));
    CHECK(zat_write(&fixture.zat, raw, sizeof(raw)) == ZAT_OK);
    CHECK(fixture.app.output_len == sizeof(raw));
    CHECK(memcmp(fixture.app.output, raw, sizeof(raw)) == 0);
    writes = fixture.app.write_count;
    CHECK(zat_write(&fixture.zat, NULL, 0) == ZAT_OK);
    CHECK(fixture.app.write_count == writes);

    clear_output(&fixture);
    CHECK(zat_println(&fixture.zat, "plain") == ZAT_OK);
    CHECK(output_is(&fixture, "plain\r\n"));
    return 1;
}

static int test_printf_text(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(zat_printf(&fixture.zat, "%%|%c|%s|%.3s|%5s|%-5s", 'Z', "text", "abcdef", "x", "y") == ZAT_OK);
    CHECK(output_is(&fixture, "%|Z|text|abc|    x|y    "));

    clear_output(&fixture);
    CHECK(zat_printf(&fixture.zat, "%s", (const char *)NULL) == ZAT_OK);
    CHECK(output_is(&fixture, "(null)"));

    clear_output(&fixture);
    CHECK(zat_printf(&fixture.zat, "%c", 0) == ZAT_OK);
    CHECK(fixture.app.output_len == 1 && fixture.app.output[0] == 0);

    clear_output(&fixture);
    CHECK(call_vprintf(&fixture.zat, "vprintf:%s:%d", "ok", 7) == ZAT_OK);
    CHECK(output_is(&fixture, "vprintf:ok:7"));
    return 1;
}

static int test_printf_integers(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(zat_printf(&fixture.zat, "%d|%i|%u|%x|%X", INT_MIN, INT_MAX, UINT_MAX, UINT_MAX, UINT_MAX) == ZAT_OK);
    CHECK(output_is(&fixture, "-2147483648|2147483647|4294967295|ffffffff|FFFFFFFF"));

    clear_output(&fixture);
    CHECK(zat_printf(&fixture.zat, "%hhd|%hhu|%hd|%hu|%ld|%lu|%lld|%llu|%zd|%zu",
                     (int)(signed char)-12, (unsigned int)(unsigned char)250,
                     (int)(short)-1234, (unsigned int)(unsigned short)60000,
                     -1234567L, 3456789UL, -9223372036854775807LL - 1LL,
                     18446744073709551615ULL, (ptrdiff_t)-42, (size_t)42) == ZAT_OK);
    CHECK(output_is(&fixture, "-12|250|-1234|60000|-1234567|3456789|-9223372036854775808|18446744073709551615|-42|42"));

    clear_output(&fixture);
    CHECK(zat_printf(&fixture.zat, "%02hhX|%04hX|%08lX|%016llX|%zx",
                     (unsigned int)0xab, (unsigned int)0xabcd, 0x1234UL,
                     0x123456789abcdef0ULL, (size_t)0x1234) == ZAT_OK);
    CHECK(output_is(&fixture, "AB|ABCD|00001234|123456789ABCDEF0|1234"));
    return 1;
}

static int test_printf_width_and_precision(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(zat_printf(&fixture.zat, "[%5d][%05d][%05d][%-5d][%08X]", 12, 12, -12, -12, 0x12U) == ZAT_OK);
    CHECK(output_is(&fixture, "[   12][00012][-0012][-12  ][00000012]"));

    clear_output(&fixture);
    CHECK(zat_printf(&fixture.zat, "[%.5d][%8.5d][%.0d][%5.0d][%8.3s]", 12, 12, 0, 0, "abcdef") == ZAT_OK);
    CHECK(output_is(&fixture, "[00012][   00012][][     ][     abc]"));

    clear_output(&fixture);
    CHECK(zat_printf(&fixture.zat, "%p|%08p", (void *)0, (void *)0) == ZAT_OK);
    CHECK(output_is(&fixture, "0x0|0x000000"));
    return 1;
}

static int test_parse_integers(void)
{
    int32_t signed_value = 777;
    uint32_t unsigned_value = 777;

    CHECK(zat_parse_i32("0", INT32_MIN, INT32_MAX, &signed_value) == ZAT_OK && signed_value == 0);
    CHECK(zat_parse_i32("+42", INT32_MIN, INT32_MAX, &signed_value) == ZAT_OK && signed_value == 42);
    CHECK(zat_parse_i32("-42", INT32_MIN, INT32_MAX, &signed_value) == ZAT_OK && signed_value == -42);
    CHECK(zat_parse_i32("2147483647", INT32_MIN, INT32_MAX, &signed_value) == ZAT_OK && signed_value == INT32_MAX);
    CHECK(zat_parse_i32("-2147483648", INT32_MIN, INT32_MAX, &signed_value) == ZAT_OK && signed_value == INT32_MIN);
    CHECK(zat_parse_i32("25", 10, 30, &signed_value) == ZAT_OK && signed_value == 25);

    signed_value = 777;
    CHECK(zat_parse_i32("9", 10, 30, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32("31", 10, 30, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32("2147483648", INT32_MIN, INT32_MAX, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32("-2147483649", INT32_MIN, INT32_MAX, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32("", INT32_MIN, INT32_MAX, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32("-", INT32_MIN, INT32_MAX, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32("+", INT32_MIN, INT32_MAX, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32(" 1", INT32_MIN, INT32_MAX, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32("1 ", INT32_MIN, INT32_MAX, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32("1.0", INT32_MIN, INT32_MAX, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32(NULL, INT32_MIN, INT32_MAX, &signed_value) == ZAT_EINVAL && signed_value == 777);
    CHECK(zat_parse_i32("1", INT32_MIN, INT32_MAX, NULL) == ZAT_EINVAL);

    CHECK(zat_parse_u32("0", 0, UINT32_MAX, &unsigned_value) == ZAT_OK && unsigned_value == 0);
    CHECK(zat_parse_u32("4294967295", 0, UINT32_MAX, &unsigned_value) == ZAT_OK && unsigned_value == UINT32_MAX);
    CHECK(zat_parse_u32("65535", 1, 65535, &unsigned_value) == ZAT_OK && unsigned_value == 65535);

    unsigned_value = 777;
    CHECK(zat_parse_u32("0", 1, 65535, &unsigned_value) == ZAT_EINVAL && unsigned_value == 777);
    CHECK(zat_parse_u32("65536", 1, 65535, &unsigned_value) == ZAT_EINVAL && unsigned_value == 777);
    CHECK(zat_parse_u32("4294967296", 0, UINT32_MAX, &unsigned_value) == ZAT_EINVAL && unsigned_value == 777);
    CHECK(zat_parse_u32("-1", 0, UINT32_MAX, &unsigned_value) == ZAT_EINVAL && unsigned_value == 777);
    CHECK(zat_parse_u32("+1", 0, UINT32_MAX, &unsigned_value) == ZAT_EINVAL && unsigned_value == 777);
    CHECK(zat_parse_u32("0x10", 0, UINT32_MAX, &unsigned_value) == ZAT_EINVAL && unsigned_value == 777);
    CHECK(zat_parse_u32("", 0, UINT32_MAX, &unsigned_value) == ZAT_EINVAL && unsigned_value == 777);
    CHECK(zat_parse_u32(NULL, 0, UINT32_MAX, &unsigned_value) == ZAT_EINVAL && unsigned_value == 777);
    CHECK(zat_parse_u32("1", 0, UINT32_MAX, NULL) == ZAT_EINVAL);
    return 1;
}

static int test_parse_bool_values(void)
{
    static const char *const false_values[] = { "0", "false", "FALSE", "FaLsE", "off", "OFF", "disable", "DISABLE" };
    static const char *const true_values[] = { "1", "true", "TRUE", "TrUe", "on", "ON", "enable", "ENABLE" };
    static const char *const invalid_values[] = { "", "2", "yes", "no", "enabled", "disabled", " true", "true " };
    int value;
    size_t i;

    for (i = 0; i < ZAT_N(false_values); ++i) {
        value = 7;
        CHECK(zat_parse_bool(false_values[i], &value) == ZAT_OK && value == 0);
    }
    for (i = 0; i < ZAT_N(true_values); ++i) {
        value = 7;
        CHECK(zat_parse_bool(true_values[i], &value) == ZAT_OK && value == 1);
    }
    for (i = 0; i < ZAT_N(invalid_values); ++i) {
        value = 7;
        CHECK(zat_parse_bool(invalid_values[i], &value) == ZAT_EINVAL && value == 7);
    }
    value = 7;
    CHECK(zat_parse_bool(NULL, &value) == ZAT_EINVAL && value == 7);
    CHECK(zat_parse_bool("true", NULL) == ZAT_EINVAL);
    return 1;
}

static int bytes_are(const unsigned char *actual, size_t actual_len, const unsigned char *expected, size_t expected_len)
{
    return actual_len == expected_len && memcmp(actual, expected, expected_len) == 0;
}

static int test_parse_hex_values(void)
{
    static const unsigned char continuous[] = { 0x01, 0x02, 0x03 };
    static const unsigned char mixed[] = { 0x0a, 0x0b, 0x0c, 0x0d, 0x0f, 0xfa };
    static const unsigned char fields[] = { 0x01, 0x02, 0x0a, 0xbc };
    static const unsigned char boundaries_a[] = { 0xab };
    static const unsigned char boundaries_b[] = { 0x0a, 0x0b };
    unsigned char output[32];
    size_t length;

    CHECK(zat_parse_hex("010203", output, sizeof(output), &length) == ZAT_OK);
    CHECK(bytes_are(output, length, continuous, sizeof(continuous)));
    CHECK(zat_parse_hex("0x010203", output, sizeof(output), &length) == ZAT_OK);
    CHECK(bytes_are(output, length, continuous, sizeof(continuous)));
    CHECK(zat_parse_hex("a 0b c d 0xFFA", output, sizeof(output), &length) == ZAT_OK);
    CHECK(bytes_are(output, length, mixed, sizeof(mixed)));
    CHECK(zat_parse_hex("  0x1   0X02\tabc  ", output, sizeof(output), &length) == ZAT_OK);
    CHECK(bytes_are(output, length, fields, sizeof(fields)));
    CHECK(zat_parse_hex("AB", output, sizeof(output), &length) == ZAT_OK);
    CHECK(bytes_are(output, length, boundaries_a, sizeof(boundaries_a)));
    CHECK(zat_parse_hex("A B", output, sizeof(output), &length) == ZAT_OK);
    CHECK(bytes_are(output, length, boundaries_b, sizeof(boundaries_b)));
    return 1;
}

static int test_parse_hex_errors(void)
{
    static const char *const invalid_values[] = { "", "   ", "0x", "0X", "12GG", "0x12x3", "-1", "+12", "12:34", "12,34" };
    unsigned char output[8];
    unsigned char original[sizeof(output)];
    size_t length;
    size_t i;

    memset(original, 0xa5, sizeof(original));
    for (i = 0; i < ZAT_N(invalid_values); ++i) {
        memcpy(output, original, sizeof(output));
        length = 99;
        CHECK(zat_parse_hex(invalid_values[i], output, sizeof(output), &length) == ZAT_EINVAL);
        CHECK(memcmp(output, original, sizeof(output)) == 0 && length == 99);
    }

    memcpy(output, original, sizeof(output));
    length = 99;
    CHECK(zat_parse_hex("010203", output, 2, &length) == ZAT_ENOSPC);
    CHECK(memcmp(output, original, sizeof(output)) == 0 && length == 99);
    CHECK(zat_parse_hex(NULL, output, sizeof(output), &length) == ZAT_EINVAL);
    CHECK(zat_parse_hex("01", NULL, sizeof(output), &length) == ZAT_EINVAL);
    CHECK(zat_parse_hex("01", output, sizeof(output), NULL) == ZAT_EINVAL);
    return 1;
}

static int test_custom_formatters(void)
{
    static const unsigned char bytes[] = { 0x12, 0xab };
    static const unsigned char ip[] = { 192, 168, 1, 100 };
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    CHECK(zat_println(&fixture.zat, "%{BYTES}|%{bytes:raw}|%{bytes:}|%{IPv4}", (const void *)bytes, (const void *)bytes, (const void *)bytes, (const void *)ip) == ZAT_OK);
    CHECK(output_is(&fixture, "12:AB|12AB|empty|192.168.1.100\r\n"));

    clear_output(&fixture);
    CHECK(zat_printf(&fixture.zat, "%{bytes:bad}", (const void *)bytes) == ZAT_EFMT);
    CHECK(fixture.app.output_len == 0);
    return 1;
}

static int test_formatter_errors(void)
{
    static const unsigned char bytes[] = { 1, 2 };
    static const char *const formats_to_reject[] = {
        "%{missing}",
        "%{}",
        "%{bytes",
        "%f",
        "%q",
        "%",
        "%ls",
        "%.2c",
        "%lp",
        "%999999999999999999999999999999s",
    };
    fixture_t fixture;
    size_t i;

    CHECK(fixture_init(&fixture));
    for (i = 0; i < ZAT_N(formats_to_reject); ++i) {
        clear_output(&fixture);
        CHECK(zat_printf(&fixture.zat, formats_to_reject[i], bytes) == ZAT_EFMT);
    }
    return 1;
}

static int test_write_errors_and_recovery(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture));
    fixture.app.fail_on_write = 1;
    CHECK(zat_write(&fixture.zat, "x", 1) == ZAT_EIO);

    clear_output(&fixture);
    fixture.app.fail_on_write = 1;
    CHECK(zat_printf(&fixture.zat, "abc") == ZAT_EIO);

    clear_output(&fixture);
    fixture.app.fail_on_write = 1;
    CHECK(feed_text(&fixture, "AT\r\n") == ZAT_EIO);
    clear_output(&fixture);
    CHECK(feed_text(&fixture, "AT\r\n") == ZAT_OK);
    CHECK(output_is(&fixture, "OK\r\n"));

    clear_output(&fixture);
    fixture.app.fail_on_write = 2;
    CHECK(zat_printf(&fixture.zat, "%05d", 1) == ZAT_EIO);
    CHECK(output_is(&fixture, "0000"));
    return 1;
}

static int test_multiple_instances(void)
{
    fixture_t first;
    fixture_t second;

    CHECK(fixture_init(&first));
    CHECK(fixture_init(&second));
    CHECK(feed_text(&first, "AT+AAAAAA=first\r\n") == ZAT_OK);
    CHECK(feed_text(&second, "AT+AAAAAA=second\r\n") == ZAT_OK);
    CHECK(strcmp(first.app.last_argv[0], "first") == 0);
    CHECK(strcmp(second.app.last_argv[0], "second") == 0);
    CHECK(output_is(&first, "OK\r\n"));
    CHECK(output_is(&second, "OK\r\n"));
    return 1;
}

static void run_test(const char *name, test_fn fn)
{
    printf("\n[TEST] %s\n", name);
    if (fn()) {
        printf("[PASS] %s\n", name);
    } else {
        ++failures;
        printf("[FAIL] %s\n", name);
    }
}

int main(void)
{
    printf("ZAT C99 test suite\n");
    printf("sizeof(zat_t)=%zu, ZAT_ARG_MAX=%d\n", sizeof(zat_t), ZAT_ARG_MAX);
    printf("human-review protocol matrix followed by compact regression results\n");

    run_test("initialization normalization", test_initialization);
    run_test("human-review AT protocol matrix", test_human_review_matrix);
    run_test("basic and flexible command syntax", test_basic_and_flexible_command_syntax);
    run_test("command operations", test_command_operations);
    run_test("SET argument edges", test_set_argument_edges);
    run_test("invalid commands", test_invalid_commands);
    run_test("fragmented and batched feed", test_fragmented_and_batched_feed);
    run_test("CRLF, CR, and LF line endings", test_line_endings);
    run_test("overflow recovery", test_overflow_recovery);
    run_test("handler results", test_handler_results);
    run_test("raw output and lines", test_raw_output_and_lines);
    run_test("printf text", test_printf_text);
    run_test("printf integers", test_printf_integers);
    run_test("printf width and precision", test_printf_width_and_precision);
    run_test("parse signed and unsigned integers", test_parse_integers);
    run_test("parse bool values", test_parse_bool_values);
    run_test("parse mixed hex values", test_parse_hex_values);
    run_test("parse hex errors atomically", test_parse_hex_errors);
    run_test("custom formatters", test_custom_formatters);
    run_test("formatter errors", test_formatter_errors);
    run_test("write errors and recovery", test_write_errors_and_recovery);
    run_test("multiple instances", test_multiple_instances);

    printf("SUMMARY: %d checks, %d test groups failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
