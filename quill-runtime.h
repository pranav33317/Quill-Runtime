#pragma once
#include <pthread.h>
#include <functional>
#include <atomic>
#include <algorithm>

#define DEQUEUE_SIZE 4096
#define MIN_TASK     1
#define NO_REQUEST   -1

extern int QUILL_WORKERS;

inline std::function<void()> NO_RESPONSE = [](){};

void find_task_and_execute();
void* thread_runtime(void* args);
void* daemon_profiler(void* args);

struct QuillThread {
    std::function<void()>* _deque[DEQUEUE_SIZE];
    volatile int    _head, _tail;
    pthread_mutex_t _lock;
    pthread_t       _tid;
    int _size, _mxsize, _taskstolen, _taskcompleted, _taskcreated;
    volatile int    _sleep_flag;
    pthread_cond_t  _cond;
    pthread_mutex_t _sleep_mutex;
    QuillThread();
    ~QuillThread();
    int  push_task_to_deque(std::function<void()>* task_ptr);
    int  pop_task_from_deque(std::function<void()>*& task_ret);
    int  steal_task_from_deque(std::function<void()>*& task_ret);
    int  deque_size();
    void update_status_cell(long ithread);
    void communicate(long ithread);
    int  update_request_cell(long iself, long itheif);
    void print_stats();
};

struct FinishScope {
    int             _ntasks;
    pthread_mutex_t _finish_lock;
    FinishScope();
    ~FinishScope();
    void increment_task_counter();
    void decrement_task_counter();
    int  get_finish_ntasks();
    void reset();
};

struct RuntimeManager {
    volatile bool    shutdown;
    QuillThread*     threadpool;
    long*            thread_ix_ptr;
    pthread_key_t    thread_id_key;
    FinishScope      finishscope;
    std::atomic<int>*                    request_cells;
    bool*                                status_cells;
    std::atomic<std::function<void()>*>* transfer_cells;
    pthread_t        daemon_tid;
    std::atomic<int> active_workers;
    pthread_mutex_t  dct_mutex;
    RuntimeManager(int workers);
    ~RuntimeManager();
    void initialize_threadpool();
    void join_theadpool();
    void initial_finish_scope();
    long generate_victim_tid();
    void configure_DOP(double jpi_prev, double jpi_curr);
};

namespace quill {
    void init_runtime();
    void finalize_runtime();
    void start_finish();
    void end_finish();
    void async(std::function<void()>&& lambda);
}
