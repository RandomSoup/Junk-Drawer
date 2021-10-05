#if !defined(EVENTLOOP_H)
#define EVENTLOOP_H

#include <quickjs.h>

int eventloop_register(JSContext *);

int eventloop_run(JSContext *, void *, size_t, const char *);

void eventloop_cleanup(JSContext *);

void eventloop_interrupt_sleep();

void eventloop_request_exit();

int eventloop_is_exit_requested();

#endif
