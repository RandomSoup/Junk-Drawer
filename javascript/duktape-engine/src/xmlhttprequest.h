#ifndef XMLHTTPREQUEST_H
#define XMLHTTPREQUEST_H

#include "duktape.h"

void xmlhttprequest_init(duk_context *);

void xmlhttprequest_loop(duk_context *);

int xmlhttprequest_cleanup();

int xmlhttprequest_isdone();

#endif
