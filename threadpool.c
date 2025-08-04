/**
 * @file threadpool.c
 * @brief 线程池实现文件
 * @author GPK
 * @date 2024
 * @version 1.0
 * 
 * 该文件实现了线程池的核心功能，包括：
 * - 任务队列管理（线程安全的入队/出队操作）
 * - 工作线程管理（创建、销毁、状态监控）
 * - 动态线程调整（根据负载自动扩容/缩容）
 * - 线程池生命周期管理
 * 
 * 所有操作都支持线程安全，适用于多线程并发环境。
 */

/* 系统头文件 */
#include "threadpool.h"    /* 线程池接口定义 */
#include <stdio.h>         /* 标准输入输出 */
#include <stdlib.h>        /* 内存管理、程序控制 */
#include <string.h>        /* 字符串和内存操作 */
#include <stdarg.h>        /* 可变参数支持 */
#include <unistd.h>        /* POSIX系统调用 */

/* 宏定义 */
#define LOG_LEVEL DEBUG    /* 日志输出级别阈值 */

/**
 * @brief 日志级别枚举
 * 
 * 定义了四种日志级别，按严重程度递增排列。
 * 用于控制日志输出的详细程度和过滤级别。
 */
enum log_level {
    DEBUG = 0,         /* 调试信息：详细的执行流程，用于开发调试 */
    INFO  = 1,         /* 普通信息：程序正常运行的关键节点 */
    WARN  = 2,         /* 警告信息：可能的问题，但不影响程序继续运行 */
    ERROR = 3          /* 错误信息：发生错误但程序仍可恢复继续 */
};

/**
 * @brief 日志输出函数
 * 
 * 根据日志级别过滤并格式化输出日志信息。只有当日志级别等于或高于
 * 设定的LOG_LEVEL时，日志才会被输出到标准输出。
 * 
 * @param level  日志级别，用于过滤和标识日志重要性
 * @param format 格式化字符串，遵循printf格式规范
 * @param ...    可变参数列表，用于填充格式化字符串
 * 
 * @note 输出格式：[LEVEL] message
 * @note 函数是线程安全的（依赖于printf的线程安全性）
 */
static void log_p(enum log_level level, const char *format, ...)
{ 
    /* 日志级别字符串映射表 */
    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    /* 级别过滤：低于阈值的日志直接丢弃 */
    if (level < LOG_LEVEL) {
        return;
    }

    /* 输出日志级别标签 */
    printf("[%s] ", level_str[level]);
    
    /* 处理可变参数并格式化输出 */
    va_list args;
    va_start(args, format);          /* 初始化可变参数列表 */
    vprintf(format, args);           /* 格式化输出消息内容 */
    printf("\n");                    /* 添加换行符 */
    va_end(args);                    /* 清理可变参数列表 */

    /* 立即刷新缓冲区，确保日志实时显示 */
    fflush(stdout);
}

/**
 * @brief 创建任务队列（内部函数）
 * 
 * 分配内存并初始化一个新的任务队列，包括队列本身和相关的同步原语。
 * 创建的队列支持线程安全的并发访问操作。
 * 
 * @return queue_t* 成功时返回队列指针，失败时返回NULL
 * 
 * @note 调用者负责在不再使用时调用task_queue_destroy()释放资源
 * @note 队列初始为空状态（count=0, front=rear=NULL）
 * 
 * @see task_queue_destroy()
 */
static queue_t *queue_create(void)
{
    log_p(INFO, "Starting queue creation...");

    /* 分配队列结构体内存 */
    queue_t *queue = (queue_t *)malloc(sizeof(queue_t));
    if (queue == NULL) {
        log_p(ERROR, "Failed to allocate memory for queue structure");
        return NULL;
    }
    
    /* 初始化队列基本字段 */
    queue->front = NULL;                /* 队列头指针（第一个任务） */
    queue->rear  = NULL;                /* 队列尾指针（最后一个任务） */
    queue->count = 0;                   /* 队列中的任务数量 */
    
    /* 初始化互斥锁：保护队列数据结构的原子性操作 */
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        log_p(ERROR, "Failed to initialize queue mutex");
        free(queue);
        return NULL;
    }

    /* 初始化条件变量：用于阻塞等待队列非空 */
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        log_p(ERROR, "Failed to initialize queue condition variable");
        pthread_mutex_destroy(&queue->mutex);    /* 清理已创建的互斥锁 */
        free(queue);
        return NULL;
    }
    
    log_p(INFO, "Queue created successfully");
    return queue;
}

/**
 * @brief 销毁任务队列（内部函数）
 * 
 * 释放队列及其所有相关资源，包括队列中剩余的任务、同步原语和队列结构体本身。
 * 在销毁前会清空队列中的所有待执行任务。
 * 
 * @param queue 要销毁的队列指针，允许为NULL（安全调用）
 * 
 * @warning 调用此函数前确保没有线程正在访问该队列
 * @warning 队列中剩余的任务将被直接释放，不会执行
 * 
 * @see queue_create()
 */
static void task_queue_destroy(queue_t *queue)
{
    log_p(INFO, "Starting queue destruction...");

    /* 参数有效性检查 */
    if (queue == NULL) {
        log_p(ERROR, "Cannot destroy NULL queue");
        return;
    }

    /* 获取锁并清空队列中的所有任务 */
    pthread_mutex_lock(&queue->mutex);
    task_t *current_task = queue->front;
    while (current_task != NULL) {
        task_t *next_task = current_task->next;
        free(current_task);                    /* 释放任务内存 */
        current_task = next_task;
    }
    pthread_mutex_unlock(&queue->mutex);

    /* 销毁同步原语 */
    pthread_cond_destroy(&queue->not_empty);   /* 销毁条件变量 */
    pthread_mutex_destroy(&queue->mutex);      /* 销毁互斥锁 */
    
    /* 释放队列结构体 */
    free(queue);
    
    log_p(INFO, "Queue destroyed successfully");
}

/**
 * @brief 向队列添加任务（入队操作，内部函数）
 * 
 * 线程安全地将一个任务添加到队列尾部。如果有线程正在等待任务，
 * 会通过条件变量唤醒一个等待的线程。
 * 
 * @param queue 目标队列指针
 * @param task  要添加的任务指针
 * 
 * @return int  成功返回0，失败返回-1
 * 
 * @note 函数会自动设置task->next为NULL
 * @note 包含队列状态自修复机制
 * @note 操作完成后会发送not_empty信号
 * 
 * @see task_queue_pop()
 */
static int task_queue_push(queue_t *queue, task_t *task)
{
    /* 参数有效性检查 */
    if (queue == NULL || task == NULL) {
        log_p(ERROR, "Invalid parameters for queue push operation");
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);
    
    /* 确保任务链表节点的完整性 */
    task->next = NULL;
    
    if (queue->front == NULL) {
        /* 队列为空：设置头尾指针都指向新任务 */
        queue->front = task;
        queue->rear = task;
    } else {
        /* 队列非空：在尾部追加新任务 */
        if (queue->rear != NULL) {
            queue->rear->next = task;
            queue->rear = task;
        } else {
            /* 
             * 异常状态修复：front非空但rear为空
             * 这种情况理论上不应发生，但为了健壮性进行修复
             */
            log_p(WARN, "Detected inconsistent queue state, repairing...");
            task_t *current = queue->front;
            while (current->next != NULL) {
                current = current->next;          /* 找到真正的队尾 */
            }
            current->next = task;
            queue->rear = task;
        }
    }
    
    /* 更新计数并通知等待线程 */
    queue->count++;
    pthread_cond_signal(&queue->not_empty);       /* 唤醒等待的消费者线程 */
    pthread_mutex_unlock(&queue->mutex);
    
    log_p(DEBUG, "Task enqueued successfully, queue size: %d", queue->count);
    return 0;
}

/**
 * @brief 从队列获取任务（出队操作，内部函数）
 * 
 * 线程安全地从队列头部取出一个任务。如果队列为空，调用线程将阻塞等待
 * 直到有新任务加入队列。
 * 
 * @param queue 源队列指针
 * @param task  输出参数，用于返回获取到的任务指针
 * 
 * @return int  成功返回0，失败返回-1
 * 
 * @note 如果队列为空，线程会阻塞等待not_empty条件变量
 * @note 包含防御性编程检查，确保操作的安全性
 * @note 当队列变为空时，会自动重置rear指针
 * 
 * @see task_queue_push()
 */
static int task_queue_pop(queue_t *queue, task_t **task)
{
    /* 参数有效性检查 */
    if (queue == NULL || task == NULL) {
        log_p(ERROR, "Invalid parameters for queue pop operation");
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);
    
    /* 等待队列非空：如果队列为空则阻塞等待 */
    while (queue->front == NULL) {
        log_p(DEBUG, "Queue is empty, waiting for tasks...");
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    /* 
     * 防御性检查：再次确认队列非空
     * 虽然上面的while循环应该保证这一点，但这是额外的安全措施
     */
    if (queue->front != NULL) {
        /* 取出队头任务 */
        *task = queue->front;
        queue->front = queue->front->next;
        
        /* 如果队列变空，重置尾指针维护一致性 */
        if (queue->front == NULL) {
            queue->rear = NULL;
        }
        
        /* 更新计数 */
        queue->count--;
        pthread_mutex_unlock(&queue->mutex);
        
        log_p(DEBUG, "Task dequeued successfully, remaining: %d", queue->count);
        return 0;
    } else {
        /* 异常情况：队列意外变空 */
        *task = NULL;
        pthread_mutex_unlock(&queue->mutex);
        log_p(WARN, "Queue became empty unexpectedly during pop operation");
        return -1;
    }
}

/**
 * @brief 工作线程主函数（内部函数）
 * 
 * 每个工作线程的主执行函数，负责从任务队列中获取并执行任务。
 * 线程会持续运行直到收到关闭信号或被标记为需要销毁。
 * 
 * @param arg 线程池指针，转换为thread_pool_t*类型使用
 * 
 * @return void* 线程退出时返回NULL
 * 
 * @note 线程执行流程：
 *       1. 等待任务或关闭信号
 *       2. 检查是否需要自我销毁
 *       3. 获取并执行任务
 *       4. 更新线程池状态
 * 
 * @note 线程安全：所有对池状态的访问都受互斥锁保护
 */
static void *worker_thread(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;

    /* 主工作循环：直到收到关闭信号 */
    pthread_mutex_lock(&pool->pool_mutex);
    while (!pool->shutdown) {
        
        /* 等待任务或关闭信号 */
        while (pool->task_queue->count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->thread_cond, &pool->pool_mutex);
            
            /* 
             * 检查线程销毁请求：
             * 只有在线程数超过最小值时才允许销毁
             */
            if (pool->destroy && pool->live_threads > pool->min_threads) {
                pool->destroy = false;              /* 重置销毁标志 */
                pool->live_threads--;               /* 更新存活线程计数 */
                
                /* 从线程数组中移除当前线程ID */
                pthread_t current_thread = pthread_self();
                for (int i = 0; i < pool->max_threads; i++) {
                    if (pool->threads[i] == current_thread) {
                        pool->threads[i] = 0;       /* 清零线程槽位 */
                        break;
                    }
                }
                
                pthread_mutex_unlock(&pool->pool_mutex);
                log_p(INFO, "Worker thread self-destructing, ID: %ld", (long)current_thread);
                pthread_exit(NULL);
            }
        }
        
        /* 再次检查关闭信号（可能在等待期间收到） */
        if (pool->shutdown) {
            break;
        }

        /* 标记为忙碌状态 */
        pool->busy_threads++;
        pthread_mutex_unlock(&pool->pool_mutex);

        /* 获取并执行任务 */
        log_p(DEBUG, "Worker thread %ld starting task execution", pthread_self());
        task_t *task = NULL;
        
        if (task_queue_pop(pool->task_queue, &task) == 0 && task != NULL) {
            /* 执行任务：检查函数指针有效性 */
            if (task->function != NULL) {
                task->function(task->arg);          /* 调用用户任务函数 */
            } else {
                log_p(WARN, "Encountered task with NULL function pointer");
            }
            free(task);                             /* 释放任务内存 */
        } else {
            log_p(DEBUG, "Failed to retrieve task from queue");
        }

        /* 任务完成，更新线程状态 */
        log_p(DEBUG, "Worker thread %ld finished task execution", pthread_self());
        pthread_mutex_lock(&pool->pool_mutex);
        pool->busy_threads--;                       /* 标记为空闲状态 */
        /* 继续持有锁进入下一次循环 */
    }
    pthread_mutex_unlock(&pool->pool_mutex);
    
    log_p(INFO, "Worker thread exiting, ID: %ld", pthread_self());
    pthread_exit(NULL);
}

/**
 * @brief 线程池动态调整线程（内部函数）
 * 
 * 监控线程池状态并根据负载情况动态调整线程数量。该线程定期检查
 * 任务队列长度和线程使用情况，实现自动扩容和缩容。
 * 
 * @param arg 线程池指针，转换为thread_pool_t*类型使用
 * 
 * @return void* 线程退出时返回NULL
 * 
 * @note 调整策略：
 *       - 扩容：当任务数 > 存活线程数 * 扩容阈值时触发
 *       - 缩容：当忙碌线程数 < 存活线程数且超过最小值时触发
 *       - 缩容采用保守策略，一次只销毁一个线程
 * 
 * @note 调整间隔由pool->adjust_interval控制
 */
static void *adjust_thread(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;
    
    /* 主调整循环：直到线程池关闭 */
    pthread_mutex_lock(&pool->pool_mutex);
    while (!pool->shutdown) {
        pthread_mutex_unlock(&pool->pool_mutex);
        /* 等待调整间隔 */
        sleep(pool->adjust_interval);

        /* 获取当前线程池状态快照（重新获取锁） */
        pthread_mutex_lock(&pool->pool_mutex);
        int max_threads  = pool->max_threads;
        int min_threads  = pool->min_threads;
        int live_threads = pool->live_threads;
        int busy_threads = pool->busy_threads;
        int task_count   = pool->task_queue->count;
        /* 保持锁，继续处理 */
        
        log_p(DEBUG, "Pool status - Live: %d, Busy: %d, Tasks: %d", 
              live_threads, busy_threads, task_count);

        /* 
         * 动态扩容逻辑：
         * 当任务堆积且未达到最大线程数时创建新线程
         */
        if (task_count > live_threads * pool->scale_up_threshold && 
            live_threads < max_threads) {
            
            /* 计算需要添加的线程数 */
            int add_threads = task_count / pool->scale_up_threshold - live_threads;
            
            /* 限制添加数量不超过最大容量 */
            if (add_threads > max_threads - live_threads) {
                add_threads = max_threads - live_threads;
            }

            /* 寻找空闲槽位并创建新线程 */
            int attempts = add_threads * 2;    /* 限制查找次数防止无限循环 */
            int index = 0;
            
            /* 暂时释放锁来创建线程 */
            pthread_mutex_unlock(&pool->pool_mutex);
            
            while (add_threads > 0 && attempts > 0) {
                index = (index + 1) % max_threads;
                attempts--;
                
                /* 找到空闲槽位 */
                if (pool->threads[index] == 0) {
                    if (pthread_create(&pool->threads[index], NULL, worker_thread, (void *)pool) == 0) {
                        log_p(DEBUG, "Created new worker thread, ID: %ld", pool->threads[index]);
                        add_threads--;
                        
                        /* 更新存活线程计数 */
                        pthread_mutex_lock(&pool->pool_mutex);
                        pool->live_threads++;
                        pthread_mutex_unlock(&pool->pool_mutex);
                    } else {
                        log_p(ERROR, "Failed to create worker thread at slot %d", index);
                    }
                }
            }
            
            /* 重新获取锁 */
            pthread_mutex_lock(&pool->pool_mutex);
        }
        
        /* 
         * 动态缩容逻辑：
         * 当线程利用率低且超过最小线程数时销毁多余线程
         * 采用保守策略，一次只标记一个线程退出
         */
        if (busy_threads < live_threads && live_threads > min_threads) {
            /* 确保当前没有其他销毁操作在进行 */
            if (!pool->destroy && pool->live_threads > pool->min_threads) {
                pool->destroy = true;                    /* 设置销毁标志 */
                
                /* 唤醒一个等待的工作线程，让它自我销毁 */
                pthread_cond_signal(&pool->thread_cond);
                log_p(DEBUG, "Signaled one thread to exit, current count: %d", live_threads);
            }
        }
        /* 继续持有锁进入下一次循环检查 */
    }
    pthread_mutex_unlock(&pool->pool_mutex);
    
    log_p(INFO, "Adjust thread exiting");
    return NULL;
}

/**
 * @brief 创建线程池
 * 
 * 分配并初始化一个新的线程池，包括任务队列、工作线程、调整线程和所有
 * 必要的同步原语。创建成功后线程池立即可用于任务提交。
 * 
 * @param min_threads      最小线程数，线程池始终保持的线程数量
 * @param max_threads      最大线程数，线程池能达到的最大线程数量
 * @param adjust_interval  动态调整检查间隔（秒）
 * 
 * @return thread_pool_t*  成功时返回线程池指针，失败时返回NULL
 * 
 * @note 参数约束：
 *       - min_threads > 0
 *       - max_threads > 0  
 *       - min_threads <= max_threads
 * 
 * @note 创建的线程池包含：
 *       - min_threads个工作线程（立即启动）
 *       - 1个动态调整线程
 *       - 线程安全的任务队列
 *       - 必要的同步原语
 * 
 * @warning 如果创建过程中任何步骤失败，会自动清理已分配的资源
 * 
 * @see thread_pool_destroy()
 */
thread_pool_t *thread_pool_create(int min_threads, int max_threads, int adjust_interval)
{
    log_p(INFO, "Starting thread pool creation...");
    
    /* 参数有效性检查 */
    if (min_threads <= 0 || max_threads <= 0 || min_threads > max_threads) {
        log_p(ERROR, "Invalid thread pool parameters: min=%d, max=%d", 
              min_threads, max_threads);
        return NULL;
    }

    /* 分配线程池结构体内存 */
    thread_pool_t *pool = (thread_pool_t *)malloc(sizeof(thread_pool_t));
    if (pool == NULL) {
        log_p(ERROR, "Failed to allocate memory for thread pool structure");
        return NULL;
    }
    
    /* 初始化基本配置参数 */
    pool->min_threads  = min_threads;
    pool->max_threads  = max_threads;
    pool->busy_threads = 0;
    pool->live_threads = 0;
    pool->shutdown     = false;
    pool->destroy      = false;

    /* 创建任务队列 */
    pool->task_queue = queue_create();
    if (pool->task_queue == NULL) {
        log_p(ERROR, "Failed to create task queue");
        free(pool);
        return NULL;
    }

    /* 初始化线程池互斥锁 */
    if (pthread_mutex_init(&pool->pool_mutex, NULL) != 0) {
        log_p(ERROR, "Failed to initialize thread pool mutex");
        task_queue_destroy(pool->task_queue);
        free(pool);
        return NULL;
    }

    /* 初始化线程池条件变量 */
    if (pthread_cond_init(&pool->thread_cond, NULL) != 0) {
        log_p(ERROR, "Failed to initialize thread pool condition variable");
        task_queue_destroy(pool->task_queue);
        pthread_mutex_destroy(&pool->pool_mutex);
        free(pool);
        return NULL;
    }

    /* 分配线程ID数组 */
    pool->threads = (pthread_t *)malloc(max_threads * sizeof(pthread_t));
    if (pool->threads == NULL) {
        log_p(ERROR, "Failed to allocate memory for thread array");
        pthread_mutex_destroy(&pool->pool_mutex);
        pthread_cond_destroy(&pool->thread_cond);
        task_queue_destroy(pool->task_queue);
        free(pool);
        return NULL;
    }
    memset(pool->threads, 0, max_threads * sizeof(pthread_t));    /* 清零数组 */

    /* 创建初始工作线程 */
    for (int i = 0; i < min_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, (void *)pool) != 0) {
            log_p(ERROR, "Failed to create worker thread %d", i);
            
            /* 清理已创建的线程 */
            pool->shutdown = true;
            for (int j = 0; j < i; j++) {
                pthread_cond_signal(&pool->thread_cond);    /* 唤醒线程 */
            }
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);       /* 等待线程退出 */
            }
            
            /* 清理所有资源 */
            free(pool->threads);
            pthread_mutex_destroy(&pool->pool_mutex);
            pthread_cond_destroy(&pool->thread_cond);
            task_queue_destroy(pool->task_queue);
            free(pool);
            return NULL;
        }
        pool->live_threads++;
    }

    /* 创建动态调整线程 */
    if (pthread_create(&pool->adjust_thread, NULL, adjust_thread, (void *)pool) != 0) {
        log_p(ERROR, "Failed to create adjust thread");
        
        /* 清理所有工作线程 */
        pool->shutdown = true;
        for (int i = 0; i < min_threads; i++) {
            pthread_cond_signal(&pool->thread_cond);
        }
        for (int i = 0; i < min_threads; i++) {
            pthread_join(pool->threads[i], NULL);
        }
        
        /* 清理所有资源 */
        free(pool->threads);
        pthread_mutex_destroy(&pool->pool_mutex);
        pthread_cond_destroy(&pool->thread_cond);
        task_queue_destroy(pool->task_queue);
        free(pool);
        return NULL;
    }

    /* 设置动态调整参数 */
    pool->adjust_interval = adjust_interval;
    pool->scale_up_threshold = 2.0;            /* 默认扩容阈值 */

    log_p(INFO, "Thread pool created successfully");
    log_p(INFO, "Initial configuration: min=%d, max=%d, live=%d", 
          min_threads, max_threads, pool->live_threads);
    return pool;
}

/**
 * @brief 向线程池提交任务
 * 
 * 创建一个新任务并将其提交到线程池的任务队列中等待执行。
 * 如果有空闲线程，任务会被立即处理；否则会排队等待。
 * 
 * @param pool     线程池指针
 * @param function 任务函数指针，将在工作线程中被调用
 * @param arg      传递给任务函数的参数
 * 
 * @return int     成功返回0，失败返回-1
 * 
 * @note 任务函数应该：
 *       - 不能长时间阻塞
 *       - 处理好异常情况
 *       - 正确管理传入的参数内存
 * 
 * @note 线程安全：可以从多个线程并发调用
 * 
 * @warning 如果线程池正在关闭，任务提交会失败
 * @warning function不能为NULL
 * 
 * @see worker_thread()
 */
int thread_pool_add_task(thread_pool_t *pool, void (*function)(void *), void *arg)
{
    /* 参数有效性检查 */
    if (pool == NULL || function == NULL) {
        log_p(ERROR, "Invalid arguments: pool=%p, function=%p", 
              (void*)pool, (void*)function);
        return -1;
    }

    /* 检查线程池状态（需要加锁检查） */
    pthread_mutex_lock(&pool->pool_mutex);
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->pool_mutex);
        log_p(ERROR, "Cannot add task: thread pool is shutting down");
        return -1;
    }
    pthread_mutex_unlock(&pool->pool_mutex);

    /* 分配新任务结构体 */
    task_t *new_task = (task_t *)malloc(sizeof(task_t));
    if (new_task == NULL) {
        log_p(ERROR, "Failed to allocate memory for new task");
        return -1;
    }

    /* 初始化任务字段 */
    new_task->function = function;
    new_task->arg      = arg;
    new_task->next     = NULL;

    /* 将任务加入队列 */
    if (task_queue_push(pool->task_queue, new_task) != 0) {
        log_p(ERROR, "Failed to enqueue task");
        free(new_task);
        return -1;
    }

    /* 获取当前队列长度用于日志 */
    pthread_mutex_lock(&pool->pool_mutex);
    int current_task_count = pool->task_queue->count;
    pthread_mutex_unlock(&pool->pool_mutex);
    
    /* 通知等待的工作线程 */
    pthread_cond_signal(&pool->thread_cond);
    
    log_p(INFO, "Task submitted successfully, queue size: %d", current_task_count);
    return 0;
}

/**
 * @brief 销毁线程池
 * 
 * 安全地关闭线程池，等待所有线程完成当前任务后退出，
 * 并释放所有相关资源。未执行的任务将被丢弃。
 * 
 * @param pool 要销毁的线程池指针
 * 
 * @return int 成功返回0，失败返回-1
 * 
 * @note 销毁过程：
 *       1. 设置关闭标志阻止新任务提交
 *       2. 唤醒所有等待的工作线程
 *       3. 等待所有工作线程完成并退出
 *       4. 等待调整线程退出
 *       5. 清理所有资源（队列、同步原语、内存）
 * 
 * @warning 调用后pool指针将失效，不可再使用
 * @warning 队列中未执行的任务会被直接丢弃
 * @warning 函数会阻塞直到所有线程退出
 * 
 * @see thread_pool_create()
 */
int thread_pool_destroy(thread_pool_t *pool)
{
    /* 参数有效性检查 */
    if (pool == NULL) {
        log_p(ERROR, "Cannot destroy NULL thread pool");
        return -1;
    }

    log_p(INFO, "Starting thread pool destruction...");
    
    /* 设置关闭标志，阻止新任务提交 */
    pthread_mutex_lock(&pool->pool_mutex);
    pool->shutdown = true;
    int live_threads = pool->live_threads;
    pthread_mutex_unlock(&pool->pool_mutex);

    /* 唤醒所有等待的工作线程，让它们检查关闭标志 */
    for (int i = 0; i < live_threads; i++) {
        pthread_cond_signal(&pool->thread_cond);
    }

    /* 等待所有工作线程安全退出 */
    for (int i = 0; i < pool->max_threads; i++) {
        if (pool->threads[i] != 0) {
            pthread_t thread_id = pool->threads[i];
            
            if (pthread_join(thread_id, NULL) == 0) {
                log_p(DEBUG, "Successfully joined worker thread %d", i);
            } else {
                log_p(WARN, "Failed to join worker thread %d", i);
            }
            
            pool->threads[i] = 0;    /* 清零已处理的线程槽位 */
        }
    }
    
    /* 等待动态调整线程退出 */
    if (pthread_join(pool->adjust_thread, NULL) != 0) {
        log_p(WARN, "Failed to join adjust thread");
    } else {
        log_p(DEBUG, "Successfully joined adjust thread");
    }

    /* 清理所有资源 */
    task_queue_destroy(pool->task_queue);         /* 销毁任务队列 */
    pthread_mutex_destroy(&pool->pool_mutex);     /* 销毁线程池互斥锁 */
    pthread_cond_destroy(&pool->thread_cond);     /* 销毁条件变量 */
    free(pool->threads);                          /* 释放线程数组 */
    free(pool);                                   /* 释放线程池结构体 */
    
    log_p(INFO, "Thread pool destroyed successfully");
    return 0;
}

/**
 * @brief 获取线程池状态信息
 * 
 * 原子地获取线程池当前的运行状态，包括线程数量和任务队列信息。
 * 返回的状态是调用时刻的快照，可能在返回后立即发生变化。
 * 
 * @param pool         线程池指针
 * @param live_threads 输出参数，当前存活的线程总数
 * @param busy_threads 输出参数，当前正在执行任务的线程数
 * @param task_count   输出参数，当前队列中等待的任务数
 * 
 * @note 所有输出参数都可以为NULL（如果不需要该信息）
 * @note 函数是线程安全的，可以从多个线程并发调用
 * @note 获取的是状态快照，不保证一致性持续时间
 * 
 * @warning 如果pool为NULL，所有输出参数将保持不变
 * 
 * @see thread_pool_create(), adjust_thread()
 */
void thread_pool_get_status(thread_pool_t *pool, int *live_threads, int *busy_threads, int *task_count)
{
    /* 参数有效性检查 */
    if (pool == NULL) {
        log_p(WARN, "Cannot get status: thread pool is NULL");
        return;
    }
    
    /* 原子地获取所有状态信息 */
    pthread_mutex_lock(&pool->pool_mutex);
    
    if (live_threads != NULL) {
        *live_threads = pool->live_threads;
    }
    
    if (busy_threads != NULL) {
        *busy_threads = pool->busy_threads;
    }
    
    if (task_count != NULL) {
        *task_count = pool->task_queue->count;
    }
    
    pthread_mutex_unlock(&pool->pool_mutex);
    
    log_p(DEBUG, "Status retrieved: live=%d, busy=%d, tasks=%d", 
          pool->live_threads, pool->busy_threads, pool->task_queue->count);
}

