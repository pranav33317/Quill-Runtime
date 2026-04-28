#include "quill-runtime.h"
#include "quill.h"
#include <cstdlib>
#include <atomic>
#include <ctime>
#include <cstdio>
#include <cmath>
#include <sched.h>

extern "C" {
    void   profiler_init();
    void   profiler_finalize();
    double calculate_JPI();
}

static constexpr int    DCT_STEP_N      = 4;
static constexpr int    DCT_INTERVAL_MS = 500;
static constexpr int    DCT_WARMUP_MS   = 300;
static constexpr double DCT_THRESHOLD   = 0.10;

int QUILL_WORKERS = 1;
RuntimeManager* quill_runtime = nullptr;

QuillThread::QuillThread()
    : _head(0), _tail(0), _tid(0),
      _size(0), _mxsize(0), _taskstolen(0), _taskcompleted(0), _taskcreated(0),
      _sleep_flag(0)
{
    pthread_mutex_init(&_lock,        nullptr);
    pthread_mutex_init(&_sleep_mutex, nullptr);
    pthread_cond_init (&_cond,        nullptr);
}

QuillThread::~QuillThread() {
    pthread_mutex_destroy(&_lock);
    pthread_mutex_destroy(&_sleep_mutex);
    pthread_cond_destroy (&_cond);
}

int QuillThread::push_task_to_deque(std::function<void()>* task_ptr) {
    int ret = 1;
    if ((_tail - 1 + DEQUEUE_SIZE) % DEQUEUE_SIZE != _head) {
#ifdef QUILL_DEBUG
        _size++; _taskcreated++;
        _mxsize = std::max(_mxsize, _size);
#endif
        ret = 0;
        _deque[(_tail - 1 + DEQUEUE_SIZE) % DEQUEUE_SIZE] = task_ptr;
        _tail = (_tail - 1 + DEQUEUE_SIZE) % DEQUEUE_SIZE;
    }
    return ret;
}

void QuillThread::update_status_cell(long ithread) {
    quill_runtime->status_cells[ithread] = deque_size() > MIN_TASK;
}

int QuillThread::deque_size() {
    return (_head + DEQUEUE_SIZE - _tail) % DEQUEUE_SIZE;
}

void QuillThread::communicate(long ithread) {
    int theif_id = quill_runtime->request_cells[ithread].load();
    std::function<void()>* task_ptr = nullptr;
    if (theif_id == NO_REQUEST) return;
    bool cond = this->deque_size() <= MIN_TASK || pop_task_from_deque(task_ptr);
    quill_runtime->transfer_cells[theif_id].store(cond ? &NO_RESPONSE : task_ptr);
    quill_runtime->request_cells[ithread].store(NO_REQUEST);
}

int QuillThread::update_request_cell(long iself, long itheif) {
    if (!quill_runtime->status_cells[iself]) return 0;
    if (quill_runtime->threadpool[iself]._sleep_flag) return 0;
    int expected = NO_REQUEST;
    return quill_runtime->request_cells[iself].compare_exchange_strong(expected, itheif);
}

int QuillThread::pop_task_from_deque(std::function<void()>*& task_ret) {
    int ret = 1;
    if (_tail == _head) return ret;
    if (_tail != _head) {
#ifdef QUILL_DEBUG
        _size--; _taskcompleted++;
#endif
        ret = 0;
        task_ret = _deque[_tail];
        _tail = (_tail + 1) % DEQUEUE_SIZE;
    }
    return ret;
}

int QuillThread::steal_task_from_deque(std::function<void()>*& task_ret) {
    int ret = 1;
    if (_tail == _head) return ret;
    pthread_mutex_lock(&_lock);
    if (_tail != _head) {
#ifdef QUILL_DEBUG
        _size--; _taskstolen++;
#endif
        ret = 0;
        task_ret = _deque[(_head - 1 + DEQUEUE_SIZE) % DEQUEUE_SIZE];
        _head = (_head - 1 + DEQUEUE_SIZE) % DEQUEUE_SIZE;
    }
    pthread_mutex_unlock(&_lock);
    return ret;
}

void QuillThread::print_stats() {
    std::printf("Greatest Queue Size ==> %d\n", (int)_mxsize);
    std::printf("Task Created        ==> %d\n", (int)_taskcreated);
    std::printf("Task Stolen         ==> %d\n", (int)_taskstolen);
    std::printf("Task Completed      ==> %d\n", (int)_taskcompleted);
}

FinishScope::FinishScope() : _ntasks(0) { pthread_mutex_init(&_finish_lock, nullptr); }
FinishScope::~FinishScope() { pthread_mutex_destroy(&_finish_lock); }
void FinishScope::reset() { pthread_mutex_lock(&_finish_lock); _ntasks = 0; pthread_mutex_unlock(&_finish_lock); }
void FinishScope::increment_task_counter() { pthread_mutex_lock(&_finish_lock); _ntasks++; pthread_mutex_unlock(&_finish_lock); }
void FinishScope::decrement_task_counter() { pthread_mutex_lock(&_finish_lock); _ntasks--; pthread_mutex_unlock(&_finish_lock); }
int FinishScope::get_finish_ntasks() { return _ntasks; }

RuntimeManager::RuntimeManager(int workers)
    : shutdown(false),
      threadpool(new QuillThread[workers]),
      thread_ix_ptr(new long[workers]),
      request_cells(new std::atomic<int>[workers]),
      status_cells(new bool[workers]),
      transfer_cells(new std::atomic<std::function<void()>*>[workers]),
      active_workers(workers)
{
    pthread_mutex_init(&dct_mutex, nullptr);
    for (int ix = 0; ix < workers; ix++) {
        thread_ix_ptr[ix]  = ix;
        status_cells[ix]   = false;
        request_cells[ix]  = NO_REQUEST;
        transfer_cells[ix] = nullptr;
    }
}

RuntimeManager::~RuntimeManager() {
    pthread_mutex_destroy(&dct_mutex);
    delete[] threadpool;
    delete[] thread_ix_ptr;
    delete[] request_cells;
    delete[] status_cells;
    delete[] transfer_cells;
}

void RuntimeManager::initialize_threadpool() {
    pthread_key_create(&thread_id_key, NULL);
    pthread_setspecific(thread_id_key, (void*)&thread_ix_ptr[0]);
    for (long ithread = 1; ithread < QUILL_WORKERS; ithread++) {
        int ret = pthread_create(&threadpool[ithread]._tid, NULL,
            thread_runtime, (void*)&thread_ix_ptr[ithread]);
        if (ret) { std::printf("Error creating worker %ld\n", ithread); exit(1); }
    }
}

void RuntimeManager::join_theadpool() {
    for (int i = 1; i < QUILL_WORKERS; i++) {
        pthread_mutex_lock(&threadpool[i]._sleep_mutex);
        threadpool[i]._sleep_flag = 0;
        pthread_cond_signal(&threadpool[i]._cond);
        pthread_mutex_unlock(&threadpool[i]._sleep_mutex);
    }
    for (int ithread = 1; ithread < QUILL_WORKERS; ithread++)
        pthread_join(threadpool[ithread]._tid, NULL);
}

void RuntimeManager::initial_finish_scope() { finishscope.reset(); }
long RuntimeManager::generate_victim_tid() { return (long)(rand() % QUILL_WORKERS); }

void RuntimeManager::configure_DOP(double jpi_prev, double jpi_curr) {
    if (jpi_curr <= 0.0 || std::isinf(jpi_curr) || std::isnan(jpi_curr)) {
        std::printf("[DCT] Invalid JPI (%.9f) -- skipping.\n", jpi_curr);
        return;
    }
    pthread_mutex_lock(&dct_mutex);
    const int total = QUILL_WORKERS, min_active = 1;
    if (jpi_prev <= 0.0) {
        int slept = 0;
        for (int i = total-1; i >= 1 && slept < DCT_STEP_N; i--) {
            if (threadpool[i]._sleep_flag == 0) {
                pthread_mutex_lock(&threadpool[i]._sleep_mutex);
                threadpool[i]._sleep_flag = 1;
                pthread_mutex_unlock(&threadpool[i]._sleep_mutex);
                active_workers--; slept++;
            }
        }
        std::printf("[DCT] First JPI=%.9f. Slept %d. Active=%d/%d\n", jpi_curr, slept, active_workers.load(), total);
    } else if (jpi_curr > jpi_prev * (1.0 + DCT_THRESHOLD)) {
        int woke = 0;
        for (int i = 1; i < total && woke < DCT_STEP_N; i++) {
            if (threadpool[i]._sleep_flag == 1) {
                pthread_mutex_lock(&threadpool[i]._sleep_mutex);
                threadpool[i]._sleep_flag = 0;
                pthread_cond_signal(&threadpool[i]._cond);
                pthread_mutex_unlock(&threadpool[i]._sleep_mutex);
                active_workers++; woke++;
            }
        }
        std::printf("[DCT] JPI rose  %.9f -> %.9f (+%.0f%%). Woke %d. Active=%d/%d\n",
            jpi_prev, jpi_curr, (jpi_curr/jpi_prev-1.0)*100.0, woke, active_workers.load(), total);
    } else {
        int slept = 0;
        for (int i = total-1; i >= min_active && slept < DCT_STEP_N; i--) {
            if (threadpool[i]._sleep_flag == 0 && active_workers > min_active) {
                pthread_mutex_lock(&threadpool[i]._sleep_mutex);
                threadpool[i]._sleep_flag = 1;
                pthread_mutex_unlock(&threadpool[i]._sleep_mutex);
                active_workers--; slept++;
            }
        }
        std::printf("[DCT] JPI fell  %.9f -> %.9f. Slept %d. Active=%d/%d\n",
            jpi_prev, jpi_curr, slept, active_workers.load(), total);
    }
    pthread_mutex_unlock(&dct_mutex);
}

static void ms_sleep(int ms) {
    struct timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (ms%1000)*1000000L;
    nanosleep(&ts, NULL);
}

void* daemon_profiler(void*) {
    ms_sleep(DCT_WARMUP_MS);
    double jpi_prev = 0.0;
    while (!quill_runtime->shutdown) {
        double jpi_curr = calculate_JPI();
        quill_runtime->configure_DOP(jpi_prev, jpi_curr);
        if (jpi_curr > 0.0 && !std::isinf(jpi_curr) && !std::isnan(jpi_curr))
            jpi_prev = jpi_curr;
        ms_sleep(DCT_INTERVAL_MS);
    }
    return NULL;
}

void find_task_and_execute() {
    std::function<void()>* task_ptr = nullptr;
    long thread_ix = *((long*)pthread_getspecific(quill_runtime->thread_id_key));
    QuillThread* self_ptr = &(quill_runtime->threadpool[thread_ix]);

    if (thread_ix > 0) {
        pthread_mutex_lock(&self_ptr->_sleep_mutex);
        while (self_ptr->_sleep_flag == 1 && !quill_runtime->shutdown)
            pthread_cond_wait(&self_ptr->_cond, &self_ptr->_sleep_mutex);
        pthread_mutex_unlock(&self_ptr->_sleep_mutex);
    }

    int retval = self_ptr->pop_task_from_deque(task_ptr);

    while (retval && quill_runtime->finishscope.get_finish_ntasks() > 0 && !quill_runtime->shutdown) {
        if (thread_ix > 0 && self_ptr->_sleep_flag == 1) {
            pthread_mutex_lock(&self_ptr->_sleep_mutex);
            while (self_ptr->_sleep_flag == 1 && !quill_runtime->shutdown)
                pthread_cond_wait(&self_ptr->_cond, &self_ptr->_sleep_mutex);
            pthread_mutex_unlock(&self_ptr->_sleep_mutex);
        }

        long victim_ix = quill_runtime->generate_victim_tid();
        while (victim_ix == thread_ix) victim_ix = quill_runtime->generate_victim_tid();

        QuillThread* victim_ptr = &(quill_runtime->threadpool[victim_ix]);
        int updateret = victim_ptr->update_request_cell(victim_ix, thread_ix);
        if (!updateret) break;

        // FIXED: atomic load + shutdown check + sched_yield
        while (quill_runtime->transfer_cells[thread_ix].load() == nullptr) {
            if (quill_runtime->shutdown) break;
            sched_yield();
        }

        std::function<void()>* response = quill_runtime->transfer_cells[thread_ix].load();
        if (response != nullptr && response != &NO_RESPONSE) {
            task_ptr = response;
            retval = 0;
        }
        quill_runtime->transfer_cells[thread_ix].store(nullptr);
        if (retval == 0) break;
    }

    self_ptr->update_status_cell(thread_ix);
    self_ptr->communicate(thread_ix);
    if (retval) return;
    (*task_ptr)();
    quill_runtime->finishscope.decrement_task_counter();
    delete task_ptr;
}

void* thread_runtime(void* args) {
    pthread_setspecific(quill_runtime->thread_id_key, args);
    while (!quill_runtime->shutdown) find_task_and_execute();
    return NULL;
}

namespace quill {

void init_runtime() {
    srand(time(NULL));
    const char* w = getenv("QUILL_WORKERS");
    if (w) QUILL_WORKERS = atoi(w);
    std::printf("[Quill] Starting with %d workers.\n", QUILL_WORKERS);
    profiler_init();
    quill_runtime = new RuntimeManager(QUILL_WORKERS);
    quill_runtime->initialize_threadpool();
    int ret = pthread_create(&quill_runtime->daemon_tid, NULL, daemon_profiler, NULL);
    if (ret) { std::printf("Error: could not create DCT daemon\n"); exit(1); }
}

void finalize_runtime() {
    quill_runtime->shutdown = true;
    pthread_join(quill_runtime->daemon_tid, NULL);
    quill_runtime->join_theadpool();
#ifdef QUILL_DEBUG
    for (int i = 0; i < QUILL_WORKERS; i++) {
        std::printf("=== Thread %d ===\n", i);
        quill_runtime->threadpool[i].print_stats();
    }
#endif
    profiler_finalize();
    pthread_key_delete(quill_runtime->thread_id_key);
    delete quill_runtime;
    quill_runtime = nullptr;
}

void start_finish() { quill_runtime->initial_finish_scope(); }

void end_finish() {
    while (quill_runtime->finishscope.get_finish_ntasks() > 0)
        find_task_and_execute();
}

void async(std::function<void()>&& lambda) {
    quill_runtime->finishscope.increment_task_counter();
    std::function<void()>* task_ptr = new std::function<void()>(lambda);
    long thread_ix = *((long*)pthread_getspecific(quill_runtime->thread_id_key));
    int ret = quill_runtime->threadpool[thread_ix].push_task_to_deque(task_ptr);
    if (ret != 0) { std::printf("Deque overflow on thread %ld!\n", thread_ix); exit(1); }
}

} // namespace quill
