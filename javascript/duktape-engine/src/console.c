#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "duktape.h"

/*
 *  Minimal 'console' binding.
 */

/* XXX: Add some form of log level filtering. */

/* XXX: Should all output be written via e.g. console.write(formattedMsg)?
 * This would make it easier for user code to redirect all console output
 * to a custom backend.
 */

/* XXX: Init console object using duk_def_prop() when that call is available. */

static duk_ret_t duk__console_log_helper(duk_context *ctx, const char *error_name) {
	duk_idx_t n = duk_get_top(ctx);

	duk_push_string(ctx, " ");
	duk_insert(ctx, 0);
	duk_join(ctx, n);

	if (error_name) {
		duk_push_error_object(ctx, DUK_ERR_ERROR, "%s", duk_require_string(ctx, -1));
		duk_push_string(ctx, "name");
		duk_push_string(ctx, error_name);
		duk_def_prop(ctx, -3, DUK_DEFPROP_FORCE | DUK_DEFPROP_HAVE_VALUE);  /* to get e.g. 'Trace: 1 2 3' */
		duk_get_prop_string(ctx, -1, "stack");
	}

	printf("%s\n", duk_to_string(ctx, -1));
	return 0;
}

static duk_ret_t duk__console_assert(duk_context *ctx) {
	if (duk_to_boolean(ctx, 0)) {
		return 0;
	}
	duk_remove(ctx, 0);

	return duk__console_log_helper(ctx, "AssertionError");
}

duk_ret_t duk__console_log(duk_context *ctx) {
	return duk__console_log_helper(ctx, NULL);
}

static duk_ret_t duk__console_trace(duk_context *ctx) {
	return duk__console_log_helper(ctx, "Trace");
}

static duk_ret_t duk__console_info(duk_context *ctx) {
	return duk__console_log_helper(ctx, NULL);
}

static duk_ret_t duk__console_warn(duk_context *ctx) {
	return duk__console_log_helper(ctx, NULL);
}

static duk_ret_t duk__console_error(duk_context *ctx) {
	return duk__console_log_helper(ctx, "Error");
}

static duk_ret_t duk__console_dir(duk_context *ctx) {
	/* For now, just share the formatting of .log() */
	return duk__console_log_helper(ctx, 0);
}

void duk__console_reg_vararg_func(duk_context *ctx, duk_c_function func, const char *name) {
	duk_push_c_function(ctx, func, DUK_VARARGS);
	duk_push_string(ctx, "name");
	duk_push_string(ctx, name);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_FORCE);  /* Improve stacktraces by displaying function name */
	duk_put_prop_string(ctx, -2, name);
}

void duk_console_init(duk_context *ctx) {
	duk_push_object(ctx);

	duk__console_reg_vararg_func(ctx, duk__console_assert, "assert");
	duk__console_reg_vararg_func(ctx, duk__console_log, "log");
	duk__console_reg_vararg_func(ctx, duk__console_log, "debug");
	duk__console_reg_vararg_func(ctx, duk__console_trace, "trace");
	duk__console_reg_vararg_func(ctx, duk__console_info, "info");

	duk__console_reg_vararg_func(ctx, duk__console_warn, "warn");
	duk__console_reg_vararg_func(ctx, duk__console_error, "error");
	duk__console_reg_vararg_func(ctx, duk__console_error, "exception");
	duk__console_reg_vararg_func(ctx, duk__console_dir, "dir");

    duk_put_global_string(ctx, "console");

    duk_get_global_string(ctx, "console");
    duk_def_prop(ctx, duk_get_top_index(ctx), DUK_DEFPROP_HAVE_ENUMERABLE);

    duk_get_global_string(ctx, "console");
    duk_push_string(ctx, "Console");
    duk_put_prop_string(ctx, duk_get_top_index(ctx) - 1, DUK_WELLKNOWN_SYMBOL("Symbol.toStringTag"));
}
