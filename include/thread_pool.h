#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stdint.h>

typedef void (*thread_task_fn)(void *arg);

struct thread_task {
  thread_task_fn fn;
  void *arg;
  struct thread_pool_wait_group *wg;
};

struct thread_pool_wait_group {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  uint32_t pending_tasks;
};

struct thread_pool {
  pthread_t *threads;
  uint32_t num_threads;

  struct thread_task *tasks;
  uint32_t queue_capacity;
  uint32_t head;
  uint32_t tail;
  uint32_t count;

  pthread_mutex_t lock;
  pthread_cond_t notify;
  int shutdown;
};

struct thread_pool *thread_pool_create(uint32_t num_threads,
                                       uint32_t queue_capacity);
int thread_pool_submit(struct thread_pool *pool, thread_task_fn fn, void *arg,
                       struct thread_pool_wait_group *wg);
void thread_pool_destroy(struct thread_pool *pool);

struct thread_pool_wait_group *thread_pool_wg_create(void);
void thread_pool_wg_add(struct thread_pool_wait_group *wg, uint32_t count);
void thread_pool_wg_done(struct thread_pool_wait_group *wg);
void thread_pool_wg_wait(struct thread_pool_wait_group *wg);
void thread_pool_wg_destroy(struct thread_pool_wait_group *wg);

#endif
