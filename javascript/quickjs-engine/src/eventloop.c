#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>

#include <quickjs.h>
#include "eventloop.h"
#include "console.h"
#include "xmlhttprequest.h"
#include "lang.h"

#define MIN_WAIT 0.0
#define MAX_WAIT 60000.0
#define MAX_EXPIRIES 10

#define MAX_TIMERS 4096 /* this is quite excessive for embedded use, but good for testing */

typedef struct {
    int64_t id; /* numeric ID (returned from e.g. setTimeout); zero if unused */
    double target; /* next target time */
    double delay; /* delay/interval */
    int oneshot; /* oneshot=1 (setTimeout), repeated=0 (setInterval) */
    int removed; /* timer has been requested for removal */
    JSValueConst func;

    /* The callback associated with the timer is held in the "global stash",
     * in <stash>.eventTimers[String(id)].  The references must be deleted
     * when a timer struct is deleted.
     */
} ev_timer;

/* Active timers.  Dense list, terminates to end of list or first unused timer.
 * The list is sorted by 'target', with lowest 'target' (earliest expiry) last
 * in the list.  When a timer's callback is being called, the timer is moved
 * to 'timer_expiring' as it needs special handling should the user callback
 * delete that particular timer.
 */
static ev_timer timer_list[MAX_TIMERS];
static ev_timer timer_expiring;
static int timer_count; /* last timer at timer_count - 1 */
static int64_t timer_next_id = 1;

/* Misc */
static int exit_requested = 0;

static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;

/* Interrupt poll() */
void eventloop_interrupt_sleep() {
    pthread_mutex_lock(&wait_lock);
    pthread_cond_signal(&wait_cond);
    pthread_mutex_unlock(&wait_lock);
}

/* Get Javascript compatible 'now' timestamp (millisecs since 1970). */
static double get_now(void) {
    struct timeval tv;
    int rc;

    rc = gettimeofday(&tv, NULL);
    if (rc != 0) {
        /* Should never happen, so return whatever. */
        return 0.0;
    }
    return ((double) tv.tv_sec) * 1000.0 + ((double) tv.tv_usec) / 1000.0;
}

static ev_timer *find_nearest_timer(void) {
    /* Last timer expires first (list is always kept sorted). */
    if (timer_count <= 0) {
        return NULL;
    }
    return timer_list + timer_count - 1;
}

/* Bubble last timer on timer list backwards until it has been moved to
 * its proper sorted position (based on 'target' time).
 */
static void bubble_last_timer(void) {
    int i;
    int n = timer_count;
    ev_timer *t;
    ev_timer tmp;

    for (i = n - 1; i > 0; i--) {
        /* Timer to bubble is at index i, timer to compare to is
         * at i-1 (both guaranteed to exist).
         */
        t = timer_list + i;
        if (t->target <= (t-1)->target) {
            /* 't' expires earlier than (or same time as) 't-1', so we're done. */
            break;
        } else {
            /* 't' expires later than 't-1', so swap them and repeat. */
            memcpy((void *) &tmp, (void *) (t - 1), sizeof (ev_timer));
            memcpy((void *) (t - 1), (void *) t, sizeof (ev_timer));
            memcpy((void *) t, (void *) &tmp, sizeof (ev_timer));
        }
    }
}

static void expire_timers(JSContext *ctx) {
    ev_timer *t;
    int sanity = MAX_EXPIRIES;
    double now;
    JSValueConst args[0];
    JSValue val;
    JSValue global_obj = JS_GetGlobalObject(ctx);

    /*
     * Because a user callback can mutate the timer list (by adding or deleting
     * a timer), we expire one timer and then rescan from the end again.  There
     * is a sanity limit on how many times we do this per expiry round.
     */

    now = get_now();
    while (sanity-- > 0) {
        /*
         * If exit has been requested, exit without running further
         * callbacks.
         */
        if (exit_requested) {
            break;
        }

        /*
         * Expired timer(s) still exist?
         */
        if (timer_count <= 0) {
            break;
        }
        t = timer_list + timer_count - 1;
        if (t->target > now) {
            break;
        }

        /*
         * Move the timer to 'expiring' for the duration of the callback.
         * Mark a one-shot timer deleted, compute a new target for an interval.
         */
        memcpy((void *) &timer_expiring, (void *) t, sizeof (ev_timer));
        memset((void *) t, 0, sizeof (ev_timer));
        timer_count--;
        t = &timer_expiring;

        if (t->oneshot) {
            t->removed = 1;
        } else {
            t->target = now + t->delay; /* XXX: or t->target + t->delay? */
        }

        /*
         * Call timer callback.  The callback can operate on the timer list:
         * add new timers, remove timers.  The callback can even remove the
         * expired timer whose callback we're calling.  However, because the
         * timer being expired has been moved to 'timer_expiring', we don't
         * need to worry about the timer's offset changing on the timer list.
         */
        val = JS_Call(ctx, t->func, global_obj, 0, args);
        if (JS_IsException(val)) {
            js_std_dump_error(ctx);
            eventloop_request_exit();
        } else {
            JS_FreeValue(ctx, val);
        }

        if (!t->removed) {
            /* Interval timer, not removed by user callback.  Queue back to
             * timer list and bubble to its final sorted position.
             */
            if (timer_count >= MAX_TIMERS) {
                fprintf(stderr, "eventloop error: "OUT_OF_TIMER_SLOTS_ERR"\n");
                fflush(stderr);
                eventloop_request_exit();
                break;
            }
            memcpy((void *) (timer_list + timer_count), (void *) t, sizeof (ev_timer));
            timer_count++;
            bubble_last_timer();
        } else {
            JS_FreeValue(ctx, t->func);
        }
    }
    JS_FreeValue(ctx, global_obj);

    memset((void *) &timer_expiring, 0, sizeof (ev_timer));
}

static void free_timers(JSContext *ctx) {
    int i, n;
    ev_timer *t;

    n = timer_count;
    for (i = 0; i < n; i++) {
        t = timer_list + i;
        if (!t->removed) {
            JS_FreeValue(ctx, t->func);
        }
    }
}

static struct timespec create_timespec(int timeInMs) {
    struct timeval tv;
    struct timespec ts;

    gettimeofday(&tv, NULL);
    ts.tv_sec = time(NULL) + timeInMs / 1000;
    ts.tv_nsec = tv.tv_usec * 1000 + 1000 * 1000 * (timeInMs % 1000);
    ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000);
    ts.tv_nsec %= (1000 * 1000 * 1000);

    return ts;
}

static void eventloop_start(JSContext *ctx) {
    ev_timer *t;
    double now;
    double diff;
    int timeout;
    int rc;
    struct timespec ts;

    for (;;) {
        /*
         * Expire timers.
         */
        expire_timers(ctx);

        /*
         * Call Events for XMLHttpRequest
         */
        xmlhttprequest_loop(ctx);

        /*
         * If exit requested, bail out as fast as possible.
         */
        if (exit_requested) {
            break;
        }

        now = get_now();
        t = find_nearest_timer();
        if (t) {
            diff = t->target - now;
            if (diff < MIN_WAIT) {
                diff = MIN_WAIT;
            } else if (diff > MAX_WAIT) {
                diff = MAX_WAIT;
            }
            timeout = (int) diff;
        } else {
            if (xmlhttprequest_isdone()) {
                break;
            }
            timeout = (int) MAX_WAIT;
        }
        pthread_mutex_lock(&wait_lock);
        ts = create_timespec(timeout);
        pthread_cond_timedwait(&wait_cond, &wait_lock, &ts);
        pthread_mutex_unlock(&wait_lock);
    }
}

static JSValue create_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    double delay;
    int oneshot;
    int idx;
    int64_t timer_id;
    double now;
    ev_timer *t;
    JSValueConst func;

    now = get_now();

    /*
     * indexes:
     *  0 = function (callback)
     *  1 = delay
     *  2 = boolean: oneshot
     */
    if (argc != 3) {
        return JS_EXCEPTION;
    }
    if (JS_IsFunction(ctx, argv[0])) {
        func = argv[0];
    } else {
        return JS_ThrowTypeError(ctx, NOT_A_FUNCTION_ERR);
    }
    if (JS_ToFloat64(ctx, &delay, argv[1])) {
        return JS_EXCEPTION;
    }
    if (JS_IsBool(argv[2])) {
        oneshot = JS_ToBool(ctx, argv[2]);
    } else {
        return JS_EXCEPTION;
    }

    if (timer_count >= MAX_TIMERS) {
        return JS_ThrowRangeError(ctx, OUT_OF_TIMER_SLOTS_ERR);
    }
    idx = timer_count++;
    timer_id = timer_next_id++;
    t = timer_list + idx;

    memset((void *) t, 0, sizeof (ev_timer));
    t->id = timer_id;
    t->target = now + delay;
    t->delay = delay;
    t->oneshot = oneshot;
    t->removed = 0;
    t->func = JS_DupValue(ctx, func);

    /*
     * Timer is now at the last position; use swaps to "bubble" it to its
     * correct sorted position.
     */
    bubble_last_timer();

    return JS_NewInt64(ctx, timer_id);
}

static JSValue delete_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int i, n;
    int64_t timer_id;
    ev_timer *t;

    /*
     * indexes:
     *  0 = timer id
     */
    if (argc != 1) {
        return JS_EXCEPTION;
    }
    if (JS_ToInt64(ctx, &timer_id, argv[0])) {
        return JS_EXCEPTION;
    }

    /*
     * Unlike insertion, deletion needs a full scan of the timer list
     * and an expensive remove.  If no match is found, nothing is deleted.
     * Caller gets a boolean return code indicating match.
     *
     * When a timer is being expired and its user callback is running,
     * the timer has been moved to 'timer_expiring' and its deletion
     * needs special handling: just mark it to-be-deleted and let the
     * expiry code remove it.
     */
    t = &timer_expiring;
    if (t->id == timer_id) {
        t->removed = 1;
        return JS_NewBool(ctx, 1);
    }

    n = timer_count;
    for (i = 0; i < n; i++) {
        t = timer_list + i;
        if (t->id == timer_id) {
            t->removed = 1;
            JS_FreeValue(ctx, t->func);
            /*
             * Shift elements downwards to keep the timer list dense
             * (no need if last element).
             */
            if (i < timer_count - 1) {
                memmove((void *) t, (void *) (t + 1), (timer_count - i - 1) * sizeof (ev_timer));
            }

            /* Zero last element for clarity. */
            memset((void *) (timer_list + n - 1), 0, sizeof (ev_timer));

            /* Update timer_count. */
            timer_count--;

            break;
        }
    }

    return JS_NewBool(ctx, 0);
}

static JSValue request_exit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void) ctx;
    (void) this_val;
    (void) argc;
    (void) argv;

    eventloop_request_exit();
    return JS_UNDEFINED;
}

void eventloop_request_exit() {
    exit_requested = 1;
}

static const char *eventloop_js = ""
    /*
     * C eventloop (eventloop.c).
     *
     * ECMAScript code to initialize the exposed API (setTimeout() etc) when
     * using the C eventloop.
     *
     * https://developer.mozilla.org/en-US/docs/Web/JavaScript/Timers
     */

    /*
     * Timer API
     */

    "(function (EventLoop, global) {\n"
        "global.setTimeout = function (func, delay) {\n"
            "var cb_func;\n"
            "var bind_args;\n"
            "var timer_id;\n"

            /*
             * Delay can be optional at least in some contexts, so tolerate that.
             * https://developer.mozilla.org/en-US/docs/Web/API/WindowOrWorkerGlobalScope/setTimeout
             */
            "if (typeof delay !== 'number') {\n"
                "if (typeof delay === 'undefined') {\n"
                    "delay = 0;\n"
                "} else {\n"
                    "throw new TypeError('"INVALID_DELAY_ERR"');\n"
                "}\n"
            "}\n"

            "if (typeof func === 'string') {\n"
                /* Legacy case: callback is a string. */
                "cb_func = eval.bind(this, func);\n"
            "} else if (typeof func !== 'function') {\n"
                "throw new TypeError('"INVALID_CALLBACK_ERR"');\n"
            "} else if (arguments.length > 2) {\n"
                /* Special case: callback arguments are provided. */
                "bind_args = Array.prototype.slice.call(arguments, 2);\n" /* [ arg1, arg2, ... ] */
                "bind_args.unshift(this);\n" /* [ global(this), arg1, arg2, ... ] */
                "cb_func = func.bind.apply(func, bind_args);\n"
            "} else {\n"
                /* Normal case: callback given as a function without arguments. */
                "cb_func = func;\n"
            "}\n"

            "timer_id = EventLoop.createTimer(cb_func, delay, true /*oneshot*/);\n"

            "return timer_id;\n"
        "};\n"

        "global.clearTimeout = function (timer_id) {"
            "if (typeof timer_id !== 'number') {\n"
                "throw new TypeError('"INVALID_TIMER_ID_ERR"');\n"
            "}\n"
            "EventLoop.deleteTimer(timer_id);\n"
        "};\n"

        "global.setInterval = function (func, delay) {\n"
            "var cb_func;\n"
            "var bind_args;\n"
            "var timer_id;\n"

            "if (typeof delay !== 'number') {\n"
                "if (typeof delay === 'undefined') {\n"
                    "delay = 0;\n"
                "} else {\n"
                    "throw new TypeError('"INVALID_DELAY_ERR"');\n"
                "}\n"
            "}\n"

            "if (typeof func === 'string') {\n"
                /* Legacy case: callback is a string. */
                "cb_func = eval.bind(this, func);\n"
            "} else if (typeof func !== 'function') {\n"
                "throw new TypeError('"INVALID_CALLBACK_ERR"');\n"
            "} else if (arguments.length > 2) {\n"
                /* Special case: callback arguments are provided. */
                "bind_args = Array.prototype.slice.call(arguments, 2);\n" /* [ arg1, arg2, ... ] */
                "bind_args.unshift(this);\n" /* [ global(this), arg1, arg2, ... ] */
                "cb_func = func.bind.apply(func, bind_args);\n"
            "} else {\n"
                /* Normal case: callback given as a function without arguments. */
                "cb_func = func;\n"
            "}\n"

            "timer_id = EventLoop.createTimer(cb_func, delay, false /*oneshot*/);\n"

            "return timer_id;\n"
        "};\n"

        "global.clearInterval = function (timer_id) {\n"
            "if (typeof timer_id !== 'number') {\n"
                "throw new TypeError('"INVALID_TIMER_ID_ERR"');\n"
            "}\n"
            "EventLoop.deleteTimer(timer_id);\n"
        "};\n"

        "global.requestEventLoopExit = function () {\n"
            "EventLoop.requestExit();\n"
        "};\n"
    "});\n";

int eventloop_register(JSContext *ctx) {
    JSValue val;
    JSValue val2;
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue loop;
    JSValueConst args[2];

    loop = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loop, "createTimer", JS_NewCFunction(ctx, create_timer, INTERNAL_FUINCTION, 3));
    JS_SetPropertyStr(ctx, loop, "deleteTimer", JS_NewCFunction(ctx, delete_timer, INTERNAL_FUINCTION, 1));
    JS_SetPropertyStr(ctx, loop, "requestExit", JS_NewCFunction(ctx, request_exit, INTERNAL_FUINCTION, 0));

    val = JS_Eval(ctx, eventloop_js, strlen(eventloop_js), INIT_FILENAME, 0);
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        return 1;
    }

    args[0] = loop;
    args[1] = global_obj;
    val2 = JS_Call(ctx, val, global_obj, 2, args);
    JS_FreeValue(ctx, val);
    if (JS_IsException(val2)) {
        js_std_dump_error(ctx);
        return 1;
    } else {
        JS_FreeValue(ctx, val2);
    }
    JS_FreeValue(ctx, loop);
    JS_FreeValue(ctx, global_obj);

    memset((void *) timer_list, 0, MAX_TIMERS * sizeof (ev_timer));
    memset((void *) &timer_expiring, 0, sizeof (ev_timer));

    xmlhttprequest_init(ctx);

    return 0;
}

static const char *start_js = "(function (src, filename, eval_filename) { setTimeout(function () { eval_filename(src, filename); }, 0); });";

int has_suffix(const char *str, const char *suffix) {
    size_t len = strlen(str);
    size_t slen = strlen(suffix);
    return (len >= slen && !memcmp(str + len - slen, suffix, slen));
}

static JSValue eval_filename(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue rc;
    JSValue global_obj = JS_GetGlobalObject(ctx);

    if (argc != 2) {
        return JS_EXCEPTION;
    }
    const char *data = JS_ToCString(ctx, argv[0]);
    const char *filename = JS_ToCString(ctx, argv[1]);

    int eval_flags;
    if (has_suffix(filename, ".mjs")) {
        eval_flags = JS_EVAL_TYPE_MODULE;
    } else {
        eval_flags = JS_EVAL_TYPE_GLOBAL;
    }
    rc = JS_Eval(ctx, data, strlen(data), filename, eval_flags);

    JS_FreeCString(ctx, data);
    JS_FreeCString(ctx, filename);
    JS_FreeValue(ctx, global_obj);
    return rc;
}

int eventloop_run(JSContext *ctx, void *data, size_t length, const char *filename) {
    /* Start a zero timer which will call _USERCODE from within
     * the event loop.
     */
    JSValue start_func = JS_Eval(ctx, start_js, strlen(start_js), INIT_FILENAME, 0);
    if (JS_IsException(start_func)) {
        js_std_dump_error(ctx);
        return 1;
    }

    JSValue val;
    JSValue str = JS_NewStringLen(ctx, data, length);
    JSValue filename_js = JS_NewString(ctx, filename);
    JSValue eval_filename_js = JS_NewCFunction(ctx, eval_filename, INTERNAL_FUINCTION, 2);
    JSValue global_obj = JS_GetGlobalObject(ctx);

    JSValueConst args[3];
    args[0] = str;
    args[1] = filename_js;
    args[2] = eval_filename_js;

    val = JS_Call(ctx, start_func, global_obj, 3, args);

    JS_FreeValue(ctx, start_func);
    JS_FreeValue(ctx, global_obj);
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, filename_js);
    JS_FreeValue(ctx, eval_filename_js);

    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        return 1;
    } else {
        JS_FreeValue(ctx, val);
    }

	/* Finally, launch eventloop.  This call only returns after the
	 * eventloop terminates.
	 */
	exit_requested = 0;
    eventloop_start(ctx);

	return 0;
}

void eventloop_cleanup(JSContext *ctx) {
    xmlhttprequest_cleanup(ctx);
    pthread_mutex_destroy(&wait_lock);
    pthread_cond_destroy(&wait_cond);
    free_timers(ctx);
}

int eventloop_is_exit_requested() {
    return exit_requested;
}
