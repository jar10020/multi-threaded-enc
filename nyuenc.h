#ifndef HEADER_FILE
#define HEADER_FILE

#include <pthread.h>

void encodeChunk(unsigned char* start, int size, int taskId, pthread_mutex_t* mutex, pthread_cond_t* cond);

void *doWork();

void addTaskToQueue(int id, pthread_mutex_t* mutex, pthread_cond_t* cond);
struct task* takeTaskFromQueue(pthread_mutex_t* mutex);

struct task {
    int id;
    struct task* next;
};

struct encodedTask {
    int id;
    struct encodedTask* next;
};

void addEncodedTaskToQueue(int id, pthread_mutex_t* mutex, pthread_cond_t* cond);
struct encodedTask* findEncodedTaskInQueue(int searchId, pthread_mutex_t* mutex);

#endif