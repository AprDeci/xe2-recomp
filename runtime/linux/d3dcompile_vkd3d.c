/* D3DCompile without Windows: HLSL to DXBC through vkd3d-shader.
 *
 * On Linux the renderer's Direct3D 11 is DXVK, which takes DXBC like the real thing but has
 * no HLSL compiler. vkd3d-shader (Wine's) compiles HLSL to the same Shader Model 5 bytecode;
 * this file gives it D3DCompile's shape, blobs included, so the renderer's calls stay as they
 * are. Built on Windows too (as rt_vkd3d_compile) to check its output against fxc's there. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif
#include <d3dcompiler.h>
#include <vkd3d_shader.h>

typedef struct Blob { ID3D10Blob iface; volatile long refs; size_t size; unsigned char data[]; } Blob;

static HRESULT STDMETHODCALLTYPE blob_query(ID3D10Blob *self, REFIID iid, void **out) {
    (void)iid;
    *out = self;
    self->lpVtbl->AddRef(self);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE blob_addref(ID3D10Blob *self) { return (ULONG)__atomic_add_fetch(&((Blob *)self)->refs, 1, __ATOMIC_SEQ_CST); }
static ULONG STDMETHODCALLTYPE blob_release(ID3D10Blob *self) {
    long n = __atomic_sub_fetch(&((Blob *)self)->refs, 1, __ATOMIC_SEQ_CST);
    if (!n) free(self);
    return (ULONG)n;
}
static void *STDMETHODCALLTYPE blob_pointer(ID3D10Blob *self) { return ((Blob *)self)->data; }
static SIZE_T STDMETHODCALLTYPE blob_size(ID3D10Blob *self) { return ((Blob *)self)->size; }
static ID3D10BlobVtbl blob_vtbl = {blob_query, blob_addref, blob_release, blob_pointer, blob_size};

static ID3D10Blob *new_blob(const void *data, size_t size) {
    Blob *b = malloc(sizeof *b + size + 1);
    if (!b) return NULL;
    b->iface.lpVtbl = &blob_vtbl;
    b->refs = 1;
    b->size = size;
    memcpy(b->data, data, size);
    b->data[size] = 0;
    return &b->iface;
}

/* What D3DCompile would do with these arguments (macros and includes unused here). Optimisation
 * flags make no difference: vkd3d-shader has one way of compiling, and the driver optimises. */
HRESULT rt_vkd3d_compile(const void *src, SIZE_T size, const char *name, const char *entry, const char *target,
                         UINT flags, ID3DBlob **code, ID3DBlob **errors) {
    (void)flags;
    struct vkd3d_shader_hlsl_source_info hlsl = {VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO};
    hlsl.entry_point = entry;
    hlsl.profile = target;
    struct vkd3d_shader_compile_option options[] = {
        {VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_2_1},
        {VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG, 1},
        {VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY, VKD3D_SHADER_COMPILE_OPTION_CONST_GLOBAL_UNIFORMS},
    };
    struct vkd3d_shader_compile_info info = {VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO};
    info.next = &hlsl;
    info.source.code = src;
    info.source.size = size;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.options = options;
    info.option_count = sizeof options / sizeof *options;
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = name;
    struct vkd3d_shader_code out = {0};
    char *messages = NULL;
    int ret = vkd3d_shader_compile(&info, &out, &messages);
    if (code) *code = NULL;
    if (errors) *errors = messages && *messages ? new_blob(messages, strlen(messages)) : NULL;
    vkd3d_shader_free_messages(messages);
    if (ret < 0) {
        vkd3d_shader_free_shader_code(&out);
        return E_FAIL;
    }
    if (code) *code = new_blob(out.code, out.size);
    vkd3d_shader_free_shader_code(&out);
    return code && !*code ? E_OUTOFMEMORY : S_OK;
}

/* D3DCompile itself on Linux; on Windows, for tools/linux/wintest.py (the Windows build trying
 * vkd3d-shader's output, with D3DCompile renamed to this), rt_vkd3d_D3DCompile. */
#ifdef _WIN32
#define RT_D3DCOMPILE rt_vkd3d_D3DCompile
#else
#define RT_D3DCOMPILE D3DCompile
#endif
HRESULT WINAPI RT_D3DCOMPILE(const void *data, SIZE_T size, const char *name, const D3D_SHADER_MACRO *defines, ID3DInclude *include,
                             const char *entry, const char *target, UINT flags, UINT effect_flags, ID3DBlob **code, ID3DBlob **errors) {
    (void)defines; (void)include; (void)effect_flags;
    return rt_vkd3d_compile(data, size, name, entry, target, flags, code, errors);
}
