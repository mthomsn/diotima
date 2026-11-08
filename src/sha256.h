#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
/* Hex digest (65 bytes incl. NUL) of n bytes at p. */
void sha256_hex(const void *p, size_t n, char out[65]);
#endif
