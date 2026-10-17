/* Where the generated protocol header comes from. The ID layout, node-ID ranges and port ranges
 * are generated from schema/protocol.yaml by `anv-schema gen` and never copied.
 *
 * Builds that add schema/generated/c to the include path (CMake, ESP-IDF, PlatformIO) find it
 * directly. Arduino can't add include paths for a library, so it falls back to the generated
 * file's place in this repository, relative to this file. */
#ifndef ANV_PROTO_SCHEMA_H
#define ANV_PROTO_SCHEMA_H

#if defined(__has_include)
#if __has_include("anv_protocol.h")
#include "anv_protocol.h"
#define ANV_PROTO_SCHEMA_FOUND 1
#endif
#endif

#ifndef ANV_PROTO_SCHEMA_FOUND
#include "../../../../schema/generated/c/anv_protocol.h"
#endif

#endif /* ANV_PROTO_SCHEMA_H */
