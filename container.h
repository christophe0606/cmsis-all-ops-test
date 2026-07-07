#ifndef CONTAINER_H
#define CONTAINER_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct {
    uint32_t container_object_size;
    uint32_t container_object_offset;
} container_object_t;

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t nb_objects;
    const container_object_t objects[];
} container_header_t;

extern void *get_container();
extern size_t get_container_length();
extern const uint8_t * get_object_pointer(int index);
extern size_t get_object_length(int index);

#if defined(__cplusplus)
}  
#endif

#endif
