/**
 * @file adaptive_thread_pool.c
 * @brief 极低开销条件变量切片并发工作线程池实现 (基于 POSIX pthread / Win32 事件模型)
 */

#include "adaptive_thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define MAX_POOL_THREADS 16

typedef struct {
    AdaptiveThreadPool* pool;
    int worker_id;
} AdaptiveWorkerContext;

struct AdaptiveThreadPool {
    int thread_count;
    volatile int stop;
    volatile int work_id;
    volatile int completed_workers;
    AdaptiveSliceFunc func;
    void* user_data;
    int total_units;

    AdaptiveWorkerContext workers[MAX_POOL_THREADS];

#if defined(_WIN32)
    HANDLE threads[MAX_POOL_THREADS];
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv_work;
    CONDITION_VARIABLE cv_done;
#else
    pthread_t threads[MAX_POOL_THREADS];
    pthread_mutex_t mutex;
    pthread_cond_t cond_work;
    pthread_cond_t cond_done;
#endif
};

#if defined(_WIN32)
static unsigned __stdcall win32_worker_proc(void* arg) {
    AdaptiveWorkerContext* ctx = (AdaptiveWorkerContext*)arg;
    AdaptiveThreadPool* pool = ctx->pool;
    int worker_id = ctx->worker_id;
    int my_last_work_id = 0;

    while (1) {
        EnterCriticalSection(&pool->cs);
        while (!pool->stop && pool->work_id == my_last_work_id) {
            SleepConditionVariableCS(&pool->cv_work, &pool->cs, INFINITE);
        }
        if (pool->stop) {
            LeaveCriticalSection(&pool->cs);
            break;
        }

        int cur_work_id = pool->work_id;
        AdaptiveSliceFunc fn = pool->func;
        void* ud = pool->user_data;
        int total = pool->total_units;
        int count = pool->thread_count;
        LeaveCriticalSection(&pool->cs);

        my_last_work_id = cur_work_id;
        int start = (worker_id * total) / count;
        int end = ((worker_id + 1) * total) / count;
        if (fn && start < end) {
            fn(ud, worker_id, count, start, end);
        }

        EnterCriticalSection(&pool->cs);
        pool->completed_workers++;
        if (pool->completed_workers >= count - 1) {
            WakeConditionVariable(&pool->cv_done);
        }
        LeaveCriticalSection(&pool->cs);
    }
    return 0;
}
#else
static void* posix_worker_proc(void* arg) {
    AdaptiveWorkerContext* ctx = (AdaptiveWorkerContext*)arg;
    AdaptiveThreadPool* pool = ctx->pool;
    int worker_id = ctx->worker_id;
    int my_last_work_id = 0;

    while (1) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->stop && pool->work_id == my_last_work_id) {
            pthread_cond_wait(&pool->cond_work, &pool->mutex);
        }
        if (pool->stop) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        int cur_work_id = pool->work_id;
        AdaptiveSliceFunc fn = pool->func;
        void* ud = pool->user_data;
        int total = pool->total_units;
        int count = pool->thread_count;
        pthread_mutex_unlock(&pool->mutex);

        my_last_work_id = cur_work_id;
        int start = (worker_id * total) / count;
        int end = ((worker_id + 1) * total) / count;
        if (fn && start < end) {
            fn(ud, worker_id, count, start, end);
        }

        pthread_mutex_lock(&pool->mutex);
        pool->completed_workers++;
        if (pool->completed_workers >= count - 1) {
            pthread_cond_signal(&pool->cond_done);
        }
        pthread_mutex_unlock(&pool->mutex);
    }
    return NULL;
}
#endif

AdaptiveThreadPool* adaptive_thread_pool_create(int thread_count) {
    if (thread_count < 1) thread_count = 1;
    if (thread_count > MAX_POOL_THREADS) thread_count = MAX_POOL_THREADS;

    AdaptiveThreadPool* pool = (AdaptiveThreadPool*)calloc(1, sizeof(AdaptiveThreadPool));
    if (!pool) return NULL;

    pool->thread_count = thread_count;
    pool->stop = 0;
    pool->work_id = 0;
    pool->completed_workers = 0;

    if (thread_count <= 1) {
        return pool;
    }

#if defined(_WIN32)
    InitializeCriticalSection(&pool->cs);
    InitializeConditionVariable(&pool->cv_work);
    InitializeConditionVariable(&pool->cv_done);

    for (int i = 1; i < thread_count; i++) {
        pool->workers[i].pool = pool;
        pool->workers[i].worker_id = i;
        pool->threads[i] = (HANDLE)_beginthreadex(
            NULL, 0, win32_worker_proc, &pool->workers[i], 0, NULL
        );
    }
#else
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond_work, NULL);
    pthread_cond_init(&pool->cond_done, NULL);

    for (int i = 1; i < thread_count; i++) {
        pool->workers[i].pool = pool;
        pool->workers[i].worker_id = i;
        pthread_create(&pool->threads[i], NULL, posix_worker_proc, &pool->workers[i]);
    }
#endif

    return pool;
}

void adaptive_thread_pool_destroy(AdaptiveThreadPool* pool) {
    if (!pool) return;

    if (pool->thread_count > 1) {
#if defined(_WIN32)
        EnterCriticalSection(&pool->cs);
        pool->stop = 1;
        WakeAllConditionVariable(&pool->cv_work);
        LeaveCriticalSection(&pool->cs);

        for (int i = 1; i < pool->thread_count; i++) {
            if (pool->threads[i]) {
                WaitForSingleObject(pool->threads[i], INFINITE);
                CloseHandle(pool->threads[i]);
            }
        }
        DeleteCriticalSection(&pool->cs);
#else
        pthread_mutex_lock(&pool->mutex);
        pool->stop = 1;
        pthread_cond_broadcast(&pool->cond_work);
        pthread_mutex_unlock(&pool->mutex);

        for (int i = 1; i < pool->thread_count; i++) {
            pthread_join(pool->threads[i], NULL);
        }
        pthread_mutex_destroy(&pool->mutex);
        pthread_cond_destroy(&pool->cond_work);
        pthread_cond_destroy(&pool->cond_done);
#endif
    }

    free(pool);
}

void adaptive_thread_pool_dispatch(
    AdaptiveThreadPool* pool,
    int total_units,
    AdaptiveSliceFunc func,
    void* user_data
) {
    if (!pool || !func || total_units <= 0) return;

    if (pool->thread_count <= 1) {
        func(user_data, 0, 1, 0, total_units);
        return;
    }

    int count = pool->thread_count;

#if defined(_WIN32)
    EnterCriticalSection(&pool->cs);
    pool->func = func;
    pool->user_data = user_data;
    pool->total_units = total_units;
    pool->completed_workers = 0;
    pool->work_id++;
    WakeAllConditionVariable(&pool->cv_work);
    LeaveCriticalSection(&pool->cs);

    /* 主线程执行第 0 块切片 */
    int start0 = 0;
    int end0 = total_units / count;
    func(user_data, 0, count, start0, end0);

    /* 等待从工作线程全部完成 */
    EnterCriticalSection(&pool->cs);
    while (pool->completed_workers < (count - 1)) {
        SleepConditionVariableCS(&pool->cv_done, &pool->cs, INFINITE);
    }
    LeaveCriticalSection(&pool->cs);
#else
    pthread_mutex_lock(&pool->mutex);
    pool->func = func;
    pool->user_data = user_data;
    pool->total_units = total_units;
    pool->completed_workers = 0;
    pool->work_id++;
    pthread_cond_broadcast(&pool->cond_work);
    pthread_mutex_unlock(&pool->mutex);

    /* 主线程执行第 0 块切片 */
    int start0 = 0;
    int end0 = total_units / count;
    func(user_data, 0, count, start0, end0);

    /* 等待从工作线程全部完成 */
    pthread_mutex_lock(&pool->mutex);
    while (pool->completed_workers < (count - 1)) {
        pthread_cond_wait(&pool->cond_done, &pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);
#endif
}

int adaptive_thread_pool_get_count(const AdaptiveThreadPool* pool) {
    return pool ? pool->thread_count : 1;
}
