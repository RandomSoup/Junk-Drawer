#if !defined(CONSOLE_H)
#define CONSOLE_H

#include "duktape.h"

void duk_console_init(duk_context *);

void duk__console_reg_vararg_func(duk_context *, duk_c_function, const char *);

duk_ret_t duk__console_log(duk_context *);

#endif
