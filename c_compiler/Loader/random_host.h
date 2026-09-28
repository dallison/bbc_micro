#ifndef random_host_h
#define random_host_h

#include <stddef.h>

// Fills buffer with operating-system entropy. Returns zero on success.
int DaveHostRandomBytes(void* buffer, size_t size);

#endif
