/*
 * Cirrus CL-GD5446 2D Hardware Acceleration - OpenCL Backend
 *
 * Tiny386 Emulation System
 * Dynamic runtime OpenCL loader with safe CPU fallback.
 */

#include "cirrus_opencl.h"
#include "cirrus_vga.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386__)) && !defined(ESP_PLATFORM)
#include <dlfcn.h>

/* Minimal OpenCL type definitions for dynamic loading */
typedef intptr_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
typedef void *cl_platform_id;
typedef void *cl_device_id;
typedef void *cl_context;
typedef void *cl_command_queue;
typedef void *cl_mem;
typedef void *cl_program;
typedef void *cl_kernel;
typedef void *cl_event;
typedef uint64_t cl_mem_flags;
typedef uint64_t cl_device_type;

#define CL_SUCCESS                                  0
#define CL_DEVICE_TYPE_DEFAULT                      (1 << 0)
#define CL_DEVICE_TYPE_CPU                          (1 << 1)
#define CL_DEVICE_TYPE_GPU                          (1 << 2)
#define CL_DEVICE_TYPE_ACCELERATOR                  (1 << 3)
#define CL_DEVICE_TYPE_ALL                          0xFFFFFFFF
#define CL_MEM_READ_WRITE                           (1 << 0)
#define CL_MEM_WRITE_ONLY                           (1 << 1)
#define CL_MEM_READ_ONLY                            (1 << 2)
#define CL_MEM_USE_HOST_PTR                         (1 << 3)
#define CL_MEM_ALLOC_HOST_PTR                       (1 << 4)
#define CL_MEM_COPY_HOST_PTR                        (1 << 5)
#define CL_TRUE                                     1
#define CL_FALSE                                    0

/* Function pointer types */
typedef cl_int (*pfn_clGetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
typedef cl_int (*pfn_clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *);
typedef cl_context (*pfn_clCreateContext)(const void *, cl_uint, const cl_device_id *, void (*)(const char *, const void *, size_t, void *), void *, cl_int *);
typedef cl_command_queue (*pfn_clCreateCommandQueue)(cl_context, cl_device_id, uint64_t, cl_int *);
typedef cl_program (*pfn_clCreateProgramWithSource)(cl_context, cl_uint, const char **, const size_t *, cl_int *);
typedef cl_int (*pfn_clBuildProgram)(cl_program, cl_uint, const cl_device_id *, const char *, void (*)(cl_program, void *), void *);
typedef cl_kernel (*pfn_clCreateKernel)(cl_program, const char *, cl_int *);
typedef cl_mem (*pfn_clCreateBuffer)(cl_context, cl_mem_flags, size_t, void *, cl_int *);
typedef cl_int (*pfn_clSetKernelArg)(cl_kernel, cl_uint, size_t, const void *);
typedef cl_int (*pfn_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, const void *, cl_uint, const cl_event *, cl_event *);
typedef cl_int (*pfn_clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, void *, cl_uint, const cl_event *, cl_event *);
typedef cl_int (*pfn_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, const cl_event *, cl_event *);
typedef cl_int (*pfn_clFinish)(cl_command_queue);
typedef cl_int (*pfn_clReleaseMemObject)(cl_mem);
typedef cl_int (*pfn_clReleaseKernel)(cl_kernel);
typedef cl_int (*pfn_clReleaseProgram)(cl_program);
typedef cl_int (*pfn_clReleaseCommandQueue)(cl_command_queue);
typedef cl_int (*pfn_clReleaseContext)(cl_context);

static void *s_lib_handle = NULL;
static bool s_opencl_initialized = false;
static bool s_opencl_available = false;

static pfn_clGetPlatformIDs cl_GetPlatformIDs = NULL;
static pfn_clGetDeviceIDs cl_GetDeviceIDs = NULL;
static pfn_clCreateContext cl_CreateContext = NULL;
static pfn_clCreateCommandQueue cl_CreateCommandQueue = NULL;
static pfn_clCreateProgramWithSource cl_CreateProgramWithSource = NULL;
static pfn_clBuildProgram cl_BuildProgram = NULL;
static pfn_clCreateKernel cl_CreateKernel = NULL;
static pfn_clCreateBuffer cl_CreateBuffer = NULL;
static pfn_clSetKernelArg cl_SetKernelArg = NULL;
static pfn_clEnqueueWriteBuffer cl_EnqueueWriteBuffer = NULL;
static pfn_clEnqueueReadBuffer cl_EnqueueReadBuffer = NULL;
static pfn_clEnqueueNDRangeKernel cl_EnqueueNDRangeKernel = NULL;
static pfn_clFinish cl_Finish = NULL;
static pfn_clReleaseMemObject cl_ReleaseMemObject = NULL;
static pfn_clReleaseKernel cl_ReleaseKernel = NULL;
static pfn_clReleaseProgram cl_ReleaseProgram = NULL;
static pfn_clReleaseCommandQueue cl_ReleaseCommandQueue = NULL;
static pfn_clReleaseContext cl_ReleaseContext = NULL;

static cl_platform_id s_platform = NULL;
static cl_device_id s_device = NULL;
static cl_context s_context = NULL;
static cl_command_queue s_queue = NULL;
static cl_program s_program = NULL;
static cl_kernel s_kernel_fill = NULL;
static cl_kernel s_kernel_copy = NULL;

/* OpenCL 1.x Kernel Source */
static const char *s_opencl_src =
    "__kernel void opencl_solid_fill(__global uchar *vram, int dstaddr, int dstpitch, int width, int height, uint color, int bpp)\n"
    "{\n"
    "    int x = get_global_id(0);\n"
    "    int y = get_global_id(1);\n"
    "    if (x >= width || y >= height) return;\n"
    "    int off = dstaddr + y * dstpitch + x * bpp;\n"
    "    if (bpp == 1) {\n"
    "        vram[off] = (uchar)color;\n"
    "    } else if (bpp == 2) {\n"
    "        vram[off] = (uchar)(color & 0xff);\n"
    "        vram[off + 1] = (uchar)((color >> 8) & 0xff);\n"
    "    } else if (bpp == 3) {\n"
    "        vram[off] = (uchar)(color & 0xff);\n"
    "        vram[off + 1] = (uchar)((color >> 8) & 0xff);\n"
    "        vram[off + 2] = (uchar)((color >> 16) & 0xff);\n"
    "    } else if (bpp == 4) {\n"
    "        vram[off] = (uchar)(color & 0xff);\n"
    "        vram[off + 1] = (uchar)((color >> 8) & 0xff);\n"
    "        vram[off + 2] = (uchar)((color >> 16) & 0xff);\n"
    "        vram[off + 3] = (uchar)((color >> 24) & 0xff);\n"
    "    }\n"
    "}\n"
    "\n"
    "__kernel void opencl_copy(__global uchar *vram, int dstaddr, int srcaddr, int dstpitch, int srcpitch, int width, int height, int rop)\n"
    "{\n"
    "    int x = get_global_id(0);\n"
    "    int y = get_global_id(1);\n"
    "    if (x >= width || y >= height) return;\n"
    "    int doff = dstaddr + y * dstpitch + x;\n"
    "    int soff = srcaddr + y * srcpitch + x;\n"
    "    uchar s = vram[soff];\n"
    "    uchar d = vram[doff];\n"
    "    uchar r = s;\n"
    "    switch (rop) {\n"
    "    case 0: r = 0; break;\n"
    "    case 1: r = s & d; break;\n"
    "    case 2: r = d; break;\n"
    "    case 3: r = s & ~d; break;\n"
    "    case 4: r = ~d; break;\n"
    "    case 5: r = s; break;\n"
    "    case 6: r = 0xff; break;\n"
    "    case 7: r = ~s & d; break;\n"
    "    case 8: r = s ^ d; break;\n"
    "    case 9: r = s | d; break;\n"
    "    case 10: r = ~s | ~d; break;\n"
    "    case 11: r = ~(s ^ d); break;\n"
    "    case 12: r = s | ~d; break;\n"
    "    case 13: r = ~s; break;\n"
    "    case 14: r = ~s | d; break;\n"
    "    case 15: r = ~s & ~d; break;\n"
    "    }\n"
    "    vram[doff] = r;\n"
    "}\n";

bool cirrus_opencl_init(void)
{
    if (s_opencl_initialized) {
        return s_opencl_available;
    }
    s_opencl_initialized = true;

    /* 1. Try loading libOpenCL.so.1 / libOpenCL.so */
    s_lib_handle = dlopen("libOpenCL.so.1", RTLD_NOW);
    if (!s_lib_handle) {
        s_lib_handle = dlopen("libOpenCL.so", RTLD_NOW);
    }
    if (!s_lib_handle) {
        fprintf(stderr, "[Cirrus OpenCL] OpenCL runtime not found. Using CPU acceleration fallback.\n");
        return false;
    }

    /* 2. Resolve OpenCL symbols */
#define LOAD_SYM(name) do { \
        cl_##name = (pfn_cl##name)dlsym(s_lib_handle, "cl" #name); \
        if (!cl_##name) { \
            fprintf(stderr, "[Cirrus OpenCL] Symbol cl" #name " not found.\n"); \
            dlclose(s_lib_handle); \
            s_lib_handle = NULL; \
            return false; \
        } \
    } while (0)

    LOAD_SYM(GetPlatformIDs);
    LOAD_SYM(GetDeviceIDs);
    LOAD_SYM(CreateContext);
    LOAD_SYM(CreateCommandQueue);
    LOAD_SYM(CreateProgramWithSource);
    LOAD_SYM(BuildProgram);
    LOAD_SYM(CreateKernel);
    LOAD_SYM(CreateBuffer);
    LOAD_SYM(SetKernelArg);
    LOAD_SYM(EnqueueWriteBuffer);
    LOAD_SYM(EnqueueReadBuffer);
    LOAD_SYM(EnqueueNDRangeKernel);
    LOAD_SYM(Finish);
    LOAD_SYM(ReleaseMemObject);
    LOAD_SYM(ReleaseKernel);
    LOAD_SYM(ReleaseProgram);
    LOAD_SYM(ReleaseCommandQueue);
    LOAD_SYM(ReleaseContext);
#undef LOAD_SYM

    /* 3. Query platforms */
    cl_uint num_platforms = 0;
    cl_int err = cl_GetPlatformIDs(1, &s_platform, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0 || !s_platform) {
        fprintf(stderr, "[Cirrus OpenCL] No OpenCL platform / ICD driver found. Using CPU fallback.\n");
        return false;
    }

    /* 4. Query devices (prefer GPU, then CPU) */
    cl_uint num_devices = 0;
    err = cl_GetDeviceIDs(s_platform, CL_DEVICE_TYPE_GPU, 1, &s_device, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        err = cl_GetDeviceIDs(s_platform, CL_DEVICE_TYPE_ALL, 1, &s_device, &num_devices);
    }
    if (err != CL_SUCCESS || num_devices == 0 || !s_device) {
        fprintf(stderr, "[Cirrus OpenCL] No compatible OpenCL device found. Using CPU fallback.\n");
        return false;
    }

    /* 5. Create context and queue */
    s_context = cl_CreateContext(NULL, 1, &s_device, NULL, NULL, &err);
    if (err != CL_SUCCESS || !s_context) {
        fprintf(stderr, "[Cirrus OpenCL] Failed to create OpenCL context.\n");
        return false;
    }

    s_queue = cl_CreateCommandQueue(s_context, s_device, 0, &err);
    if (err != CL_SUCCESS || !s_queue) {
        fprintf(stderr, "[Cirrus OpenCL] Failed to create OpenCL command queue.\n");
        return false;
    }

    /* 6. Build Program */
    size_t src_len = strlen(s_opencl_src);
    s_program = cl_CreateProgramWithSource(s_context, 1, &s_opencl_src, &src_len, &err);
    if (err != CL_SUCCESS || !s_program) {
        fprintf(stderr, "[Cirrus OpenCL] Failed to create OpenCL program.\n");
        return false;
    }

    err = cl_BuildProgram(s_program, 1, &s_device, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[Cirrus OpenCL] OpenCL kernel build failed.\n");
        return false;
    }

    s_kernel_fill = cl_CreateKernel(s_program, "opencl_solid_fill", &err);
    s_kernel_copy = cl_CreateKernel(s_program, "opencl_copy", &err);

    if (!s_kernel_fill || !s_kernel_copy) {
        fprintf(stderr, "[Cirrus OpenCL] OpenCL kernel creation failed.\n");
        return false;
    }

    s_opencl_available = true;
    fprintf(stderr, "[Cirrus OpenCL] 2D Hardware Acceleration successfully enabled!\n");
    return true;
}

bool cirrus_opencl_available(void)
{
    return s_opencl_available;
}

void cirrus_opencl_cleanup(void)
{
    if (!s_opencl_available) return;
    if (s_kernel_fill) cl_ReleaseKernel(s_kernel_fill);
    if (s_kernel_copy) cl_ReleaseKernel(s_kernel_copy);
    if (s_program) cl_ReleaseProgram(s_program);
    if (s_queue) cl_ReleaseCommandQueue(s_queue);
    if (s_context) cl_ReleaseContext(s_context);
    if (s_lib_handle) dlclose(s_lib_handle);
    s_opencl_available = false;
    s_opencl_initialized = false;
}

bool cirrus_opencl_solid_fill(CirrusVGAState *s,
                              uint32_t dstaddr, int dstpitch,
                              int width, int height,
                              uint32_t color, int pixelwidth, int rop)
{
    if (!s_opencl_available || !s || !s->vga || !s->vga->vga_ram) return false;
    if (rop != 0x0d && rop != 0x00 && rop != 0x0e) return false; // fast path for common fills

    int pixels_w = width / (pixelwidth > 0 ? pixelwidth : 1);
    if (pixels_w <= 0 || height <= 0) return false;

    // Fast threshold: small fills (< 1024 pixels) are faster on CPU due to driver launch overhead
    if (pixels_w * height < 1024) return false;

    cl_int err;
    size_t span = (size_t)dstpitch * height;
    if (dstaddr + span > (size_t)s->real_vram_size) return false;

    cl_mem buf = cl_CreateBuffer(s_context, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                                 s->real_vram_size, s->vga->vga_ram, &err);
    if (err != CL_SUCCESS || !buf) return false;

    cl_SetKernelArg(s_kernel_fill, 0, sizeof(cl_mem), &buf);
    cl_SetKernelArg(s_kernel_fill, 1, sizeof(int), &dstaddr);
    cl_SetKernelArg(s_kernel_fill, 2, sizeof(int), &dstpitch);
    cl_SetKernelArg(s_kernel_fill, 3, sizeof(int), &pixels_w);
    cl_SetKernelArg(s_kernel_fill, 4, sizeof(int), &height);
    uint32_t actual_color = (rop == 0x00) ? 0 : ((rop == 0x0e) ? 0xffffffff : color);
    cl_SetKernelArg(s_kernel_fill, 5, sizeof(uint32_t), &actual_color);
    cl_SetKernelArg(s_kernel_fill, 6, sizeof(int), &pixelwidth);

    size_t global_work_size[2] = { (size_t)pixels_w, (size_t)height };
    err = cl_EnqueueNDRangeKernel(s_queue, s_kernel_fill, 2, NULL, global_work_size, NULL, 0, NULL, NULL);
    if (err == CL_SUCCESS) {
        cl_Finish(s_queue);
    }
    cl_ReleaseMemObject(buf);
    return (err == CL_SUCCESS);
}

bool cirrus_opencl_copy(CirrusVGAState *s,
                        uint32_t dstaddr, uint32_t srcaddr,
                        int dstpitch, int srcpitch,
                        int width, int height,
                        bool is_forward, bool is_transp,
                        int rop, int pixelwidth)
{
    if (!s_opencl_available || !s || !s->vga || !s->vga->vga_ram) return false;
    if (is_transp || !is_forward) return false;
    if (width * height < 2048) return false; // use CPU for small copy

    cl_int err;
    cl_mem buf = cl_CreateBuffer(s_context, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                                 s->real_vram_size, s->vga->vga_ram, &err);
    if (err != CL_SUCCESS || !buf) return false;

    cl_SetKernelArg(s_kernel_copy, 0, sizeof(cl_mem), &buf);
    cl_SetKernelArg(s_kernel_copy, 1, sizeof(int), &dstaddr);
    cl_SetKernelArg(s_kernel_copy, 2, sizeof(int), &srcaddr);
    cl_SetKernelArg(s_kernel_copy, 3, sizeof(int), &dstpitch);
    cl_SetKernelArg(s_kernel_copy, 4, sizeof(int), &srcpitch);
    cl_SetKernelArg(s_kernel_copy, 5, sizeof(int), &width);
    cl_SetKernelArg(s_kernel_copy, 6, sizeof(int), &height);
    cl_SetKernelArg(s_kernel_copy, 7, sizeof(int), &rop);

    size_t global_work_size[2] = { (size_t)width, (size_t)height };
    err = cl_EnqueueNDRangeKernel(s_queue, s_kernel_copy, 2, NULL, global_work_size, NULL, 0, NULL, NULL);
    if (err == CL_SUCCESS) {
        cl_Finish(s_queue);
    }
    cl_ReleaseMemObject(buf);
    return (err == CL_SUCCESS);
}

#else

/* Non-x86 / Embedded Fallback */
bool cirrus_opencl_init(void) { return false; }
bool cirrus_opencl_available(void) { return false; }
void cirrus_opencl_cleanup(void) {}
bool cirrus_opencl_solid_fill(CirrusVGAState *s, uint32_t dstaddr, int dstpitch, int width, int height, uint32_t color, int pixelwidth, int rop)
{
    (void)s; (void)dstaddr; (void)dstpitch; (void)width; (void)height; (void)color; (void)pixelwidth; (void)rop;
    return false;
}
bool cirrus_opencl_copy(CirrusVGAState *s, uint32_t dstaddr, uint32_t srcaddr, int dstpitch, int srcpitch, int width, int height, bool is_forward, bool is_transp, int rop, int pixelwidth)
{
    (void)s; (void)dstaddr; (void)srcaddr; (void)dstpitch; (void)srcpitch; (void)width; (void)height; (void)is_forward; (void)is_transp; (void)rop; (void)pixelwidth;
    return false;
}

#endif
