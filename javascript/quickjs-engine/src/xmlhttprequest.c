#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <string.h>
#include <quickjs.h>
#include "eventloop.h"
#include "console.h"
#include "lang.h"

#define OUT_OF_MEMORY "XMLHttpRequest: Out of Memory\n"

typedef struct thread_data {
    int id;
    char *data;
    char *status;
    int status_code;
} thread_data;

typedef struct url_data {
    int id;
    char *body;
    char *url;
    char *method;
    int headers_length;
    struct curl_slist *request_headers;
} url_data;

typedef struct xmlhttprequest_data {
    char *url;
    char *method;

    int ready_state;
    char *response;
    char *response_type;
    char *status;
    int status_code;

    JSValue event_listeners;

    struct curl_slist *request_headers;
} xmlhttprequest_data;

static thread_data **results;
static int curl_length = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t run_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t run_cond = PTHREAD_COND_INITIALIZER; 

static int running = 0;

static JSValue *xmlhttprequest_requests = NULL;
static int requests_length = 0;

static JSClassID JS_CLASS_XMLHTTPREQUEST_ID;

static int prop_is_callable(JSContext *ctx, JSValue obj, JSAtom prop) {
    int rc = 0;
    JSValue val;

    if (JS_HasProperty(ctx, obj, prop)) {
        val = JS_GetProperty(ctx, obj, prop);
        if (JS_IsFunction(ctx, val)) {
            rc = 1;
        }
        JS_FreeValue(ctx, val);
    }

    JS_FreeAtom(ctx, prop);
    return rc;
}

static JSValue build_event(JSContext *ctx, JSValue obj) {
    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, obj));
    return event;
}

static int call_method(JSContext *ctx, JSValue obj, JSValue prop) {
    int rc = 0;

    JSValue event = build_event(ctx, obj);
    JSValueConst args[1];
    args[0] = event;
    JSValue val = JS_Call(ctx, prop, obj, 1, args);
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        eventloop_request_exit();
        rc = 1;
    } else {
        JS_FreeValue(ctx, val);
    }
    JS_FreeValue(ctx, event);
    JS_FreeValue(ctx, prop);

    return rc;
}

static void call_event_listener(JSContext *ctx, int runOnload, JSValue obj) {
    if (prop_is_callable(ctx, obj, JS_NewAtom(ctx, "onreadystatechange"))) {
        if (call_method(ctx, obj, JS_GetPropertyStr(ctx, obj, "onreadystatechange"))) return;
    }
    xmlhttprequest_data *xml_data = JS_GetOpaque(obj, JS_CLASS_XMLHTTPREQUEST_ID);

    JSValue onreadystatechange = JS_GetPropertyStr(ctx, xml_data->event_listeners, "onreadystatechange");
    JSValue onload = JS_GetPropertyStr(ctx, xml_data->event_listeners, "onload");
    JSValue onerror = JS_GetPropertyStr(ctx, xml_data->event_listeners, "onerror");
    int64_t length;

    JSValue lenJS = JS_GetPropertyStr(ctx, onreadystatechange, "length");
    JS_ToInt64(ctx, &length, lenJS);
    JS_FreeValue(ctx, lenJS);

    for (int i = 0; i < length; i++) {
        if (prop_is_callable(ctx, onreadystatechange, JS_NewAtomUInt32(ctx, i))) {
            if (call_method(ctx, obj, JS_GetPropertyUint32(ctx, onreadystatechange, i))) goto cleanup;
        }
    }

    if (runOnload == 1) {
        if (prop_is_callable(ctx, obj, JS_NewAtom(ctx, "onload"))) {
            if (call_method(ctx, obj, JS_GetPropertyStr(ctx, obj, "onload"))) goto cleanup;
        }

        lenJS = JS_GetPropertyStr(ctx, onload, "length");
        JS_ToInt64(ctx, &length, lenJS);
        JS_FreeValue(ctx, lenJS);

        for (int i = 0; i < length; i++) {
            if (prop_is_callable(ctx, onload, JS_NewAtomUInt32(ctx, i))) {
                if (call_method(ctx, obj, JS_GetPropertyUint32(ctx, onload, i))) goto cleanup;
            }
        }
    } else if (runOnload == -1) {
        if (prop_is_callable(ctx, obj, JS_NewAtom(ctx, "onerror"))) {
            if (call_method(ctx, obj, JS_GetPropertyStr(ctx, obj, "onerror"))) goto cleanup;
        }

        lenJS = JS_GetPropertyStr(ctx, onerror, "length");
        JS_ToInt64(ctx, &length, lenJS);
        JS_FreeValue(ctx, lenJS);

        for (int i = 0; i < length; i++) {
            if (prop_is_callable(ctx, onerror, JS_NewAtomUInt32(ctx, i))) {
                if (call_method(ctx, obj, JS_GetPropertyUint32(ctx, onerror, i))) goto cleanup;
            }
        }
    }
cleanup:
    JS_FreeValue(ctx, onreadystatechange);
    JS_FreeValue(ctx, onload);
    JS_FreeValue(ctx, onerror);
}

static void on_done(JSContext *ctx, thread_data *result) {
    JSValue obj = xmlhttprequest_requests[result->id];
    xmlhttprequest_data *data = JS_GetOpaque(obj, JS_CLASS_XMLHTTPREQUEST_ID);

    data->ready_state = 4;
    data->response = strdup(result->data);
    data->status_code = result->status_code;
    data->status = strdup(result->status);
    data->response_type = "text";

    if (result->status_code == 200) {
        call_event_listener(ctx, 1, obj);
    } else {
        call_event_listener(ctx, -1, obj);
    }

    JS_FreeValue(ctx, obj);
    xmlhttprequest_requests[result->id] = JS_NULL;
    free(result);
}

typedef struct curl_data {
    char *memory;
    size_t size;
} curl_data;
 
static size_t curl_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    if (eventloop_is_exit_requested()) {
        return 0;
    }
    size_t realsize = size * nmemb;
    struct curl_data *mem = (struct curl_data *) userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        fprintf(stderr, OUT_OF_MEMORY);
        fflush(stderr);
        exit(1);
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

typedef struct header_data {
    char *status;
    int done;
} header_data;

static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    if (eventloop_is_exit_requested()) {
        return 0;
    }
    size_t numbytes = size * nitems;
    header_data *data = (header_data *) userdata;
    if (!data->done) {
        int length = 0;
        char last_char = buffer[length];
        while (buffer[length] != '\0' && buffer[length] != '\n' && length < nitems) {
            length++;
            last_char = buffer[length];
        }
        int add_eof = 0;
        if (last_char == '\n') {
            buffer[length - 1] = '\0';
        } else if (last_char != '\0') {
            add_eof = 1;
            length++;
        }
        char *data_str = malloc(length * sizeof (char));
        for (int i = 0; i < length; i++) {
            if (add_eof && i == length - 1) {
                data_str[i] = '\0';
            } else {
                data_str[i] = buffer[i];
            }
        }
        data->status = data_str;
        data->done = 1;
    }
    return numbytes;
}

static void *get_data(void *void_data) {
    url_data *data = (url_data *) void_data;
    thread_data *new_data = (thread_data *) malloc(sizeof (thread_data));
    new_data->id = data->id;

    CURL *curl_handle;
    CURLcode res;
    curl_data *chunk = malloc(sizeof (curl_data));
    chunk->memory = NULL;
    chunk->size = 0;
    curl_handle = curl_easy_init();
    curl_easy_setopt(curl_handle, CURLOPT_URL, data->url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, data->request_headers);
    header_data *statusText = malloc(sizeof (header_data));
    statusText->done = 0;
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *) statusText); 
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");
    if (strcmp(data->method, "GET") == 0) {
        curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1);
    } else if (strcmp(data->method, "POST") == 0) {
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, data->body);
    }
    res = curl_easy_perform(curl_handle);
    if (eventloop_is_exit_requested()) {
        goto fail;
    }
    if (res != CURLE_OK) {
        new_data->status = (char *) curl_easy_strerror(res);
        long response_code;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
        new_data->status_code = response_code;
        new_data->data = "";
        goto realloc;
    } else {
        long response_code;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
        new_data->status_code = response_code;
        new_data->status = statusText->status;
    }
    curl_easy_cleanup(curl_handle);

    char *out = realloc(chunk->memory, chunk->size + sizeof (char));
    if (!out) {
        fprintf(stderr, OUT_OF_MEMORY);
        fflush(stderr);
        exit(1);
    }
    out[chunk->size / sizeof (char)] = '\0';

    new_data->data = out;

realloc:
    free(data);
    free(chunk);
    free(statusText);

    pthread_mutex_lock(&lock);
    curl_length++;

    size_t new_size = curl_length * sizeof (thread_data);
    if (new_size > sizeof results) {
        thread_data **tmp = realloc(results, new_size);
        if (tmp) {
            results = tmp;
        } else {
            fprintf(stderr, OUT_OF_MEMORY);
            fflush(stderr);
            exit(1);
        }
    }
    results[curl_length - 1] = new_data;

    pthread_mutex_unlock(&lock);

    eventloop_interrupt_sleep();

    goto rc;

fail:
    curl_easy_cleanup(curl_handle);
    free(data);
    free(chunk);
    free(statusText);
    free(new_data);

    pthread_mutex_lock(&run_lock);
    // Is this the last thread?
    if (running == 1) {
        pthread_cond_signal(&run_cond);
    }
    pthread_mutex_unlock(&run_lock);

    eventloop_interrupt_sleep();

    goto rc;

rc:
    pthread_mutex_lock(&lock);
    running--;
    pthread_mutex_unlock(&lock);

    return NULL;
}

static JSValue xmlhttprequest_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc != 2) {
        return JS_EXCEPTION;
    }

    xmlhttprequest_data *xml_data = JS_GetOpaque(this_val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (!xml_data) {
        return JS_ThrowTypeError(ctx, NOT_XMLHTTPREQUEST_ERR);
    }

    if (xml_data->ready_state > 0) {
        return JS_ThrowTypeError(ctx, OPEN_CALLED_ERR);
    }
    if (xml_data->ready_state > 1 && xml_data->ready_state != 4) {
        return JS_ThrowTypeError(ctx, EXISTING_REQUEST_ERR);
    }

    const char *url = JS_ToCString(ctx, argv[1]);
    xml_data->url = strdup(url);
    JS_FreeCString(ctx, url);

    const char *method = JS_ToCString(ctx, argv[0]);
    xml_data->method = strdup(method);
    JS_FreeCString(ctx, method);

    xml_data->ready_state = 1;
    call_event_listener(ctx, 0, this_val);

    return JS_UNDEFINED;
}

static int get_id() {
    for (int i = 0; i < requests_length; i++) {
        if (JS_IsNull(xmlhttprequest_requests[i])) {
            return i;
        }
    }
    return requests_length;
}

static JSValue xmlhttprequest_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 0 || argc > 1) {
        return JS_EXCEPTION;
    }

    xmlhttprequest_data *xml_data = JS_GetOpaque(this_val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (!xml_data) {
        return JS_ThrowTypeError(ctx, NOT_XMLHTTPREQUEST_ERR);
    }

    if (xml_data->ready_state < 1) {
        return JS_ThrowTypeError(ctx, OPEN_NOT_CALLED_ERR);
    }
    if (xml_data->ready_state > 1) {
        return JS_ThrowTypeError(ctx, SEND_CALLED_ERR);
    }

    url_data *data = (url_data *) malloc(sizeof (url_data));
    data->id = get_id();

    xml_data->ready_state = 3;
    call_event_listener(ctx, 0, this_val);

    data->url = xml_data->url;
    data->method = xml_data->method;

    data->request_headers = xml_data->request_headers;

    if (argc == 1) {
        const char *body = JS_ToCString(ctx, argv[0]);
        data->body = strdup(body);
        JS_FreeCString(ctx, body);
    } else {
        data->body = "";
    }

    if (data->id + 1 > requests_length) {
        JSValue *ptr = js_realloc(ctx, xmlhttprequest_requests, (data->id + 1) * sizeof (JSValue));
        if (ptr == NULL) {
            return JS_EXCEPTION;
        }
        xmlhttprequest_requests = ptr;
        requests_length = data->id + 1;
    }
    xmlhttprequest_requests[data->id] = JS_DupValue(ctx, this_val);

    pthread_t thread;
    pthread_create(&thread, NULL, get_data, (void *) data);
    running++;

    return JS_UNDEFINED;
}

static JSValue xmlhttprequest_set_request_header(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc != 2) {
        return JS_EXCEPTION;
    }

    xmlhttprequest_data *data = JS_GetOpaque(this_val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (!data) {
        return JS_ThrowTypeError(ctx, NOT_XMLHTTPREQUEST_ERR);
    }

    if (data->ready_state < 1) {
        return JS_ThrowTypeError(ctx, OPEN_NOT_CALLED_ERR);
    }
    if (data->ready_state > 1) {
        return JS_ThrowTypeError(ctx, SEND_CALLED_ERR);
    }

    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);

    char *header = malloc((strlen(name) + 2 + strlen(value)) * sizeof (char));
    sprintf(header, "%s: %s", name, value);
    data->request_headers = curl_slist_append(data->request_headers, header);
    free(header);

    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);

    return JS_UNDEFINED;
}

static JSValue xmlhttprequest_addeventlistener(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc != 2) {
        return JS_EXCEPTION;
    }
    const char *event;
    JSValue func;
    JSValue rc = JS_UNDEFINED;

    xmlhttprequest_data *xml_data = JS_GetOpaque(this_val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (!xml_data) {
        return JS_ThrowTypeError(ctx, NOT_XMLHTTPREQUEST_ERR);
    }

    event = JS_ToCString(ctx, argv[0]);
    if (JS_IsFunction(ctx, argv[1])) {
        func = JS_DupValue(ctx, argv[1]);
    } else {
        rc = JS_ThrowTypeError(ctx, NOT_A_FUNCTION_ERR);
        goto free_event;
    }

    JSValue onreadystatechange = JS_GetPropertyStr(ctx, xml_data->event_listeners, "onreadystatechange");
    JSValue onload = JS_GetPropertyStr(ctx, xml_data->event_listeners, "onload");
    JSValue onerror = JS_GetPropertyStr(ctx, xml_data->event_listeners, "onerror");

    int64_t length;
    JSValue lenJS;
    if (strcmp(event, "readystatechange") == 0) {
        lenJS = JS_GetPropertyStr(ctx, onreadystatechange, "length");
        JS_ToInt64(ctx, &length, lenJS);

        JS_SetPropertyUint32(ctx, onreadystatechange, length, func);
    } else if (strcmp(event, "load") == 0) {
        lenJS = JS_GetPropertyStr(ctx, onload, "length");
        JS_ToInt64(ctx, &length, lenJS);

        JS_SetPropertyUint32(ctx, onload, length, func);
    } else if (strcmp(event, "error") == 0) {
        lenJS = JS_GetPropertyStr(ctx, onerror, "length");
        JS_ToInt64(ctx, &length, lenJS);

        JS_SetPropertyUint32(ctx, onerror, length, func);
    } else {
        rc = JS_ThrowTypeError(ctx, INVALID_EVENT_ERR);
        goto cleanup;
    }

    JS_FreeValue(ctx, lenJS);
cleanup:
    JS_FreeValue(ctx, onreadystatechange);
    JS_FreeValue(ctx, onload);
    JS_FreeValue(ctx, onerror);
free_event:
    JS_FreeCString(ctx, event);

    return rc;
}

static void xmlhttprequest_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    xmlhttprequest_data *data = JS_GetOpaque(val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (data) {
        JS_MarkValue(rt, data->event_listeners, mark_func);
    }
}

static void xmlhttprequest_finalizer(JSRuntime *rt, JSValue val) {
    xmlhttprequest_data *data = JS_GetOpaque(val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (data) {
        JS_FreeValueRT(rt, data->event_listeners);
        curl_slist_free_all(data->request_headers);
        js_free_rt(rt, data);
    }
}

static JSValue xmlhttprequest_ready_state(JSContext *ctx, JSValueConst this_val) {
    xmlhttprequest_data *data = JS_GetOpaque(this_val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (!data) {
        return JS_ThrowTypeError(ctx, NOT_XMLHTTPREQUEST_ERR);
    }
    return JS_NewInt64(ctx, data->ready_state);
}

static JSValue xmlhttprequest_response(JSContext *ctx, JSValueConst this_val) {
    xmlhttprequest_data *data = JS_GetOpaque(this_val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (!data) {
        return JS_ThrowTypeError(ctx, NOT_XMLHTTPREQUEST_ERR);
    }
    return JS_NewString(ctx, data->response);
}

static JSValue xmlhttprequest_response_type(JSContext *ctx, JSValueConst this_val) {
    xmlhttprequest_data *data = JS_GetOpaque(this_val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (!data) {
        return JS_ThrowTypeError(ctx, NOT_XMLHTTPREQUEST_ERR);
    }
    return JS_NewString(ctx, data->response_type);
}

static JSValue xmlhttprequest_status(JSContext *ctx, JSValueConst this_val) {
    xmlhttprequest_data *data = JS_GetOpaque(this_val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (!data) {
        return JS_ThrowTypeError(ctx, NOT_XMLHTTPREQUEST_ERR);
    }
    return JS_NewString(ctx, data->status);
}

static JSValue xmlhttprequest_status_code(JSContext *ctx, JSValueConst this_val) {
    xmlhttprequest_data *data = JS_GetOpaque(this_val, JS_CLASS_XMLHTTPREQUEST_ID);
    if (!data) {
        return JS_ThrowTypeError(ctx, NOT_XMLHTTPREQUEST_ERR);
    }
    return JS_NewInt64(ctx, data->status_code);
}

static JSClassDef JS_CLASS_XMLHTTPREQUEST = {
    XMLHTTPREQUEST_NAME,
    .finalizer = xmlhttprequest_finalizer,
    .gc_mark = xmlhttprequest_gc_mark
};

static const JSCFunctionListEntry xmlhttprequest_proto_funcs[] = {
    JS_CFUNC_DEF("open", 2, xmlhttprequest_open),
    JS_CFUNC_DEF("send", 1, xmlhttprequest_send),
    JS_CFUNC_DEF("setRequestHeader", 1, xmlhttprequest_set_request_header),
    JS_CFUNC_DEF("addEventListener", 2, xmlhttprequest_addeventlistener),

    JS_CGETSET_DEF("readyState", xmlhttprequest_ready_state, NULL),
    JS_CGETSET_DEF("response", xmlhttprequest_response, NULL),
    JS_CGETSET_DEF("responseText", xmlhttprequest_response, NULL),
    JS_CGETSET_DEF("status", xmlhttprequest_status_code, NULL),
    JS_CGETSET_DEF("statusText", xmlhttprequest_status, NULL),
    JS_CGETSET_DEF("responseType", xmlhttprequest_response_type, NULL),

    JS_PROP_STRING_DEF("[Symbol.toStringTag]", XMLHTTPREQUEST_NAME, JS_PROP_CONFIGURABLE)
};

static JSValue xmlhttprequest_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
	if (argc != 0) {
	    return JS_EXCEPTION;
	}

    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        return JS_EXCEPTION;
    }
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, JS_CLASS_XMLHTTPREQUEST_ID);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }

	xmlhttprequest_data *data = js_mallocz(ctx, sizeof (xmlhttprequest_data));
    if (data == NULL) {
        return JS_EXCEPTION;
    }

	JSValue listeners = JS_NewObject(ctx);
	JSValue onreadystatechange = JS_NewArray(ctx);
	JS_SetPropertyStr(ctx, listeners, "onreadystatechange", onreadystatechange);
	JSValue onload = JS_NewArray(ctx);
	JS_SetPropertyStr(ctx, listeners, "onload", onload);
	JSValue onerror = JS_NewArray(ctx);
	JS_SetPropertyStr(ctx, listeners, "onerror", onerror);
	data->event_listeners = listeners;

    data->response = "";
    data->ready_state = 0;
    data->status = "";
    data->response_type = "";
    data->status_code = 0;
    JS_SetOpaque(obj, data);

	return obj;
}

void xmlhttprequest_init(JSContext *ctx) {
    curl_global_init(CURL_GLOBAL_ALL);

    xmlhttprequest_requests = NULL;

    JS_NewClassID(&JS_CLASS_XMLHTTPREQUEST_ID);
    JS_NewClass(JS_GetRuntime(ctx), JS_CLASS_XMLHTTPREQUEST_ID, &JS_CLASS_XMLHTTPREQUEST);

    JSValue ctor = JS_NewCFunction2(ctx, xmlhttprequest_constructor, XMLHTTPREQUEST_NAME, 0, JS_CFUNC_constructor, 0);
    JSValue proto = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, proto, xmlhttprequest_proto_funcs, sizeof xmlhttprequest_proto_funcs / sizeof (JSCFunctionListEntry));
    JS_SetClassProto(ctx, JS_CLASS_XMLHTTPREQUEST_ID, proto);

    JS_SetConstructor(ctx, ctor, proto);

    JSValue global_obj = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, global_obj, XMLHTTPREQUEST_NAME, ctor, JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE | JS_PROP_HAS_VALUE);
    JS_FreeValue(ctx, global_obj);
}

void xmlhttprequest_loop(JSContext *ctx) {
    pthread_mutex_lock(&lock);
    if (curl_length > 0) {
        for (int i = 0; i < curl_length; i++) {
            if (eventloop_is_exit_requested()) {
                break;
            }
            on_done(ctx, results[i]);
        }

        curl_length = 0;
        results = NULL;
    }
    pthread_mutex_unlock(&lock);
}

void xmlhttprequest_cleanup(JSContext *ctx) {
    if (running > 0) {
        pthread_mutex_lock(&run_lock);
        pthread_cond_wait(&run_cond, &run_lock);
        pthread_mutex_unlock(&run_lock);
    }

    for (int i = 0; i < requests_length; i++) {
        if (!JS_IsNull(xmlhttprequest_requests[i])) {
            JS_FreeValue(ctx, xmlhttprequest_requests[i]);
        }
    }
    js_free(ctx, xmlhttprequest_requests);

    pthread_mutex_destroy(&lock);
    pthread_mutex_destroy(&run_lock);
    pthread_cond_destroy(&run_cond);
    curl_global_cleanup();
}

int xmlhttprequest_isdone() {
    return running < 1;
}
