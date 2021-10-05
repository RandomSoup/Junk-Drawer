#if !defined(EVENTLOOP_H)
#define EVENTLOOP_H

#include "duktape.h"

int eventloop_register(duk_context *);

int eventloop_run(duk_context *, char *);

void eventloop_cleanup();

void eventloop_interrupt_sleep();

#endif
