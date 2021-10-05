#ifndef XMLHTTPREQUEST_H
#define XMLHTTPREQUEST_H

#include <quickjs.h>

void xmlhttprequest_init(JSContext *);

void xmlhttprequest_loop(JSContext *);

int xmlhttprequest_cleanup(JSContext *);

int xmlhttprequest_isdone();

#endif
