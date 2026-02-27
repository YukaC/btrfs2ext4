#include "thread_pool.h"
#include <stdlib.h>

static void *thread_worker(void *arg) {
  struct thread_pool *pool = (struct thread_pool *)arg;

  for (;;) {
    struct thread_task task;

    pthread_mutex_lock(&pool->lock);
    while (pool->count == 0 && !pool->shutdown) {
      pthread_cond_wait(&pool->notify, &pool->lock);
    }

    if (pool->shutdown && pool->count == 0) {
      pthread_mutex_unlock(&pool->lock);
      break;
    }

    task = pool->tasks[pool->head];
    pool->head = (pool->head + 1) % pool->queue_capacity;
    pool->count--;
    pthread_mutex_unlock(&pool->lock);

    /* Execute task */
    if (task.fn) {
      task.fn(task.arg);
    }
    if (task.wg) {
      thread_pool_wg_done(task.wg);
    }
  }
  return NULL;
}

struct thread_pool *thread_pool_create(uint32_t num_threads,
                                       uint32_t queue_capacity) {
  if (num_threads == 0 || queue_capacity == 0)
    return NULL;

  struct thread_pool *pool = calloc(1, sizeof(*pool));
  if (!pool)
    return NULL;

  pool->num_threads = num_threads;
  pool->queue_capacity = queue_capacity;
  pool->tasks = calloc(queue_capacity, sizeof(struct thread_task));
  if (!pool->tasks) {
    free(pool);
    return NULL;
  }

  pthread_mutex_init(&pool->lock, NULL);
  pthread_cond_init(&pool->notify, NULL);

  pool->threads = calloc(num_threads, sizeof(pthread_t));
  for (uint32_t i = 0; i < num_threads; i++) {
    pthread_create(&pool->threads[i], NULL, thread_worker, pool);
  }

  return pool;
}

int thread_pool_submit(struct thread_pool *pool, thread_task_fn fn, void *arg,
                       struct thread_pool_wait_group *wg) {
  if (!pool || !fn)
    return -1;

  pthread_mutex_lock(&pool->lock);
  if (pool->count == pool->queue_capacity || pool->shutdown) {
    pthread_mutex_unlock(&pool->lock);
    return -1;
  }

  pool->tasks[pool->tail].fn = fn;
  pool->tasks[pool->tail].arg = arg;
  pool->tasks[pool->tail].wg = wg;
  pool->tail = (pool->tail + 1) % pool->queue_capacity;
  pool->count++;

  pthread_cond_signal(&pool->notify);
  pthread_mutex_unlock(&pool->lock);

  return 0;
}

void thread_pool_destroy(struct thread_pool *pool) {
  if (!pool)
    return;

  pthread_mutex_lock(&pool->lock);
  pool->shutdown = 1;
  pthread_cond_broadcast(&pool->notify);
  pthread_mutex_unlock(&pool->lock);

  for (uint32_t i = 0; i < pool->num_threads; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  pthread_mutex_destroy(&pool->lock);
  pthread_cond_destroy(&pool->notify);
  free(pool->tasks);
  free(pool->threads);
  free(pool);
}

struct thread_pool_wait_group *thread_pool_wg_create(void) {
  struct thread_pool_wait_group *wg = calloc(1, sizeof(*wg));
  if (wg) {
    pthread_mutex_init(&wg->lock, NULL);
    pthread_cond_init(&wg->cond, NULL);
  }
  return wg;
}

void thread_pool_wg_add(struct thread_pool_wait_group *wg, uint32_t count) {
  if (!wg)
    return;
  pthread_mutex_lock(&wg->lock);
  wg->pending_tasks += count;
  pthread_mutex_unlock(&wg->lock);
}

void thread_pool_wg_done(struct thread_pool_wait_group *wg) {
  if (!wg)
    return;
  pthread_mutex_lock(&wg->lock);
  if (wg->pending_tasks > 0) {
    wg->pending_tasks--;
    if (wg->pending_tasks == 0) {
      pthread_cond_broadcast(&wg->cond);
    }
  }
  pthread_mutex_unlock(&wg->lock);
}

void thread_pool_wg_wait(struct thread_pool_wait_group *wg) {
  if (!wg)
    return;
  pthread_mutex_lock(&wg->lock);
  while (wg->pending_tasks > 0) {
    pthread_cond_wait(&wg->cond, &wg->lock);
  }
  pthread_mutex_unlock(&wg->lock);
}

void thread_pool_wg_destroy(struct thread_pool_wait_group *wg) {
  if (!wg)
    return;
  pthread_mutex_destroy(&wg->lock);
  pthread_cond_destroy(&wg->cond);
  free(wg);
}
