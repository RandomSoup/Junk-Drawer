#include <stdio.h>
#include <stdlib.h>
#include <readline/readline.h>
#include <quickjs.h>
#include "lang.h"

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int i;
    const char *str;

    for (i = 0; i < argc; i++) {
        if (i != 0)
            putchar(' ');
        str = JS_ToCString(ctx, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        fputs(str, stdout);
        JS_FreeCString(ctx, str);
    }
    putchar('\n');
    return JS_UNDEFINED;
}

static JSValue js_assert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int i;
    const char *str;

    if (argc < 1) {
        return JS_EXCEPTION;
    }
    int bool_val = JS_ToBool(ctx, argv[0]);
    if (bool_val == -1) {
        return JS_EXCEPTION;
    }

    if (!bool_val) {
        fputs("Assertion failed", stdout);
        for (i = 1; i < argc; i++) {
            if (i != 1) {
                putchar(' ');
            } else {
                fputs(": ", stdout);
            }
            str = JS_ToCString(ctx, argv[i]);
            if (!str)
                return JS_EXCEPTION;
            fputs(str, stdout);
            JS_FreeCString(ctx, str);
        }
        putchar('\n');
    }
    return JS_UNDEFINED;
}

static JSValue js_input(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *str;
    JSValue out;

    if (argc == 0) {
        out = JS_NewString(ctx, readline(str));
    } else if (argc == 1) {
        str = JS_ToCString(ctx, argv[0]);
        if (!str)
            return JS_EXCEPTION;

        out = JS_NewString(ctx, readline(str));
        JS_FreeCString(ctx, str);
    } else {
        return JS_EXCEPTION;
    }

    return out;
}

void js_std_dump_error(JSContext *ctx) {
    JSValue exception_val, val;
    const char *stack;
    int is_error;

    exception_val = JS_GetException(ctx);
    is_error = JS_IsError(ctx, exception_val);
    if (!is_error)
        printf("Throw: ");
    js_print(ctx, JS_NULL, 1, (JSValueConst *) &exception_val);
    if (is_error) {
        val = JS_GetPropertyStr(ctx, exception_val, "stack");
        if (!JS_IsUndefined(val)) {
            stack = JS_ToCString(ctx, val);
            printf("%s", stack);
            JS_FreeCString(ctx, stack);
        }
        JS_FreeValue(ctx, val);
    }
    JS_FreeValue(ctx, exception_val);
}

static const JSCFunctionListEntry js_console_funcs[] = {
    JS_CFUNC_DEF("log", 1, js_print),
    JS_CFUNC_DEF("info", 1, js_print),
    JS_CFUNC_DEF("warn", 1, js_print),
    JS_CFUNC_DEF("error", 1, js_print),
    JS_CFUNC_DEF("dir", 1, js_print),
    JS_CFUNC_DEF("debug", 1, js_print),
    JS_CFUNC_DEF("trace", 1, js_print),
    JS_CFUNC_DEF("assert", 1, js_assert),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", CONSOLE_TO_STRING_TAG, JS_PROP_CONFIGURABLE)
};

void js_console_init(JSContext *ctx) {
    JSValue global_obj, console, window;

    /* XXX: should these global definitions be enumerable? */
    global_obj = JS_GetGlobalObject(ctx);

    console = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, console, js_console_funcs, sizeof js_console_funcs / sizeof (JSCFunctionListEntry));
    JS_SetPropertyStr(ctx, global_obj, "console", console);

    JS_SetPropertyStr(ctx, global_obj, "alert", JS_NewCFunction(ctx, js_print, "alert", 1));
    JS_SetPropertyStr(ctx, global_obj, "prompt", JS_NewCFunction(ctx, js_input, "prompt", 1));

    JS_SetPropertyStr(ctx, global_obj, "window", JS_DupValue(ctx, global_obj));

    JS_FreeValue(ctx, global_obj);
}
