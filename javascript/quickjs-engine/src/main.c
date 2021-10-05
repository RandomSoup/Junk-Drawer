#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <quickjs.h>
#include <cutils.h>
#include <assert.h>
#include <limits.h>
#include "console.h"
#include "eventloop.h"

uint8_t *js_load_file(JSContext *ctx, size_t *pbuf_len, const char *filename) {
    FILE *f;
    uint8_t *buf;
    size_t buf_len;

    f = fopen(filename, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    buf_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (ctx) {
        buf = js_malloc(ctx, buf_len + 1);
    } else {
        buf = malloc(buf_len + 1);
    }
    fread(buf, 1, buf_len, f);
    buf[buf_len] = '\0';
    fclose(f);
    *pbuf_len = buf_len;
    return buf;
}

static int eval_file(JSContext *ctx, const char *filename, const char *display_filename) {
    uint8_t *buf;
    int ret;
    size_t buf_len;

    buf = js_load_file(ctx, &buf_len, filename);
    if (!buf) {
        perror(filename);
        exit(1);
    }
    ret = eventloop_run(ctx, buf, buf_len, display_filename);
    js_free(ctx, buf);
    return ret;
}

int js_module_set_import_meta(JSContext *ctx, JSValueConst func_val, int use_realpath, int is_main) {
    JSModuleDef *m;
    char buf[PATH_MAX + 16], *res;
    JSValue meta_obj;
    JSAtom module_name_atom;
    const char *module_name;

    assert(JS_VALUE_GET_TAG(func_val) == JS_TAG_MODULE);
    m = JS_VALUE_GET_PTR(func_val);

    module_name_atom = JS_GetModuleName(ctx, m);
    module_name = JS_AtomToCString(ctx, module_name_atom);
    JS_FreeAtom(ctx, module_name_atom);
    if (!module_name)
        return -1;
    if (!strchr(module_name, ':')) {
        strcpy(buf, "file://");
        /* realpath() cannot be used with modules compiled with qjsc
           because the corresponding module source code is not
           necessarily present */
        if (use_realpath) {
            res = realpath(module_name, buf + strlen(buf));
            if (!res) {
                JS_ThrowTypeError(ctx, "realpath failure");
                JS_FreeCString(ctx, module_name);
                return -1;
            }
        } else {
            pstrcat(buf, sizeof(buf), module_name);
        }
    } else {
        pstrcpy(buf, sizeof(buf), module_name);
    }
    JS_FreeCString(ctx, module_name);

    meta_obj = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta_obj)) {
        return -1;
    }
    JS_DefinePropertyValueStr(ctx, meta_obj, "url", JS_NewString(ctx, buf), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta_obj, "main", JS_NewBool(ctx, is_main), JS_PROP_C_W_E);
    JS_FreeValue(ctx, meta_obj);
    return 0;
}

JSModuleDef *js_module_loader(JSContext *ctx, const char *module_name, void *opaque) {
    JSModuleDef *m;

    size_t buf_len;
    uint8_t *buf;
    JSValue func_val;

    buf = js_load_file(ctx, &buf_len, module_name);
    if (!buf) {
        JS_ThrowReferenceError(ctx, "could not load module filename '%s'", module_name);
        return NULL;
    }

    /* compile the module */
    func_val = JS_Eval(ctx, (char *) buf, buf_len, module_name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    js_free(ctx, buf);
    if (JS_IsException(func_val)) {
        return NULL;
    }
    /* XXX: could propagate the exception */
    if (js_module_set_import_meta(ctx, func_val, 1, 0) != 0) {
        JS_FreeValue(ctx, func_val);
        return NULL;
    }
    /* the module is already referenced, so we must free it */
    m = JS_VALUE_GET_PTR(func_val);
    JS_FreeValue(ctx, func_val);
    return m;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc != 3) {
        fprintf(stderr, "USAGE: js <File Path> <File Name>\n");
        exit(1);
    }

    JSRuntime *rt;
    JSContext *ctx;

    rt = JS_NewRuntime();
    if (!rt) {
        fprintf(stderr, "qjs: cannot allocate JS runtime\n");
        exit(1);
    }
    ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "qjs: cannot allocate JS context\n");
        exit(1);
    }
    JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);

    js_console_init(ctx);

    if (eventloop_register(ctx) == 0) {
        if (argc == 3) {
            eval_file(ctx, argv[1], argv[2]);
        }
    }
    eventloop_cleanup(ctx);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}
