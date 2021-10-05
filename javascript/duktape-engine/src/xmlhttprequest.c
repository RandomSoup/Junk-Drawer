#define DUK_FILE_MACRO "xmlhttprequest.c"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <string.h>
#include "duktape.h"
#include "eventloop.h"

#define OUT_OF_MEMORY "XMLHttpRequest: Out of Memory\n"

typedef struct thread_data {
    int id;
    char *data;
    char *status;
    int statusCode;
} thread_data;

typedef struct url_data {
    int id;
    char *body;
    char *url;
    char *method;
    char ***headers;
    int headers_length;
} url_data;

thread_data **results;
int curl_length = 0;
pthread_mutex_t lock;

int running = 0;

static int prop_is_callable(duk_context *ctx, int idx, char *prop) {
    int rc = 0;
    if (duk_has_prop_string(ctx, idx, prop)) {
        duk_get_prop_string(ctx, idx, prop);
        if (duk_is_callable(ctx, -1)) {
            rc = 1;
        }
        duk_pop(ctx);
    }
    return rc;
}

static void call_method(duk_context *ctx, int this_idx) {
    duk_dup(ctx, this_idx);

    duk_push_object(ctx);
    duk_push_string(ctx, "target");
    duk_dup(ctx, this_idx);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    duk_call_method(ctx, 1);
}

static void call_event_listener(duk_context *ctx, int onload) {
    int this_idx = duk_get_top_index(ctx);

    if (prop_is_callable(ctx, -1, "onreadystatechange")) {
        duk_get_prop_string(ctx, -1, "onreadystatechange");
        call_method(ctx, this_idx);
        duk_pop(ctx);
    }
    
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("onreadystatechange"));

    int length = duk_get_length(ctx, -1);
    
    for (int i = 0; i < length; i++) {
        duk_get_prop_index(ctx, -1, i);
        call_method(ctx, this_idx);
        duk_pop(ctx);
    }
    
    duk_pop(ctx);
    
    if (onload == 1) {
        if (prop_is_callable(ctx, -1, "onload")) {
            duk_get_prop_string(ctx, -1, "onload");
            call_method(ctx, this_idx);
            duk_pop(ctx);
        }
        
        duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("onload"));

        int length = duk_get_length(ctx, -1);
        
        for (int i = 0; i < length; i++) {
            duk_get_prop_index(ctx, -1, i);
            call_method(ctx, this_idx);
            duk_pop(ctx);
        }
        
        duk_pop(ctx);
    } else if (onload == -1) {
        if (prop_is_callable(ctx, -1, "onerror")) {
            duk_get_prop_string(ctx, -1, "onerror");
            call_method(ctx, this_idx);
            duk_pop(ctx);
        }

        duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("onerror"));

        int length = duk_get_length(ctx, -1);

        for (int i = 0; i < length; i++) {
            duk_get_prop_index(ctx, -1, i);
            call_method(ctx, this_idx);
            duk_pop(ctx);
        }

        duk_pop(ctx);
    }
}

static void on_done(duk_context *ctx, thread_data *result) {
    running--;

    duk_push_global_object(ctx);
    (void) duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("requests"));
    (void) duk_get_prop_index(ctx, -1, result->id);

    duk_push_string(ctx, "readyState");
	duk_push_number(ctx, 4);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    duk_push_string(ctx, "response");
	duk_push_string(ctx, result->data);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    duk_push_string(ctx, "responseText");
	duk_push_string(ctx, result->data);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    duk_push_string(ctx, "status");
	duk_push_number(ctx, result->statusCode);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    duk_push_string(ctx, "statusText");
	duk_push_string(ctx, result->status);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

	duk_push_string(ctx, "responseType");
	duk_push_string(ctx, "text");
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    if (result->statusCode == 200) {
        call_event_listener(ctx, 1);
    } else {
        call_event_listener(ctx, -1);
    }

    duk_pop(ctx);

    (void) duk_del_prop_index(ctx, -1, result->id);

    duk_pop_2(ctx);
}

typedef struct curl_data {
    char *memory;
    size_t size;
} curl_data;
 
static size_t curl_callback(void *contents, size_t size, size_t nmemb, void *userp) {
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
    
    struct curl_slist *headers = NULL;
    for (int i = 0; i < data->headers_length; i++) {
        char *header = malloc(sizeof (data->headers[i][0]) + sizeof (data->headers[i][1]) + sizeof (": "));
        sprintf(header, "%s: %s", data->headers[i][0], data->headers[i][1]);
        headers = curl_slist_append(headers, header);
    }
    
    CURL *curl_handle;
    CURLcode res;
    curl_data *chunk = malloc(sizeof (curl_data));
    chunk->memory = malloc(1);
    chunk->size = 0;
    curl_handle = curl_easy_init();
    curl_easy_setopt(curl_handle, CURLOPT_URL, data->url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0); 
    header_data *statusText = malloc(sizeof (header_data));
    statusText->done = 0;
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *) statusText); 
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");
    if (headers) {
        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers); 
    }
    if (strcmp(data->method, "GET") == 0) {
        curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1);
    } else if (strcmp(data->method, "POST") == 0) {
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, data->body);
    }
    res = curl_easy_perform(curl_handle);
    if (res != CURLE_OK) {
        new_data->status = (char *) curl_easy_strerror(res);
        long response_code;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
        new_data->statusCode = response_code;
        new_data->data = "";
        goto realloc;
    } else {
        long response_code;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
        new_data->statusCode = response_code;
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
    curl_slist_free_all(headers);

    pthread_mutex_lock(&lock);
    curl_length++;
    
    thread_data **tmp = realloc(results, curl_length * sizeof *results);
    if (tmp) {
        results = tmp;
    } else {
        fprintf(stderr, OUT_OF_MEMORY);
        fflush(stderr);
        exit(1);
    }
    results[curl_length - 1] = new_data;
    
    pthread_mutex_unlock(&lock);

    eventloop_interrupt_sleep();
    
    return NULL;
}

int global_id = 0;

static duk_ret_t xmlhttprequest_open(duk_context *ctx) {
    (void) duk_require_string(ctx, 0);
    (void) duk_require_string(ctx, 1);

    duk_push_this(ctx);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("method"));
    duk_dup(ctx, 1);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("url"));

    duk_push_string(ctx, "readyState");
	duk_push_number(ctx, 1);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);
    
    call_event_listener(ctx, 0);
    
    duk_pop_2(ctx);
    
    return 0;
}

static duk_ret_t xmlhttprequest_send(duk_context *ctx) {
    url_data *data = (url_data *) malloc(sizeof (url_data));
    data->id = global_id;
    duk_push_this(ctx);
    
    (void) duk_get_prop_string(ctx, -1, "readyState");
    if (duk_to_number(ctx, -1) < 1) {
        (void) duk_error(ctx, DUK_ERR_ERROR, "open() has not been called");
    }
    if (duk_to_number(ctx, -1) > 1) {
        (void) duk_error(ctx, DUK_ERR_ERROR, "send() has already been called");
    }
    duk_pop(ctx);
    
    duk_push_string(ctx, "readyState");
	duk_push_number(ctx, 3);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);
    
    call_event_listener(ctx, 0);
    
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("headers"));
    int length = duk_get_length(ctx, -1);
    char ***headers = malloc((length * 2) * sizeof (char *));
    for (int i = 0; i < length; i++) {
        char **header = malloc(2 * sizeof (char *));
        duk_get_prop_index(ctx, -1, i);
        duk_get_prop_index(ctx, -1, 0);
        header[0] = (char *) duk_to_string(ctx, -1);
        duk_pop(ctx);
        duk_get_prop_index(ctx, -1, 1);
        header[1] = (char *) duk_to_string(ctx, -1);
        duk_pop(ctx);
        headers[i] = header;
        duk_pop(ctx);
    }
    duk_pop(ctx);
    data->headers = headers;
    data->headers_length = length;
    
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("url"));
    data->url = (char *) duk_to_string(ctx, -1);
    duk_pop(ctx);
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("method"));
    data->method = (char *) duk_to_string(ctx, -1);
    duk_pop_2(ctx);
    data->body = (char *) duk_to_string(ctx, 0);
    
    duk_push_global_object(ctx);
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("requests"));
    duk_push_this(ctx);
    (void) duk_put_prop_index(ctx, -2, data->id);
    duk_pop_2(ctx);
    
    global_id++;
    
    pthread_t thread;
    pthread_create(&thread, NULL, get_data, (void *) data);
    running++;
    
    return 0;
}

static duk_ret_t xmlhttprequest_setrequestheader(duk_context *ctx) {
    (void) duk_require_string(ctx, 0);
    (void) duk_require_string(ctx, 1);

    duk_push_this(ctx);
    
    (void) duk_get_prop_string(ctx, -1, "readyState");
    if (duk_to_number(ctx, -1) < 1) {
        (void) duk_error(ctx, DUK_ERR_ERROR, "open() has not been called");
    }
    if (duk_to_number(ctx, -1) > 1) {
        (void) duk_error(ctx, DUK_ERR_ERROR, "send() has already been called");
    }
    duk_pop(ctx);
    
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("headers"));
    
    duk_push_array(ctx);
    
    duk_dup(ctx, 0);
    (void) duk_put_prop_index(ctx, -2, 0);
    
    duk_dup(ctx, 1);
    (void) duk_put_prop_index(ctx, -2, 1);

    int length = duk_get_length(ctx, -1);

    (void) duk_put_prop_index(ctx, -2, length);
    
    duk_pop(ctx);
    
    return 0;
}

static duk_ret_t xmlhttprequest_addeventlistener(duk_context *ctx) {
    const char *event = duk_require_string(ctx, 0);
    (void) duk_require_callable(ctx, 1);
    
    duk_push_this(ctx);
    
    if (strcmp(event, "readystatechange") == 0) {
        duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("onreadystatechange"));

        int length = duk_get_length(ctx, -1);
        
        duk_dup(ctx, 1);
        (void) duk_put_prop_index(ctx, -2, length);
        
        duk_pop(ctx);
    } else if (strcmp(event, "load") == 0) {
        duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("onload"));

        int length = duk_get_length(ctx, -1);

        duk_dup(ctx, 1);
        (void) duk_put_prop_index(ctx, -2, length);
        
        duk_pop(ctx);
    } else if (strcmp(event, "error") == 0) {
        duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("error"));

        int length = duk_get_length(ctx, -1);

        duk_dup(ctx, 1);
        (void) duk_put_prop_index(ctx, -2, length);

        duk_pop(ctx);
    } else {
        (void) duk_error(ctx, DUK_ERR_ERROR, "Invalid Event");
    }
    
    return 0;
}

static duk_ret_t xmlhttprequest_constructor(duk_context *ctx) {
	duk_require_constructor_call(ctx);
	duk_push_this(ctx);
	
	duk_push_string(ctx, "readyState");
	duk_push_number(ctx, 0);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);
    
    duk_push_array(ctx);
    (void) duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("headers"));
    
    duk_push_array(ctx);
    (void) duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("onload"));
    
    duk_push_array(ctx);
    (void) duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("onreadystatechange"));

    duk_push_array(ctx);
    (void) duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("onerror"));

    duk_push_null(ctx);
    (void) duk_put_prop_string(ctx, -2, "onload");

    duk_push_null(ctx);
    (void) duk_put_prop_string(ctx, -2, "onreadystatechange");

    duk_push_null(ctx);
    (void) duk_put_prop_string(ctx, -2, "onerror");

    duk_push_string(ctx, "response");
    duk_push_null(ctx);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    duk_push_string(ctx, "responseText");
    duk_push_null(ctx);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    duk_push_string(ctx, "status");
    duk_push_null(ctx);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    duk_push_string(ctx, "statusText");
    duk_push_null(ctx);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);

    duk_push_string(ctx, "responseType");
    duk_push_null(ctx);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_FORCE);
	
	return 0;
}

void xmlhttprequest_init(duk_context *ctx) {
    curl_global_init(CURL_GLOBAL_ALL);
    if (pthread_mutex_init(&lock, NULL) != 0) { 
        fprintf(stderr, "XMLHttpRequest: Failed to Create Mutex Lock\n");
        fflush(stderr);
        exit(1);
    }
    
    duk_push_c_function(ctx, xmlhttprequest_constructor, 0);
	duk_push_object(ctx);
	duk_push_c_function(ctx, xmlhttprequest_send, 1);
	duk_put_prop_string(ctx, -2, "send");
	duk_push_c_function(ctx, xmlhttprequest_open, 2);
	duk_put_prop_string(ctx, -2, "open");
	duk_push_c_function(ctx, xmlhttprequest_setrequestheader, 2);
	duk_put_prop_string(ctx, -2, "setRequestHeader");
	duk_push_c_function(ctx, xmlhttprequest_addeventlistener, 2);
	duk_put_prop_string(ctx, -2, "addEventListener");
	duk_put_prop_string(ctx, -2, "prototype");
	duk_put_global_string(ctx, "XMLHttpRequest");
	
	duk_push_global_object(ctx);
	duk_push_array(ctx);
	duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("requests"));
	duk_pop(ctx);
}

void xmlhttprequest_loop(duk_context *ctx) {
    pthread_mutex_lock(&lock);
    if (curl_length > 0) {
        for (int i = 0; i < curl_length; i++) {
            on_done(ctx, results[i]);
        }
        
        curl_length = 0;
        results = NULL;
    }
    pthread_mutex_unlock(&lock);
}

void xmlhttprequest_cleanup() {
    pthread_mutex_destroy(&lock);
    curl_global_cleanup();
}

int xmlhttprequest_isdone() {
    return running < 1;
}
