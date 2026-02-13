#include "internal.h"

static void mem_destr(struct wdf_memory *mem) {
    //Free memory
    wdf_cleanup_obj(&mem->object);
    if(mem->owns_data) free(mem->data);
    free(mem);
}
__winfnc NTSTATUS WdfMemoryCreatePreallocated(WDF_DRIVER_GLOBALS *globals, WDF_OBJECT_ATTRIBUTES *obj_attrs, void *buffer, size_t buffer_size, WDFOBJECT *out) {
    //Create the memory object
    struct wdf_memory *mem = (struct wdf_memory*) malloc(sizeof(struct wdf_memory));
    if(!mem) return winerr_from_errno();

    wdf_create_obj((struct wdf_object*) winwdf_get_driver(globals), &mem->object, (wdf_obj_destr_fnc*) mem_destr, obj_attrs);

    mem->data = buffer;
    mem->data_size = buffer_size;
    mem->owns_data = false;

    *out = &mem->object;
    return STATUS_SUCCESS;
}
WDFFUNC(WdfMemoryCreatePreallocated, 118)

__winfnc NTSTATUS WdfMemoryCreate(WDF_DRIVER_GLOBALS *globals, WDF_OBJECT_ATTRIBUTES *obj_attrs, ULONG pool_type, ULONG pool_tag, size_t buffer_size, WDFOBJECT *out, void **buffer) {
    struct wdf_memory *mem = (struct wdf_memory*) malloc(sizeof(struct wdf_memory));
    if(!mem) return winerr_from_errno();

    wdf_create_obj((struct wdf_object*) winwdf_get_driver(globals), &mem->object, (wdf_obj_destr_fnc*) mem_destr, obj_attrs);

    mem->data = calloc(1, buffer_size);
    if(!mem->data) { free(mem); return winerr_from_errno(); }
    mem->data_size = buffer_size;
    mem->owns_data = true;

    *out = &mem->object;
    if(buffer) *buffer = mem->data;
    return STATUS_SUCCESS;
}
WDFFUNC(WdfMemoryCreate, 117)

__winfnc void *WdfMemoryGetBuffer(WDF_DRIVER_GLOBALS *globals, WDFOBJECT mem_obj, size_t *size) {
    struct wdf_memory *mem = (struct wdf_memory*) mem_obj;
    if(size) *size = mem->data_size;
    return mem->data;
}
WDFFUNC(WdfMemoryGetBuffer, 119)

__winfnc NTSTATUS WdfMemoryAssignBuffer(WDF_DRIVER_GLOBALS *globals, WDFOBJECT mem_obj, void *buffer, size_t size) {
    struct wdf_memory *mem = (struct wdf_memory*) mem_obj;
    if(mem->owns_data) free(mem->data);
    mem->data = buffer;
    mem->data_size = size;
    mem->owns_data = false;
    return STATUS_SUCCESS;
}
WDFFUNC(WdfMemoryAssignBuffer, 120)

__winfnc NTSTATUS WdfMemoryCopyToBuffer(WDF_DRIVER_GLOBALS *globals, WDFOBJECT src_obj, size_t src_offset, void *dst, size_t num_bytes) {
    struct wdf_memory *src = (struct wdf_memory*) src_obj;
    if(src_offset + num_bytes > src->data_size) return 0xC0000023L; /* STATUS_BUFFER_TOO_SMALL */
    memcpy(dst, (uint8_t*)src->data + src_offset, num_bytes);
    return STATUS_SUCCESS;
}
WDFFUNC(WdfMemoryCopyToBuffer, 121)

__winfnc NTSTATUS WdfMemoryCopyFromBuffer(WDF_DRIVER_GLOBALS *globals, WDFOBJECT dst_obj, size_t dst_offset, void *src, size_t num_bytes) {
    struct wdf_memory *dst = (struct wdf_memory*) dst_obj;
    if(dst_offset + num_bytes > dst->data_size) return 0xC0000023L; /* STATUS_BUFFER_TOO_SMALL */
    memcpy((uint8_t*)dst->data + dst_offset, src, num_bytes);
    return STATUS_SUCCESS;
}
WDFFUNC(WdfMemoryCopyFromBuffer, 122)