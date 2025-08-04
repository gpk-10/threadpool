/**
 * @file threadpool.h
 * @brief 线程池实现的头文件
 * @author GPK
 * @date 2025
 * @version 1.0
 * 
 * 该文件定义了线程池的数据结构和公共接口函数。
 * 提供线程安全的任务队列和动态线程管理功能。
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

/* 系统头文件 */
#include <pthread.h>    /* POSIX线程库 */
#include <stdbool.h>    /* C99布尔类型支持 */

/**
 * @brief 任务结构体
 * 
 * 表示线程池中的单个任务，包含要执行的函数和参数
 */
typedef struct task {
    void (*function)(void *arg);  // 任务函数指针，接受void*参数
    void *arg;                    // 传递给任务函数的参数
    struct task *next;            // 指向下一个任务的指针，用于构建任务队列
} task_t;

/**
 * @brief 任务队列结构体
 * 
 * 用于存储待处理的任务，支持线程安全的入队和出队操作
 */
typedef struct queue {
    task_t *front;                // 队列头部指针
    task_t *rear;                 // 队列尾部指针
    int count;                    // 队列中任务的数量
    
    pthread_mutex_t mutex;        // 队列互斥锁，保证线程安全
    pthread_cond_t not_empty;     // 条件变量，用于通知队列非空
} queue_t;

/**
 * @brief 线程池结构体
 * 
 * 管理线程池的所有资源，包括工作线程、任务队列和同步机制
 */
typedef struct thread_pool {
    pthread_t *threads;           // 工作线程数组
    int min_threads;              // 最小线程数
    int max_threads;              // 最大线程数
    int busy_threads;             // 当前忙碌的线程数
    int live_threads;             // 当前存活的线程数

    queue_t *task_queue;          // 任务队列指针

    bool shutdown;                // 线程池关闭标志，true表示正在关闭
    bool destroy;                 // 线程销毁标志，true表示正在销毁

    pthread_mutex_t pool_mutex;   // 线程池互斥锁，保护线程池状态
    pthread_cond_t thread_cond;   // 线程条件变量，用于线程间同步
    
    pthread_t adjust_thread;      // 动态调整线程，负责根据负载调整线程数量

    int adjust_interval;          // 调整线程检查间隔（秒）
    double scale_up_threshold;    // 线程扩容阈值
} thread_pool_t;

/**
 * @brief 创建线程池
 * 
 * 初始化线程池并创建指定数量的工作线程
 * 
 * @param min_threads 最小线程数
 * @param max_threads 最大线程数
 * @param adjust_interval 动态调整检查间隔(秒)
 * @return 成功返回线程池指针，失败返回NULL
 */
thread_pool_t *thread_pool_create(int min_threads, int max_threads, int adjust_interval);

/**
 * @brief 向线程池添加任务
 * 
 * 将一个新的任务添加到线程池的任务队列中
 * 
 * @param pool 线程池指针
 * @param function 要执行的任务函数
 * @param arg 传递给任务函数的参数
 * @return 成功返回0，失败返回-1
 */
int thread_pool_add_task(thread_pool_t *pool, void (*function)(void *), void *arg);

/**
 * @brief 销毁线程池
 * 
 * 关闭并清理线程池的所有资源
 * 
 * @param pool 线程池指针
 * @return 成功返回0，失败返回-1
 */
int thread_pool_destroy(thread_pool_t *pool);

/**
 * @brief 获取线程池状态
 * 
 * 获取线程池当前的运行状态信息
 * 
 * @param pool 线程池指针
 * @param live_threads 输出参数，返回当前存活的线程数
 * @param busy_threads 输出参数，返回当前忙碌的线程数
 * @param task_count 输出参数，返回任务队列中的任务数
 */
void thread_pool_get_status(thread_pool_t *pool, int *live_threads, int *busy_threads, int *task_count);

#endif // THREADPOOL_H
