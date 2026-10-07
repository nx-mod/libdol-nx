// UAM, deko3d's shader compiler, linked into the Switch build: GLSL in, a DKSH
// out, in memory (docs/deko3d.md; used by src/platform/gpu/dk_shader_switch.cpp).
//
// The one file compiled with UAM's own include paths and defines, outside the
// runtime's sources. cmake/game/CMakeLists.txt links it with UAM's library into
// a single object and renames every symbol in it but dol_dk_compile_glsl: UAM
// is Mesa's GLSL compiler, and so is the Mesa that NVK links, so linked plainly
// the two collide. The scheme is Halo's deko3d port's (nx-mod/haloce-nx,
// port/switch/host/host_dk_compiler.cpp), which also patches UAM to keep its
// front end alive between compiles.
//
// One compile at a time: nothing else may be inside UAM at once.
#include "compiler_iface.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

// `fragment` 0 for a vertex shader. On success *dksh is a malloc'd DKSH the
// caller frees, *size its bytes.
extern "C" int dol_dk_compile_glsl(int fragment, const char* glsl, char** dksh, size_t* size) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int ok = 0;

    *dksh = nullptr;
    *size = 0;
    pthread_mutex_lock(&lock);
    {
        DekoCompiler compiler{fragment ? pipeline_stage_fragment : pipeline_stage_vertex};

        if (compiler.CompileGlsl(glsl)) {
            FILE* memory = open_memstream(dksh, size);
            if (memory) {
                compiler.WriteDksh(memory);
                ok = !ferror(memory);
                if (fclose(memory) != 0) {
                    ok = 0;
                }
            }
        }
    }
    pthread_mutex_unlock(&lock);
    if (!ok) {
        free(*dksh);
        *dksh = nullptr;
        *size = 0;
    }
    return ok;
}
