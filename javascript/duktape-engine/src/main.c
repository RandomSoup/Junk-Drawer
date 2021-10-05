#include "duktape.h"
#include "log.h"
#include "eventloop.h"
#include "window.h"
#include "console.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

#define LOG(x) printf("[logcat] %s\n", x);

/*
 * Run JS Code
 */

static void fatal_error(void *udata, const char *msg) {
    (void) udata;

    fprintf(stderr, "FATAL ERROR: %s\n", (msg ? msg : "Unknown"));
}

static void load(duk_context *ctx, char *data) {
    if (duk_pcompile_string(ctx, 0, data) == 0) {
        if (duk_pcall(ctx, 0) != 0) {
            print_error(ctx);
        }
    } else {
        print_error(ctx);
    }
    duk_pop(ctx);
}

void run_javascript(const char *nativeString, char *babel, char *polyfill) {
    LOG("Transpiling...");
    duk_context *ctx = duk_create_heap(NULL, NULL, NULL, NULL, fatal_error);

    load(ctx, polyfill);
    load(ctx, babel);

    char* result;

    duk_push_string(ctx, nativeString);
    (void) duk_put_global_string(ctx, "SRC");

    if (duk_pcompile_string(ctx, 0, "Babel.transform(SRC, {sourceType: 'script', presets: ['env'], retainLines: true}).code;") == 0) {
        if (duk_pcall(ctx, 0) != 0) {
            print_error(ctx);
            duk_pop(ctx);
            duk_destroy_heap(ctx);
            return;
        }
    } else {
        print_error(ctx);
        duk_pop(ctx);
        duk_destroy_heap(ctx);
        return;
    }

    result = strdup(duk_get_string(ctx, -1));
    duk_pop(ctx);

    duk_destroy_heap(ctx);

    LOG("Running...");
    ctx = duk_create_heap(NULL, NULL, NULL, NULL, fatal_error);

    load(ctx, polyfill);

    duk_console_init(ctx);
    duk_window_init(ctx);

    if (eventloop_register(ctx) == 0) {
        if (eventloop_run(ctx, result) != 0) {
            print_error(ctx);
        }
        duk_pop(ctx);
        eventloop_cleanup();
    } else {
        print_error(ctx);
    }
    duk_pop(ctx);

    duk_destroy_heap(ctx);

    free(result);

    LOG("Finalizing the Javascript interpreter");
}

char *read_file(char *name) {
    FILE *file = fopen(name, "r");

    int length = 80;
    char *input = malloc(length * sizeof(char));
    int count = 0;
    char c;
    while ((c = getc(file)) != (char) EOF) {
        if (count >= length) {
            input = realloc(input, (length += 1) * sizeof(char));
        }
        input[count++] = c;
    }
    if (count >= length) {
        input = realloc(input, (length += 1) * sizeof(char));
    }
    input[count++] = '\0';

    fclose(file);

    return input;
}

int main(int argc, char *argv[]) { // Data, Babel, Polyfill
    if (argc != 4) {
        fprintf(stderr, "USAGE: js <File Path> <Babel Path> <Polyfill Path>\n");
        exit(1);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    run_javascript(read_file(argv[1]), read_file(argv[2]), read_file(argv[3]));
    return 0;
}
