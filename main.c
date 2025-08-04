#include "threadpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/* 测试任务数据结构 */
typedef struct {
    int task_id;
    int sleep_time;
    char message[256];
} test_task_data_t;

/* 测试任务函数1：简单的计算任务 */
void compute_task(void *arg) {
    test_task_data_t *data = (test_task_data_t *)arg;
    
    printf("Task %d started: %s\n", data->task_id, data->message);
    
    // 模拟计算工作
    long result = 0;
    for (int i = 0; i < 1000000; i++) {
        result += i * data->task_id;
    }
    
    // 模拟I/O等待
    sleep(data->sleep_time);
    
    printf("Task %d completed: result=%ld, slept for %d seconds\n", 
           data->task_id, result, data->sleep_time);
    
    free(data);  // 释放任务数据
}

/* 测试任务函数2：打印任务 */
void print_task(void *arg) {
    char *message = (char *)arg;
    printf("Print task: %s (thread: %lu)\n", message, pthread_self());
    sleep(1);  // 模拟工作时间
    free(message);  // 释放消息内存
}

/* 测试任务函数3：文件操作任务 */
void file_task(void *arg) {
    int *file_id = (int *)arg;
    char filename[64];
    snprintf(filename, sizeof(filename), "test_file_%d.txt", *file_id);
    
    printf("File task %d: Creating file %s\n", *file_id, filename);
    
    // 创建并写入文件
    FILE *fp = fopen(filename, "w");
    if (fp != NULL) {
        fprintf(fp, "This is test file %d created by thread %lu\n", 
                *file_id, pthread_self());
        fclose(fp);
        printf("File task %d: File %s created successfully\n", *file_id, filename);
    } else {
        printf("File task %d: Failed to create file %s\n", *file_id, filename);
    }
    
    sleep(2);  // 模拟文件操作时间
    free(file_id);
}

/* 基础功能测试 */
void test_basic_functionality(void) {
    printf("\n");
    printf("=====================================\n");
    printf("        基础功能测试开始\n");
    printf("=====================================\n");
    
    // 创建线程池：最小2个线程，最大8个线程，每3秒检查一次
    thread_pool_t *pool = thread_pool_create(2, 8, 3);
    if (pool == NULL) {
        printf("Failed to create thread pool\n");
        return;
    }
    
    // 添加一些简单任务
    for (int i = 0; i < 5; i++) {
        test_task_data_t *data = malloc(sizeof(test_task_data_t));
        data->task_id = i + 1;
        data->sleep_time = 1 + (i % 3);
        snprintf(data->message, sizeof(data->message), 
                "Basic test task %d", data->task_id);
        
        if (thread_pool_add_task(pool, compute_task, data) != 0) {
            printf("Failed to add task %d\n", i + 1);
            free(data);
        }
    }
    
    // 等待任务完成
    printf("Waiting for tasks to complete...\n");
    sleep(8);
    
    // 获取状态
    int current_threads, busy_threads, task_count;
    thread_pool_get_status(pool, &current_threads, &busy_threads, &task_count);
    printf("Pool status: threads=%d, busy=%d, tasks=%d\n", 
           current_threads, busy_threads, task_count);
    
    // 销毁线程池
    printf("Destroying thread pool...\n");
    thread_pool_destroy(pool);
    printf("Basic functionality test completed\n");
    printf("=====================================\n\n");
}

/* 动态扩容测试 */
void test_dynamic_scaling_up(void) {
    printf("=====================================\n");
    printf("       动态扩容测试开始\n");
    printf("=====================================\n");
    
    // 创建线程池：最小2个线程，最大10个线程，每2秒检查一次
    thread_pool_t *pool = thread_pool_create(2, 10, 2);
    if (pool == NULL) {
        printf("Failed to create thread pool\n");
        return;
    }
    
    // 快速添加大量任务触发扩容
    printf("Adding 10 tasks to trigger scaling up...\n");
    for (int i = 0; i < 10; i++) {
        test_task_data_t *data = malloc(sizeof(test_task_data_t));
        data->task_id = i + 1;
        data->sleep_time = 2;  // 较短的睡眠时间
        snprintf(data->message, sizeof(data->message), 
                "Scale-up test task %d", data->task_id);
        
        thread_pool_add_task(pool, compute_task, data);
        usleep(100000);  // 100ms间隔添加任务
    }
    
    // 监控扩容过程
    for (int i = 0; i < 10; i++) {
        int current_threads, busy_threads, task_count;
        thread_pool_get_status(pool, &current_threads, &busy_threads, &task_count);
        printf("Monitor %d: threads=%d, busy=%d, tasks=%d\n", 
               i + 1, current_threads, busy_threads, task_count);
        sleep(2);
        
        // 如果没有任务了就提前结束
        if (task_count == 0 && busy_threads == 0) {
            printf("All tasks completed, ending monitor early\n");
            break;
        }
    }
    
    printf("Destroying thread pool...\n");
    thread_pool_destroy(pool);
    printf("Dynamic scaling up test completed\n");
    printf("=====================================\n\n");
}

/* 动态缩容测试 */
void test_dynamic_scaling_down(void) {
    printf("=====================================\n");
    printf("       动态缩容测试开始\n");
    printf("=====================================\n");
    
    // 创建线程池：最小2个线程，最大8个线程，每2秒检查一次
    thread_pool_t *pool = thread_pool_create(2, 8, 2);
    if (pool == NULL) {
        printf("Failed to create thread pool\n");
        return;
    }
    
    // 先添加一些任务触发扩容
    printf("Adding tasks to trigger initial scaling up...\n");
    for (int i = 0; i < 20; i++) {
        test_task_data_t *data = malloc(sizeof(test_task_data_t));
        data->task_id = i + 1;
        data->sleep_time = 1;
        snprintf(data->message, sizeof(data->message), 
                "Scale-down setup task %d", data->task_id);
        
        thread_pool_add_task(pool, compute_task, data);
    }
    
    // 等待扩容完成
    printf("Waiting for scale-up to complete...\n");
    sleep(6);
    
    // 停止添加任务，等待缩容
    printf("Waiting for scaling down...\n");
    for (int i = 0; i < 20; i++) {
        int live_threads, busy_threads, task_count;
        thread_pool_get_status(pool, &live_threads, &busy_threads, &task_count);
        printf("Monitor %d: threads=%d, busy=%d, tasks=%d\n", 
               i + 1, live_threads, busy_threads, task_count);
        sleep(2);
        
        // 如果线程数回到最小值就结束
        if (live_threads == 2 && task_count == 0) {
            printf("Scaled down to minimum threads, ending test\n");
            break;
        }
    }
    
    printf("Destroying thread pool...\n");
    thread_pool_destroy(pool);
    printf("Dynamic scaling down test completed\n");
    printf("=====================================\n\n");
}

/* 并发性能测试 */
void test_concurrent_performance(void) {
    printf("=====================================\n");
    printf("       并发性能测试开始\n");
    printf("=====================================\n");
    
    thread_pool_t *pool = thread_pool_create(4, 12, 2);
    if (pool == NULL) {
        printf("Failed to create thread pool\n");
        return;
    }
    
    clock_t start_time = clock();
    
    // 添加中等数量的不同类型任务
    printf("Adding 30 mixed tasks...\n");
    for (int i = 0; i < 30; i++) {
        if (i % 3 == 0) {
            // 计算任务
            test_task_data_t *data = malloc(sizeof(test_task_data_t));
            data->task_id = i + 1;
            data->sleep_time = 1;
            snprintf(data->message, sizeof(data->message), 
                    "Performance test compute task %d", data->task_id);
            thread_pool_add_task(pool, compute_task, data);
        } else if (i % 3 == 1) {
            // 打印任务
            char *message = malloc(256);
            snprintf(message, 256, "Performance test print task %d", i + 1);
            thread_pool_add_task(pool, print_task, message);
        } else {
            // 文件任务
            int *file_id = malloc(sizeof(int));
            *file_id = i + 1;
            thread_pool_add_task(pool, file_task, file_id);
        }
        
        // 每10个任务暂停一下
        if ((i + 1) % 10 == 0) {
            usleep(100000);  // 100ms
            
            int current_threads, busy_threads, task_count;
            thread_pool_get_status(pool, &current_threads, &busy_threads, &task_count);
            printf("Progress: %d/30 tasks added, threads=%d, busy=%d, queued=%d\n", 
                   i + 1, current_threads, busy_threads, task_count);
        }
    }
    
    // 等待所有任务完成
    printf("Waiting for all tasks to complete...\n");
    int task_count;
    int monitor_count = 0;
    do {
        int current_threads, busy_threads;
        thread_pool_get_status(pool, &current_threads, &busy_threads, &task_count);
        printf("Status: threads=%d, busy=%d, remaining=%d\n", 
               current_threads, busy_threads, task_count);
        sleep(2);
        monitor_count++;
        
        // 避免无限等待
        if (monitor_count > 15) {
            printf("Timeout waiting for tasks, ending test\n");
            break;
        }
    } while (task_count > 0 || monitor_count < 3);
    
    // 等待最后的任务完成
    sleep(3);
    
    clock_t end_time = clock();
    double elapsed_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    
    printf("Performance test completed in %.2f seconds\n", elapsed_time);
    
    printf("Destroying thread pool...\n");
    thread_pool_destroy(pool);
    printf("=====================================\n\n");
}

/* 错误处理测试 */
void test_error_handling(void) {
    printf("=====================================\n");
    printf("       错误处理测试开始\n");
    printf("=====================================\n");
    
    // 测试无效参数
    printf("Testing invalid parameters...\n");
    thread_pool_t *pool1 = thread_pool_create(0, 5, 1);  // 无效最小线程数
    thread_pool_t *pool2 = thread_pool_create(5, 3, 1);  // 最小 > 最大
    thread_pool_t *pool3 = thread_pool_create(-1, 5, 1); // 负数参数
    
    if (pool1 == NULL && pool2 == NULL && pool3 == NULL) {
        printf("✓ Invalid parameter handling works correctly\n");
    } else {
        printf("✗ Invalid parameter handling failed\n");
    }
    
    // 测试正常线程池
    thread_pool_t *pool = thread_pool_create(2, 5, 2);
    if (pool == NULL) {
        printf("Failed to create valid thread pool\n");
        return;
    }
    
    // 测试空指针参数
    printf("Testing NULL pointer handling...\n");
    if (thread_pool_add_task(NULL, print_task, NULL) == -1) {
        printf("✓ NULL pool parameter handled correctly\n");
    }
    
    if (thread_pool_add_task(pool, NULL, NULL) == -1) {
        printf("✓ NULL function parameter handled correctly\n");
    }
    
    // 测试关闭后添加任务
    printf("Testing add task after shutdown...\n");
    printf("Destroying thread pool...\n");
    thread_pool_destroy(pool);
    sleep(2);
    
    char *message = malloc(256);
    strcpy(message, "This should fail");
    if (thread_pool_add_task(pool, print_task, message) == -1) {
        printf("✓ Add task after shutdown handled correctly\n");
        free(message);
    } else {
        printf("✗ Add task after shutdown not handled correctly\n");
    }
    
    printf("Error handling test completed\n");
    printf("=====================================\n\n");
}

/* 清理测试文件 */
void cleanup_test_files(void) {
    printf("\n=== 清理测试文件 ===\n");
    
    char command[256];
    snprintf(command, sizeof(command), "rm -f test_file_*.txt");
    if (system(command) == 0) {
        printf("Test files cleaned up\n");
    } else {
        printf("Failed to clean up test files\n");
    }
}

/* 主测试函数 */
int main(void) {
    printf("======================================\n");
    printf("         线程池功能测试开始\n");
    printf("======================================\n");
    
    // 运行各项测试
    test_basic_functionality();
    test_dynamic_scaling_up();
    test_dynamic_scaling_down();
    test_concurrent_performance();
    test_error_handling();
    
    // 清理
    cleanup_test_files();
    
    printf("\n======================================\n");
    printf("         所有测试完成\n");
    printf("======================================\n");
    
    return 0;
} 
