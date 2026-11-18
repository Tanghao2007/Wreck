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

typedef void (*AdaptiveSliceFunc)(void* user_data, int slice_idx, int total_slices, int start_unit, int end_unit);

typedef struct AdaptiveThreadPool AdaptiveThreadPool;

typedef struct {
    AdaptiveThreadPool* pool;
    int worker_id;
} AdaptiveWorkerContext;

struct AdaptiveThreadPool {
    int thread_count;
    int stop;
    int work_id;
    int completed_workers;
    AdaptiveSliceFunc func;
    void* user_data;
    int total_units;

    AdaptiveWorkerContext workers[16];

#if defined(_WIN32)
    HANDLE threads[16];
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv_work;
    CONDITION_VARIABLE cv_done;
#else
    pthread_t threads[16];
    pthread_mutex_t lock;
    pthread_cond_t cv_work;
    pthread_cond_t cv_done;
#endif
};

#if defined(_WIN32)
static unsigned __stdcall win32_worker_proc(void* arg) {
    AdaptiveWorkerContext* ctx = (AdaptiveWorkerContext*)arg;
    AdaptiveThreadPool* pool = ctx->pool;
    int worker_id = ctx->worker_id;
    int my_last_work_id = 0;

    while (1) {
        EnterCriticalSection(&pool->lock);
        while (!pool->stop && pool->work_id == my_last_work_id) {
            SleepConditionVariableCS(&pool->cv_work, &pool->lock, INFINITE);
        }
        if (pool->stop) {
            LeaveCriticalSection(&pool->lock);
            break;
        }

        my_last_work_id = pool->work_id;
        AdaptiveSliceFunc fn = pool->func;
        void* ud = pool->user_data;
        int total = pool->total_units;
        int count = pool->thread_count;
        LeaveCriticalSection(&pool->lock);

        int start = (worker_id * total) / count;
        int end = ((worker_id + 1) * total) / count;
        if (fn && start < end) {
            fn(ud, worker_id, count, start, end);
        }

        EnterCriticalSection(&pool->lock);
        pool->completed_workers++;
        if (pool->completed_workers == (pool->thread_count - 1)) {
            WakeConditionVariable(&pool->cv_done);
        }
        LeaveCriticalSection(&pool->lock);
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
        pthread_mutex_lock(&pool->lock);
        while (!pool->stop && pool->work_id == my_last_work_id) {
            pthread_cond_wait(&pool->cv_work, &pool->lock);
        }
        if (pool->stop) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        my_last_work_id = pool->work_id;
        AdaptiveSliceFunc fn = pool->func;
        void* ud = pool->user_data;
        int total = pool->total_units;
        int count = pool->thread_count;
        pthread_mutex_unlock(&pool->lock);

        int start = (worker_id * total) / count;
        int end = ((worker_id + 1) * total) / count;
        if (fn && start < end) {
            fn(ud, worker_id, count, start, end);
        }

        pthread_mutex_lock(&pool->lock);
        pool->completed_workers++;
        if (pool->completed_workers == (pool->thread_count - 1)) {
            pthread_cond_signal(&pool->cv_done);
        }
        pthread_mutex_unlock(&pool->lock);
    }
    return NULL;
}
#endif

AdaptiveThreadPool* adaptive_thread_pool_create(int thread_count) {
    if (thread_count < 1) thread_count = 1;
    if (thread_count > 16) thread_count = 16;

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
    InitializeCriticalSection(&pool->lock);
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
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cv_work, NULL);
    pthread_cond_init(&pool->cv_done, NULL);

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
        EnterCriticalSection(&pool->lock);
        pool->stop = 1;
        WakeAllConditionVariable(&pool->cv_work);
        LeaveCriticalSection(&pool->lock);

        for (int i = 1; i < pool->thread_count; i++) {
            if (pool->threads[i]) {
                WaitForSingleObject(pool->threads[i], INFINITE);
                CloseHandle(pool->threads[i]);
            }
        }
        DeleteCriticalSection(&pool->lock);
#else
        pthread_mutex_lock(&pool->lock);
        pool->stop = 1;
        pthread_cond_broadcast(&pool->cv_work);
        pthread_mutex_unlock(&pool->lock);

        for (int i = 1; i < pool->thread_count; i++) {
            pthread_join(pool->threads[i], NULL);
        }
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->cv_work);
        pthread_cond_destroy(&pool->cv_done);
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
    EnterCriticalSection(&pool->lock);
    pool->func = func;
    pool->user_data = user_data;
    pool->total_units = total_units;
    pool->completed_workers = 0;
    pool->work_id++;
    WakeAllConditionVariable(&pool->cv_work);
    LeaveCriticalSection(&pool->lock);

    /* Thread 0 执行第 0 块切片 */
    int start0 = 0;
    int end0 = total_units / count;
    func(user_data, 0, count, start0, end0);

    /* 等待其他工作线程完成 */
    EnterCriticalSection(&pool->lock);
    while (pool->completed_workers < (count - 1)) {
        SleepConditionVariableCS(&pool->cv_done, &pool->lock, INFINITE);
    }
    LeaveCriticalSection(&pool->lock);
#else
    pthread_mutex_lock(&pool->lock);
    pool->func = func;
    pool->user_data = user_data;
    pool->total_units = total_units;
    pool->completed_workers = 0;
    pool->work_id++;
    pthread_cond_broadcast(&pool->cv_work);
    pthread_mutex_unlock(&pool->lock);

    /* Thread 0 执行第 0 块切片 */
    int start0 = 0;
    int end0 = total_units / count;
    func(user_data, 0, count, start0, end0);

    /* 等待其他工作线程完成 */
    pthread_mutex_lock(&pool->lock);
    while (pool->completed_workers < (count - 1)) {
        pthread_cond_wait(&pool->cv_done, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
#endif
}

/* ========================================================================== */
/* 测试用例                                                                   */
/* ========================================================================== */

static void sample_slice_task(void* user_data, int slice_idx, int total_slices, int start_unit, int end_unit) {
    int* array = (int*)user_data;
    for (int i = start_unit; i < end_unit; i++) {
        array[i] = (slice_idx + 1) * 1000 + i;
    }
}

int main() {
    printf("[ThreadPool Test] Starting verification...\n");
    const int N = 720;
    int* data = (int*)malloc(N * sizeof(int));

    /* 测试 1: 4 线程并发 */
    AdaptiveThreadPool* pool4 = adaptive_thread_pool_create(4);
    if (!pool4) {
        printf("FAILED to create pool 4\n");
        return 1;
    }

    memset(data, 0, N * sizeof(int));
    for (int iter = 0; iter < 100; iter++) {
        adaptive_thread_pool_dispatch(pool4, N, sample_slice_task, data);
    }

    /* 验证结果完整性与边界 */
    int errors = 0;
    for (int i = 0; i < N; i++) {
        int expected_slice = (i * 4) / N;
        int expected_val = (expected_slice + 1) * 1000 + i;
        if (data[i] != expected_val) {
            printf("Error at index %d: got %d, expected %d\n", i, data[i], expected_val);
            errors++;
            if (errors > 10) break;
        }
    }

    adaptive_thread_pool_destroy(pool4);

    if (errors == 0) {
        printf("[ThreadPool Test] 100 iterations on 4 threads: 100%% PASS!\n");
    } else {
        printf("[ThreadPool Test] FAILED with %d errors!\n", errors);
        return 1;
    }

    free(data);
    return 0;
}
