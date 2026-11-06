#ifndef MF_MD5_H
#define MF_MD5_H

#include <stddef.h>
#include <stdint.h>

void md5_buffer(const void *data, size_t len, uint8_t out[16]);
void md5_hex(const uint8_t digest[16], char out[33]);

#endif
