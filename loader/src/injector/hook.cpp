#include <dlfcn.h>
#include <sys/mount.h>
#include <unistd.h>
#include <unwind.h>

#include <lsplt.hpp>
#include <span>

#include "art_method.hpp"
#include "jni_helper.hpp"
#include "logging.h"
#include "misc.hpp"
#include "module.hpp"
#include "zygisk.hpp"

using namespace std;

// *********************
// Zygisk Bootstrapping
// *********************
//
// Zygisk's lifecycle is driven by several PLT function hooks in libandroid_runtime, libart, and
// libnative_bridge. As Zygote is starting up, these carefully selected functions will call into
// the respective lifecycle callbacks in Zygisk to drive the progress forward.
//
// The entire bootstrap process is shown in the graph below.
// Arrows represent control flow, and the blocks are sorted chronologically from top to bottom.
//
//       libandroid_runtime                zygisk                 libart
//
//           ┌───────┐                 ┌─────────────┐
//           │ start │                 │ remote_call │
//           └───┬───┘                 └──────┬──────┘
//               │                            │
//               │                            ▼
//               │                        ┌────────┐
//               │                        │hook_plt│
//               │                        └────────┘
//               ▼
//   ┌──────────────────────┐
//   │ strdup("ZygoteInit") │
//   └───────────┬────┬─────┘
//               │    │                ┌───────────────┐
//               │    └───────────────►│hook_zygote_jni│
//               │                     └───────────────┘       ┌─────────┐
//               │                                             │         │
//               └────────────────────────────────────────────►│   JVM   │
//                                                             │         │
//                                                             └──┬─┬────┘
//     ┌───────────────────┐                                      │ │
//     │nativeXXXSpecialize│◄─────────────────────────────────────┘ │
//     └─────────────┬─────┘                                        │
//                   │                 ┌─────────────┐              │
//                   └────────────────►│ZygiskContext│              │
//                                     └─────────────┘              ▼
//                                                        ┌────────────────────┐
//                                                        │pthread_attr_destroy│
//                                                        └─────────┬──────────┘
//                                    ┌────────────────┐            │
//                                    │restore_plt_hook│◄───────────┘
//                                    └────────────────┘
//
// Some notes regarding the important functions/symbols during bootstrap:
//
// * HookContext::hook_plt(): hook functions like |unshare| and |strdup|
// * strdup: called in AndroidRuntime::start before calling ZygoteInit#main(...)
// * HookContext::hook_zygote_jni(): replace the process specialization functions registered
//   with register_jni_procs. This marks the final step of the code injection bootstrap process.
// * pthread_attr_destroy: called whenever the JVM tries to setup threads for itself. We use
//   this method to cleanup and unload Zygisk from the process.

constexpr const char *kZygoteInit = "com.android.internal.os.ZygoteInit";
constexpr const char *kZygote = "com/android/internal/os/Zygote";

// Global contexts:
//
// HookContext lives as long as Zygisk is loaded in memory. It tracks the process's function
// hooking state and bootstraps code injection until we replace the process specialization methods.
//
// ZygiskContext lives during the process specialization process. It implements Zygisk
// features, such as loading modules and customizing process fork/specialization.

ZygiskContext *g_ctx;
struct HookContext;
static HookContext *g_hook;

using JNIMethods = std::span<JNINativeMethod>;

struct HookContext {
#include "jni_hooks.hpp"

    // std::array<JNINativeMethod> zygote_methods
    vector<tuple<dev_t, ino_t, const char *, void **>> plt_backup;
    void *self_handle = nullptr;
    bool should_unmap = false;
    jint MODIFIER_NATIVE = 0;
    jmethodID member_getModifiers = nullptr;

    void hook_plt();
    void hook_unloader();
    void restore_plt_hook();
    void hook_zygote_jni();
    void restore_zygote_hook(JNIEnv *env);
    void hook_jni_methods(JNIEnv *env, const char *clz, JNIMethods methods);

private:
    void register_hook(dev_t dev, ino_t inode, const char *symbol, void *new_func, void **old_func);
};

// -----------------------------------------------------------------

#define DCL_HOOK_FUNC(ret, func, ...)                                                              \
    ret (*old_##func)(__VA_ARGS__);                                                                \
    ret new_##func(__VA_ARGS__)

DCL_HOOK_FUNC(static char *, strdup, const char *str) {
    if (strcmp(kZygoteInit, str) == 0) {
        g_hook->hook_zygote_jni();
    }
    return old_strdup(str);
}

// Skip actual fork and return cached result if applicable
DCL_HOOK_FUNC(int, fork) { return (g_ctx && g_ctx->pid >= 0) ? g_ctx->pid : old_fork(); }

// Unmount stuffs in the process's private mount namespace
DCL_HOOK_FUNC(static int, unshare, int flags) {
    int res = old_unshare(flags);
    if (g_ctx && (flags & CLONE_NEWNS) != 0 && res == 0 &&
        // For some unknown reason, unmounting app_process in SysUI can break.
        // This is reproducible on the official AVD running API 26 and 27.
        // Simply avoid doing any unmounts for SysUI to avoid potential issues.
        (g_ctx->info_flags & PROCESS_IS_SYS_UI) == 0) {
        if (g_ctx->flags & DO_REVERT_UNMOUNT) {
            if (g_ctx->info_flags & PROCESS_ROOT_IS_KSU) {
                revert_unmount_ksu();
            } else if (g_ctx->info_flags & PROCESS_ROOT_IS_MAGISK) {
                revert_unmount_magisk();
            }
        }
        // Restore errno back to 0
        errno = 0;
    }
    return res;
}

// This is the last moment before the secontext of the process changes
DCL_HOOK_FUNC(static int, selinux_android_setcontext, uid_t uid, bool isSystemServer,
              const char *seinfo, const char *pkgname) {
    // Pre-fetch logd before secontext transition
    // TODO: zygisk_get_logd();
    return old_selinux_android_setcontext(uid, isSystemServer, seinfo, pkgname);
}

// Close file descriptors to prevent crashing
DCL_HOOK_FUNC(static void, android_log_close) {
    if (g_ctx == nullptr || !(g_ctx->flags & SKIP_CLOSE_LOG_PIPE)) {
        // This happens during forks like nativeForkApp, nativeForkUsap,
        // nativeForkSystemServer, and nativeForkAndSpecialize.
        // TODO: zygisk_close_logd();
        logging::setfd(-1);
    }
    old_android_log_close();
}

// We cannot directly call `dlclose` to unload ourselves, otherwise when `dlclose` returns,
// it will return to our code which has been unmapped, causing segmentation fault.
// Instead, we hook `pthread_attr_destroy` which will be called when VM daemon threads start.
DCL_HOOK_FUNC(static int, pthread_attr_destroy, void *target) {
    int res = old_pthread_attr_destroy((pthread_attr_t *) target);

    // Only perform unloading on the main thread
    if (gettid() != getpid()) return res;

    LOGV("pthread_attr_destroy\n");
    if (g_hook->should_unmap) {
        g_hook->restore_plt_hook();
        if (g_hook->should_unmap) {
            LOGV("dlclosing self\n");
            void *self_handle = g_hook->self_handle;
            delete g_hook;

            // Because both `pthread_attr_destroy` and `dlclose` have the same function signature,
            // we can use `musttail` to let the compiler reuse our stack frame and thus
            // `dlclose` will directly return to the caller of `pthread_attr_destroy`.
            [[clang::musttail]] return dlclose(self_handle);
        }
    }

    delete g_hook;
    return res;
}

#undef DCL_HOOK_FUNC

// -----------------------------------------------------------------

ZygiskContext::ZygiskContext(JNIEnv *env, void *args)
    : env(env),
      args{args},
      process(nullptr),
      pid(-1),
      flags(0),
      info_flags(0),
      hook_info_lock(PTHREAD_MUTEX_INITIALIZER) {
    g_ctx = this;
}

ZygiskContext::~ZygiskContext() {
    // This global pointer points to a variable on the stack.
    // Set this to nullptr to prevent leaking local variable.
    // This also disables most plt hooked functions.
    g_ctx = nullptr;

    if (!is_child()) return;

    // TODO: zygisk_close_logd();
    // TODO: android_logging();

    // Strip out all API function pointers
    for (auto &m : modules) {
        m.clearApi();
    }

    // Cleanup
    g_hook->should_unmap = true;
    g_hook->restore_zygote_hook(env);
    g_hook->hook_unloader();
}

// -----------------------------------------------------------------

inline void *unwind_get_region_start(_Unwind_Context *ctx) {
    auto fp = _Unwind_GetRegionStart(ctx);
#if defined(__arm__)
    // On arm32, we need to check if the pc is in thumb mode,
    // if so, we need to set the lowest bit of fp to 1
    auto pc = _Unwind_GetGR(ctx, 15);  // r15 is pc
    if (pc & 1) {
        // Thumb mode
        fp |= 1;
    }
#endif
    return reinterpret_cast<void *>(fp);
}

// -----------------------------------------------------------------

void HookContext::register_hook(dev_t dev, ino_t inode, const char *symbol, void *new_func,
                                void **old_func) {
    if (!lsplt::RegisterHook(dev, inode, symbol, new_func, old_func)) {
        LOGE("Failed to register plt_hook \"%s\"\n", symbol);
        return;
    }
    plt_backup.emplace_back(dev, inode, symbol, old_func);
}

#define PLT_HOOK_REGISTER_SYM(DEV, INODE, SYM, NAME)                                               \
    register_hook(DEV, INODE, SYM, reinterpret_cast<void *>(new_##NAME),                           \
                  reinterpret_cast<void **>(&old_##NAME))

#define PLT_HOOK_REGISTER(DEV, INODE, NAME) PLT_HOOK_REGISTER_SYM(DEV, INODE, #NAME, NAME)

void HookContext::hook_plt() {
    ino_t android_runtime_inode = 0;
    dev_t android_runtime_dev = 0;

    for (auto &map : lsplt::MapInfo::Scan()) {
        if (map.path.ends_with("/libandroid_runtime.so")) {
            android_runtime_inode = map.inode;
            android_runtime_dev = map.dev;
        }
    }

    PLT_HOOK_REGISTER(android_runtime_dev, android_runtime_inode, fork);
    PLT_HOOK_REGISTER(android_runtime_dev, android_runtime_inode, unshare);
    PLT_HOOK_REGISTER(android_runtime_dev, android_runtime_inode, selinux_android_setcontext);
    PLT_HOOK_REGISTER(android_runtime_dev, android_runtime_inode, strdup);
    PLT_HOOK_REGISTER_SYM(android_runtime_dev, android_runtime_inode, "__android_log_close",
                          android_log_close);

    if (!lsplt::CommitHook()) LOGE("plt_hook failed\n");

    // Remove unhooked methods
    plt_backup.erase(std::remove_if(plt_backup.begin(), plt_backup.end(),
                                    [](auto &t) { return *std::get<3>(t) == nullptr; }),
                     plt_backup.end());
}

void HookContext::hook_unloader() {
    ino_t art_inode = 0;
    dev_t art_dev = 0;

    for (auto &map : lsplt::MapInfo::Scan()) {
        if (map.path.ends_with("/libart.so")) {
            art_inode = map.inode;
            art_dev = map.dev;
            break;
        }
    }

    PLT_HOOK_REGISTER(art_dev, art_inode, pthread_attr_destroy);
    if (!lsplt::CommitHook()) LOGE("plt_hook failed\n");
}

void HookContext::restore_plt_hook() {
    // Unhook plt_hook
    for (const auto &[dev, inode, sym, old_func] : plt_backup) {
        if (!lsplt::RegisterHook(dev, inode, sym, *old_func, nullptr)) {
            LOGE("Failed to register plt_hook [%s]\n", sym);
            should_unmap = false;
        }
    }
    if (!lsplt::CommitHook()) {
        LOGE("Failed to restore plt_hook\n");
        should_unmap = false;
    }
}

// -----------------------------------------------------------------

void HookContext::hook_jni_methods(JNIEnv *env, const char *clz, JNIMethods methods) {
    auto clazz = env->FindClass(clz);
    if (clazz == nullptr) {
        env->ExceptionClear();
        for (auto &method : methods) {
            method.fnPtr = nullptr;
        }
        return;
    }

    vector<JNINativeMethod> hooks;
    for (auto &native_method : methods) {
        // It's useful to allow nullptr function pointer for restoring hook
        if (!native_method.fnPtr) continue;

        auto method_id = env->GetMethodID(clazz, native_method.name, native_method.signature);
        bool is_static = false;
        if (method_id == nullptr) {
            env->ExceptionClear();
            method_id = env->GetStaticMethodID(clazz, native_method.name, native_method.signature);
            is_static = true;
        }
        if (method_id == nullptr) {
            env->ExceptionClear();
            native_method.fnPtr = nullptr;
            continue;
        }
        auto method = lsplant::JNI_ToReflectedMethod(env, clazz, method_id, is_static);
        auto modifier = lsplant::JNI_CallIntMethod(env, method, member_getModifiers);
        if ((modifier & MODIFIER_NATIVE) == 0) {
            native_method.fnPtr = nullptr;
            continue;
        }
        auto artMethod = lsplant::art::ArtMethod::FromReflectedMethod(env, method);
        hooks.push_back(native_method);
        auto original_method = artMethod->GetData();
        LOGV("replaced %s %s orig %p", clz, native_method.name, original_method);
        native_method.fnPtr = original_method;
    }

    if (hooks.empty()) return;
    env->RegisterNatives(clazz, hooks.data(), hooks.size());
}

void HookContext::hook_zygote_jni() {
    auto get_created_java_vms = reinterpret_cast<jint (*)(JavaVM **, jsize, jsize *)>(
        dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!get_created_java_vms) {
        for (auto &map : lsplt::MapInfo::Scan()) {
            if (!map.path.ends_with("/libnativehelper.so")) continue;
            void *h = dlopen(map.path.data(), RTLD_LAZY);
            if (!h) {
                LOGW("cannot dlopen libnativehelper.so: %s\n", dlerror());
                break;
            }
            get_created_java_vms =
                reinterpret_cast<decltype(get_created_java_vms)>(dlsym(h, "JNI_GetCreatedJavaVMs"));
            dlclose(h);
            break;
        }
        if (!get_created_java_vms) {
            LOGW("JNI_GetCreatedJavaVMs not found\n");
            return;
        }
    }
    JavaVM *vm = nullptr;
    jsize num = 0;
    jint res = get_created_java_vms(&vm, 1, &num);
    if (res != JNI_OK || vm == nullptr) return;
    JNIEnv *env = nullptr;
    res = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (res != JNI_OK || env == nullptr) return;

    auto classMember = lsplant::JNI_FindClass(env, "java/lang/reflect/Member");
    if (classMember != nullptr)
        member_getModifiers = lsplant::JNI_GetMethodID(env, classMember, "getModifiers", "()I");
    auto classModifier = lsplant::JNI_FindClass(env, "java/lang/reflect/Modifier");
    if (classModifier != nullptr) {
        auto fieldId = lsplant::JNI_GetStaticFieldID(env, classModifier, "NATIVE", "I");
        if (fieldId != nullptr)
            MODIFIER_NATIVE = lsplant::JNI_GetStaticIntField(env, classModifier, fieldId);
    }
    if (member_getModifiers == nullptr || MODIFIER_NATIVE == 0) return;
    if (!lsplant::art::ArtMethod::Init(env)) {
        LOGE("failed to init ArtMethod");
        return;
    }
    hook_jni_methods(env, kZygote, zygote_methods);
}

void HookContext::restore_zygote_hook(JNIEnv *env) {
    hook_jni_methods(env, kZygote, zygote_methods);
}

// -----------------------------------------------------------------

void hook_entry(void *handle) {
    default_new(g_hook);
    g_hook->self_handle = handle;
    g_hook->hook_plt();
}

void hookJniNativeMethods(JNIEnv *env, const char *clz, JNINativeMethod *methods, int numMethods) {
    g_hook->hook_jni_methods(env, clz, {methods, (size_t) numMethods});
}
