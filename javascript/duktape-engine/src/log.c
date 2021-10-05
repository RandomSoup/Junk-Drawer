#include <stdio.h>
#include "duktape.h"

void print_error(duk_context *ctx) {
    fprintf(stderr, "%s\n", duk_safe_to_stacktrace(ctx, -1));
    fflush(stderr);
}
