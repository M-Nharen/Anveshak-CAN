/* Compiles the generated protocol code (schema/generated/c/anv_protocol.c: pack/unpack and the
 * common parameter table) as part of this library, so no build has to list it. Arduino can't
 * add source folders to a library, so the file is pulled in by include, found the same way as
 * anv_proto_schema.h finds the header. */
#if defined(__has_include)
#if __has_include("anv_protocol.c")
#include "anv_protocol.c"
#define ANV_NODE_PROTOCOL_C_FOUND 1
#endif
#endif

#ifndef ANV_NODE_PROTOCOL_C_FOUND
#include "../../../../schema/generated/c/anv_protocol.c"
#endif
