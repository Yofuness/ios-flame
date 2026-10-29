/* sunfish build: the libxpc declarations bluetool-stub needs (the theos iOS 16.5 SDK has no xpc headers;
 * libSystem exports the symbols). Same idea as the stub in tools/build-cocoatop-linux.sh. */
#ifndef XPC_DECL_H
#define XPC_DECL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <dispatch/dispatch.h>
typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
typedef bool (^xpc_dictionary_applier_t)(const char *key, xpc_object_t value);
extern const struct _xpc_type_s _xpc_type_dictionary, _xpc_type_connection, _xpc_type_error, _xpc_type_string;
#define XPC_TYPE_STRING (&_xpc_type_string)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1ull << 0)
xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t q, uint64_t flags);
void xpc_connection_set_event_handler(xpc_connection_t c, xpc_handler_t h);
void xpc_connection_resume(xpc_connection_t c);
void xpc_connection_send_message(xpc_connection_t c, xpc_object_t m);
int xpc_connection_get_pid(xpc_connection_t c);
xpc_type_t xpc_get_type(xpc_object_t o);
char *xpc_copy_description(xpc_object_t o);
void xpc_release(xpc_object_t o);
xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
xpc_object_t xpc_dictionary_get_value(xpc_object_t d, const char *key);
const char *xpc_dictionary_get_string(xpc_object_t d, const char *key);
void xpc_dictionary_set_int64(xpc_object_t d, const char *key, int64_t v);
void xpc_dictionary_set_string(xpc_object_t d, const char *key, const char *s);
const char *xpc_string_get_string_ptr(xpc_object_t s);
void xpc_dictionary_set_value(xpc_object_t d, const char *key, xpc_object_t v);
bool xpc_dictionary_apply(xpc_object_t d, xpc_dictionary_applier_t f);
#endif
