#pragma once

#include <jni.h>
#include <sys/types.h>

void hook_entry(void *start_addr, size_t block_size);

bool update_mnt_ns(pid_t pid, bool clean, bool dry_run = false);

void hookJniNativeMethods(JNIEnv *env, const char *clz, JNINativeMethod *methods, int numMethods);

void clean_trace(const char *path, size_t load, size_t unload, bool spoof_maps);
