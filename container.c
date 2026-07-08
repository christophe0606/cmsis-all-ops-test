#include "container.h"

_Static_assert(sizeof(container_object_t) == 8, "Flash container object descriptor layout must match container.py");
_Static_assert(sizeof(container_header_t) == 16, "Flash container header layout must match container.py");

#define EXPECTED_MAGIC 0xBEEFDEADu

#if !defined(USE_FLASH)
extern const unsigned char _container_address_start[];
#else
const unsigned char *_container_address_start = (const unsigned char *)IMAGE_ADDRESS;
#endif

static int has_valid_container_header(void)
{
    const container_header_t *container_header = (const container_header_t *)_container_address_start;
    return container_header->magic == EXPECTED_MAGIC &&
           container_header->nb_objects > 0;
}

void *get_container()
{
    return (void *)_container_address_start;
}

size_t get_container_length()
{
    const container_header_t *container_header = (const container_header_t *)_container_address_start;
    return has_valid_container_header() ? container_header->size : 0;
}

uint32_t get_max_object_size()
{
    const container_header_t *container_header = (const container_header_t *)_container_address_start;
    return has_valid_container_header() ? container_header->max_object_size : 0;
}


const uint8_t * get_object_pointer(int index)
{
    const container_header_t *container_header = (const container_header_t *)_container_address_start;
    if (has_valid_container_header() &&
        index >= 0 &&
        (uint32_t)index < container_header->nb_objects) {
        /*
         * _container_address_start must have the same alignment as the
         * base_addr used by container.py when generating the container.
         */
        return _container_address_start + container_header->objects[index].container_object_offset;
    }
    return NULL;
}


size_t get_object_length(int index)
{
    const container_header_t *container_header = (const container_header_t *)_container_address_start;
    if (has_valid_container_header() &&
        index >= 0 &&
        (uint32_t)index < container_header->nb_objects) {
        return container_header->objects[index].container_object_size;
    }
    return 0;
}
