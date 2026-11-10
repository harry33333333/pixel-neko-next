#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>
#include <stdint.h>

void* memset(void* dest, int val, size_t count);
void* memcpy(void* dest, const void* src, size_t count);
void* memmove(void* dest, const void* src, size_t count);
int   memcmp(const void* s1, const void* s2, size_t n);

size_t strlen(const char* str);
int    strcmp(const char* s1, const char* s2);
int    strncmp(const char* s1, const char* s2, size_t n);
char*  strcpy(char* dest, const char* src);
char*  strncpy(char* dest, const char* src, size_t n);

/* Fortify runtime safety stubs for freestanding compilation */
void* __memset_chk(void* dest, int val, size_t count, size_t destlen);
void* __memcpy_chk(void* dest, const void* src, size_t count, size_t destlen);
void* __memmove_chk(void* dest, const void* src, size_t count, size_t destlen);

#endif /* _STRING_H */
