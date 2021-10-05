#include <stdio.h>
#include <readline/readline.h>
#include "duktape.h"
#include "console.h"

/*
 * Minimal 'window' binding.
 */

static duk_ret_t duk__window_prompt(duk_context *ctx) {
	duk_push_string(ctx, readline(duk_require_string(ctx, 0)));
	return 1;
}

void duk_window_init(duk_context *ctx) {
    // Create Window Object
	duk_push_global_object(ctx);
    duk_put_global_string(ctx, "window");

    duk_get_global_string(ctx, "window");
    duk_def_prop(ctx, duk_get_top_index(ctx), DUK_DEFPROP_HAVE_ENUMERABLE);

	// Add To Global Object
	duk_push_global_object(ctx);

	duk__console_reg_vararg_func(ctx, duk__console_log, "alert");
	duk__console_reg_vararg_func(ctx, duk__console_log, "print");
    duk__console_reg_vararg_func(ctx, duk__window_prompt, "prompt");

    duk_pop(ctx);
}
