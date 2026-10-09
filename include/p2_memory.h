#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int p2_memory_init(uint32_t bytes);
void* p2_physical_to_host(uint32_t address);
uint32_t p2_host_to_physical(const void* address);
void p2_destroy_mutex(void* mutex);
void p2_destroy_condition(void* condition);
void p2_destroy_message_queue(void* queue);
void p2_destroy_thread(void* thread);
void p2_thread_checkpoint(void);
int p2_critical_section_held(void);
void p2_critical_thread_exit(void);
void p2_thread_sleep_ticks(int64_t ticks);
int p2_thread_should_stop(void);
// Cancellation requested, even inside a work scope. Only for idle blocking waits.
int p2_thread_cancel_requested(void);
int p2_begin_thread_work(void);
void p2_end_thread_work(int cancellation_state);
void p2_heap_report(const char* text);
void p2_heap_reportf(const char* format, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Record/replay tick barrier (src/threads.cpp): created threads count as busy
// except while blocked in a wait.
void p2_worker_idle_begin(void);
void p2_worker_idle_end(void);
void p2_main_block_begin(void);
void p2_main_block_end(void);
void p2_wait_main_yield(void);
int p2_busy_workers(void);
void p2_thread_exclude_from_barrier(void);
int p2_messages_pending_for_waiters(void);
#ifdef __cplusplus
}
#endif
