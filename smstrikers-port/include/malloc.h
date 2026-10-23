#ifndef PORT_MSL_MALLOC_H
#define PORT_MSL_MALLOC_H
#if defined(__has_include_next) && __has_include_next(<malloc.h>)
# include_next <malloc.h>
#else
# include <stdlib.h>
#endif
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void MallocInit(void* base, size_t len);
void MallocCleanup(void);
int  MallocIsInitalized(void);
void GetFreeArena(size_t* max_size, size_t* free_size, size_t* alloc_size);
int  CheckArena(void);
void DisplayArena(void);
#ifdef __cplusplus
}
#endif
#endif // PORT_MSL_MALLOC_H
