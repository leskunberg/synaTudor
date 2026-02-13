#include <pthread.h>
#include <errno.h>
#include "internal.h"

typedef DWORD __winfnc THREAD_START_ROUTINE(void *param);

#define CREATE_SUSPENDED 0x00000004

struct win_thread {
    struct win_sync_object sync_obj;
    HANDLE handle;

    pthread_mutex_t lock;
    pthread_t thread;
    DWORD thread_id;
    bool has_detached;

    pthread_cond_t suspend_cond;
    int suspend_cntr;

    pthread_cond_t start_cond;
    struct winmodule *start_module;
    THREAD_START_ROUTINE *start_proc;
    void *start_param;
};

static void thread_destr(struct win_thread *thread) {
    //Free memory
    if(!thread->has_detached) cant_fail_ret(pthread_detach(thread->thread));
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));
    cant_fail_ret(pthread_mutex_destroy(&thread->lock));
    cant_fail_ret(pthread_cond_destroy(&thread->suspend_cond));
    free(thread);
}

static DWORD thread_wait(struct win_thread *thread, DWORD timeout) {
    if(thread->has_detached) return 0;

    if(timeout == INFINITE) {
        cant_fail_ret(pthread_join(thread->thread, NULL));
    } else {
        struct timespec time;
        time.tv_nsec = timeout * 10000000L;
        time.tv_sec = timeout / 1000L;
        int err = pthread_timedjoin_np(thread->thread, NULL, &time);
        if(err == ETIMEDOUT) return WAIT_TIMEOUT;
        cant_fail(err);
    }

    thread->has_detached = TRUE;
    return 0;
}

static void thread_wait_resume(struct win_thread *thread) {
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    while(thread->suspend_cntr > 0) cant_fail_ret(pthread_cond_wait(&thread->suspend_cond, &thread->lock));
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));
}

static void *thread_entry(void *arg) {
    struct win_thread *thread = (struct win_thread*) arg;

    cant_fail_ret(pthread_mutex_lock(&thread->lock));

    //Initialize state
    winmodule_set_cur(thread->start_module);
    thread->thread_id = win_get_thread_id();
    win_init_tib();

    //Signal start thread that we've started
    cant_fail_ret(pthread_cond_signal(&thread->start_cond));
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));

    //Call the thread start routine
    thread_wait_resume(thread);
    log_debug("[THREAD %u] start_proc=%p starting", thread->thread_id, thread->start_proc);
    thread->start_proc(thread->start_param);
    log_debug("[THREAD %u] start_proc=%p RETURNED", thread->thread_id, thread->start_proc);

    return (void*) 0;
}

__winfnc HANDLE CreateThread(void *security_attrs, SIZE_T stack_size, THREAD_START_ROUTINE *start_proc, void *param, DWORD flags, DWORD *id) {
    log_debug("CreateThread called: start_proc=%p param=%p flags=0x%x", start_proc, param, flags);
    //Allocate thread
    struct win_thread *thread = (struct win_thread*) malloc(sizeof(struct win_thread));
    if(!thread) { winerr_set_errno(); return NULL; }
    thread->sync_obj.wait_fnc = (win_sync_obj_wait_fnc*) thread_wait;

    cant_fail_ret(pthread_mutex_init(&thread->lock, NULL));
    thread->suspend_cntr = ((flags & CREATE_SUSPENDED) != 0) ? 1 : 0;
    cant_fail_ret(pthread_cond_init(&thread->suspend_cond, NULL));

    cant_fail_ret(pthread_cond_init(&thread->start_cond, NULL));
    thread->start_module = winmodule_get_cur();
    thread->start_proc = start_proc;
    thread->start_param = param;

    thread->handle = winhandle_create(thread, (winhandle_destr_fnc*) thread_destr);

    //Create the actual thread
    thread->has_detached = FALSE;
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    cant_fail_ret(pthread_create(&thread->thread, NULL, thread_entry, thread));
    cant_fail_ret(pthread_cond_wait(&thread->start_cond, &thread->lock));
    cant_fail_ret(pthread_cond_destroy(&thread->start_cond));
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));

    if(id) *id = thread->thread_id;

    return thread->handle;
}
WINAPI(CreateThread)

__winfnc DWORD GetThreadId(HANDLE handle) {
    struct win_thread *thread = (struct win_thread*) handle->data;
    return thread->thread_id;
}
WINAPI(GetThreadId)

__winfnc DWORD ResumeThread(HANDLE handle) {
    struct win_thread *thread = (struct win_thread*) handle->data;
    log_debug("ResumeThread called (thread_id=%u, suspend_cntr=%d)", thread->thread_id, thread->suspend_cntr);

    //Decrement the suspend counter
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    DWORD suspend_ctr = thread->suspend_cntr;
    if(thread->suspend_cntr > 0) thread->suspend_cntr--;
    cant_fail_ret(pthread_cond_signal(&thread->suspend_cond));
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));

    log_debug("ResumeThread returned (prev_suspend_cntr=%u)", suspend_ctr);
    return suspend_ctr;
}
WINAPI(ResumeThread)

__winfnc void ExitThread(DWORD exit_code) {
    log_debug("ExitThread called (exit_code=%u) [ret=%p]", exit_code, __builtin_return_address(0));
    pthread_exit((void*) (uintptr_t) exit_code);
}
WINAPI(ExitThread)

__winfnc BOOL TerminateThread(HANDLE handle, DWORD exit_code) {
    struct win_thread *thread = (struct win_thread*) handle->data;
    log_warn("TerminateThread called (thread_id=%u, exit_code=0x%x)", thread->thread_id, exit_code);
    pthread_cancel(thread->thread);
    return TRUE;
}
WINAPI(TerminateThread)