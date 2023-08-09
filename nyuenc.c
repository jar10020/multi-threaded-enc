#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#include "nyuenc.h"

unsigned char* chunkStarts[256000];
int chunkSizes[256000];

unsigned char* encStarts[256000];
int encSizes[256000];

/*
Mutexes and cond variables for encoding work
*/
//mutex and cond variable for multithreaded optioin
pthread_mutex_t getWorkMutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
pthread_cond_t workInQueueCond = PTHREAD_COND_INITIALIZER;
int workQueueSize = 0; //keep track of how many works in the work queue

//work queue linked list init
struct task* workQueue = NULL;
struct task* workQueueTail = NULL;


/*
Mutexes and cond variables for stitching work
*/
pthread_mutex_t getEncodedWorkMutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
pthread_cond_t workInEncodedQueueCond = PTHREAD_COND_INITIALIZER;
int encodedWorkQueueSize = 0;

//queue linked list init
struct encodedTask* encodedQueue = NULL;
struct encodedTask* encodedQueueTail = NULL;


int main(int argc, char *argv[]) {

    //Check if we're multithreading or running sequentially
    //https://www.gnu.org/software/libc/manual/html_node/Example-of-Getopt.html
    if (getopt(argc, argv, "j:") == -1) {

        /*
        SEQUENTIALLY RUNNING NO MULTITHREADING
        */

        //argv[1] is the first arg supplied, argv[0] is the command itself

        unsigned char* allEncs[argc - 1];
        int encEnds[argc-1];

        for(int i = 1; i < argc; i++) {

            //read input file
            // Open file
            int fd = open(argv[i], O_RDONLY);
            if (fd == -1) {
                //handle_error();
                printf("error\n");
            }   

            // Get file size
            struct stat sb;
            if (fstat(fd, &sb) == -1) {
                //handle_error();
                printf("error\n");
            }

            // Map file into memory
            unsigned char *addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (addr == MAP_FAILED) {
                //handle_error();
                printf("error\n");
            }
            
            //ENCODING

            //allocating enough memory in buffer for encoded string
            unsigned char* enc = malloc(sb.st_size * 2);
            
            int encPtr = 0;
            int curCount = 1;

            if(sb.st_size == 1) {
                enc[encPtr] = addr[0];
                enc[encPtr+1] = 1;
                encPtr+= 2;
            } else {
                for(int i = 1; i < sb.st_size; i++) {
                    if(addr[i-1] == addr[i]) {
                        curCount++;
                        continue;
                    } else {
                        /* whenever we reach the end of a group of same characters, add the char and its amount to
                        the next available spot in enc, in a pair. then increase encPtr by 2 to the next blank spot. */
                        enc[encPtr] = addr[i-1];
                        enc[encPtr+1] = curCount;
                        encPtr+= 2;
                        curCount = 1;
                    }
                    
                }
                enc[encPtr] = addr[sb.st_size - 1];
                enc[encPtr+1] = curCount;
                encPtr+= 2;
            }

            allEncs[i-1] = enc; //save this enc globally
            encEnds[i-1] = encPtr; //remember the index of the last character count in this enc (NOT INCLUSIVE)
            //this is the first byte AFTER the arr has ended.
        }

        //Stitch
        for(int i = 0; i < argc - 2; i++) { //argc-1 is size of allEncs[], and we need to access the i and i+1th item, so we -2

            // fprintf(stderr, "%d\n", allEncs[i][encEnds[i]-2]);
            // fprintf(stderr, "%d\n", allEncs[i+1][0]);

            if(allEncs[i][encEnds[i]-2] == allEncs[i+1][0]) {
                //set first count byte of next string to the sum of both count bytes
                int sum = allEncs[i][encEnds[i]-1] + allEncs[i+1][1];
                allEncs[i+1][1] = sum;
                //set end two forward to delete last two now unnecessary bytes of this enc
                encEnds[i] -= 2;
            }
        }

        //write to stdout
        for(int i = 0; i < argc - 1; i++) {
            fwrite(allEncs[i], 1, (encEnds[i]), stdout);
        }
        fflush(stdout);
        
    } else {

        //MULTITHREADED VERSION OF THE PROGRAM

        //mutex and thread basic logic somewhat inspired by 
        //http://www.cs.kent.edu/~ruttan/sysprog/lectures/multi-thread/multi-thread.html#definition

        int threadCount = atoi(optarg);

        int totalChunks = 0;
        int collectedChunks = 0;

        //making our threads
        //inspired by proc-2 slide 263
        pthread_t tid[threadCount];
        for(int i = 0; i < threadCount; i++) {
            pthread_create(&tid[i], NULL, doWork, NULL);
        }


        //---------------------------------------------------------------------------------------------
        //Opening files and making chunks

        for(int i = 3; i < argc; i++) { //open each file one by one
            int fd = open(argv[i], O_RDONLY);
            if (fd == -1) {
                //handle_error();
                printf("error\n");
            }   
            // Get file size
            struct stat sb;
            if (fstat(fd, &sb) == -1) {
                //handle_error();
                printf("error\n");
            }
            // Map file into memory
            unsigned char *addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (addr == MAP_FAILED) {
                //handle_error();
                printf("error\n");
            }

            //MAKE CHUNKS OUT OF FILE
            if(sb.st_size < 4096) {
                chunkStarts[totalChunks] = addr;
                chunkSizes[totalChunks] = sb.st_size;
                addTaskToQueue(totalChunks, &getWorkMutex, &workInQueueCond); //add this chunk to queue, w/ current index as id
                totalChunks++;
            } else {
                // int leftover = 0;
                // int lastj = 0;
                // for(int j = 0; j < sb.st_size; j+= 4096) {
                //     chunkStarts[totalChunks] = addr+j;
                //     chunkSizes[totalChunks] = 4096;

                //     addTaskToQueue(totalChunks, &getWorkMutex, &workInQueueCond); //add this chunk to queue, w/ current index as id
                //     totalChunks++;

                //     leftover = sb.st_size-j;
                //     lastj = j+4096;
                // }
                // if(leftover > 0) {
                //     chunkStarts[totalChunks] = addr+lastj;
                //     chunkSizes[totalChunks] = leftover;

                //     addTaskToQueue(totalChunks, &getWorkMutex, &workInQueueCond); //add this chunk to queue, w/ current index as id
                //     totalChunks++;
                // }
                int lastEnd = 0;
                for(int j = 4096; j < sb.st_size; j+=4096) {
                    chunkStarts[totalChunks] = addr+lastEnd;
                    chunkSizes[totalChunks] = 4096;

                    addTaskToQueue(totalChunks, &getWorkMutex, &workInQueueCond);
                    totalChunks++;
                    lastEnd = j;
                }
                if(sb.st_size - lastEnd > 0) { //for last chunk, less than 4096
                    chunkStarts[totalChunks] = addr+lastEnd;
                    chunkSizes[totalChunks] = sb.st_size - lastEnd;

                    addTaskToQueue(totalChunks, &getWorkMutex, &workInQueueCond);
                    totalChunks++;
                }
            }
        }

        // fwrite(chunkStarts[0], 1, chunkSizes[0], stderr);
        // fwrite(chunkStarts[1], 1, chunkSizes[1], stderr);

        // for(int i = 0; i < 258; i++) {
        //     fprintf(stderr, "Chunk num: %d. Size is %d\n",i, chunkSizes[i]);
        // }

        // fwrite(chunkStarts[totalChunks-1], 1, chunkSizes[totalChunks-1], stdout);
        // printf("\n-----------------\n");
        // fwrite(chunkStarts[totalChunks-2], 1, chunkSizes[totalChunks-2], stdout);
        // exit(1);
        
        // fprintf(stderr, "\nTotal chunks: %d\n", totalChunks);


        //----------------------------------------------------------------------------------------
        //GET ENCODED CHUNKS AND FWRITE THEM TO OUTPUT

        // struct encodedTask* curEnc;
        int lastChunkWritten = -1; //inspired by tutor's idea in discord
        int curChunk = 0;
        pthread_mutex_lock(&getEncodedWorkMutex);
        while (1) {
            if(encodedWorkQueueSize > 0) {
                if(findEncodedTaskInQueue(curChunk, &getEncodedWorkMutex) != NULL) {
                    
                    if(totalChunks == 1) {
                        fwrite(encStarts[0], 1, encSizes[0], stdout);
                    }

                    if(curChunk == 0) { //write 0 to n-2
                        // fprintf(stderr, "Writing first chunk\n");

                        fwrite(encStarts[curChunk], 1, encSizes[curChunk]-2, stdout);
                    } else if(curChunk < totalChunks - 1) {
                        // fprintf(stderr, "Writing middle chunk id: %d\n", curChunk);

                        //check this chunk's first character against last chunk's last character and stitch if necessary
                        if(encStarts[curChunk][0] == encStarts[lastChunkWritten][encSizes[lastChunkWritten]-2]) {
                            //stitch
                            int sum = encStarts[lastChunkWritten][encSizes[lastChunkWritten]-1] + encStarts[curChunk][1];
                            encStarts[curChunk][1] = sum;
                            fwrite(encStarts[curChunk], 1, encSizes[curChunk]-2, stdout);
                        } else {
                            //don't stitch
                            fwrite(encStarts[lastChunkWritten]+(encSizes[lastChunkWritten]-2), 1, 2, stdout); //write last two of prev
                            fwrite(encStarts[curChunk], 1, encSizes[curChunk]-2, stdout); //write all but last two of this chunk
                        }

                    } else if (curChunk == totalChunks - 1) {
                        // fprintf(stderr, "Writing last chunk id=%d\n",curChunk);
                        // fprintf(stderr, "Last chunk size: %d\n", encSizes[curChunk]);
                        // fprintf(stderr, "Last chunk final character: %c\n", encStarts[curChunk][encSizes[curChunk]-2]);

                        //check this chunk's first character against last chunk's last character and stitch if necessary
                        if(encStarts[curChunk][0] == encStarts[lastChunkWritten][encSizes[lastChunkWritten]-2]) {
                            //stitch
                            int sum = encStarts[lastChunkWritten][encSizes[lastChunkWritten]-1] + encStarts[curChunk][1];
                            encStarts[curChunk][1] = sum;
                            fwrite(encStarts[curChunk], 1, encSizes[curChunk], stdout); //skip last 2 of last chunk, and write to end
                        } else {
                            //don't stitch
                            fwrite(encStarts[lastChunkWritten]+(encSizes[lastChunkWritten]-2), 1, 2, stdout); //write last two of prev
                            fwrite(encStarts[curChunk], 1, encSizes[curChunk], stdout); //write rest of this chunk
                        }
                    }

                    curChunk++;
                    lastChunkWritten++;
                    collectedChunks++;
                }
                // curEnc = findEncodedTaskInQueue(0, &getEncodedWorkMutex); //get task
                // int thisTaskId = curEnc->id;
                // fwrite(encStarts[thisTaskId], 1, encSizes[thisTaskId], stdout);
                
                
            } else {
                if(collectedChunks >= totalChunks) {
                    exit(0);
                }
                pthread_cond_wait(&workInEncodedQueueCond, &getEncodedWorkMutex);
            }
        }

        // pleaseWork = findEncodedTaskInQueue(0, &getEncodedWorkMutex);
    }
}


void *doWork() {

    // fprintf(stderr, "I am a thread doing work\n");

    struct task* taskToDo;
    //work loop
    pthread_mutex_lock(&getWorkMutex);
    
    while (1) {
        if(workQueueSize > 0) {
            taskToDo = takeTaskFromQueue(&getWorkMutex); //get task
            int thisTaskId = taskToDo->id;

            //encode
            encodeChunk(chunkStarts[thisTaskId], chunkSizes[thisTaskId], thisTaskId, &getEncodedWorkMutex, &workInEncodedQueueCond);
        } else {
            pthread_cond_wait(&workInQueueCond, &getWorkMutex);
        }
    }
}


void addTaskToQueue(int id, pthread_mutex_t* mutex, pthread_cond_t* cond) {
    //create new task struct
    struct task* newTask;
    newTask = (struct task*) malloc(sizeof(struct task));

    newTask->id = id;
    newTask->next = NULL; //default next is NULL because it is added to end of list

    // fprintf(stderr, "addTaskToQ id: %d \n", id);

    //now we need to add it to the queue, and to touch the queue we need to mutex always
    pthread_mutex_lock(mutex);

    //standard linkedlist adding
    if(workQueueSize != 0) { //default, at least one item already in linkedlist
        workQueueSize++;
        workQueueTail->next = newTask;
        workQueueTail = newTask;
    } else { //if this is the first thing we're adding
        workQueueSize++;
        workQueue = newTask;
        workQueueTail = newTask;
    }
    pthread_cond_signal(cond);
    pthread_mutex_unlock(mutex); //unlock since we are done editing the workQueue

    // fprintf(stderr, "SUCCESS addTaskToQ id: %d \n", id);

     //signal cond variable
    return;
}

struct task* takeTaskFromQueue(pthread_mutex_t* mutex) {
    //im just going to assume that there is a task in the queue if this is called,
    //so it's on the calling function to check that workQueueSize > 0

    pthread_mutex_lock(mutex); //lock the mutex so we can edit the queue
    struct task* returnedTask = workQueue;
    workQueue = workQueue->next;
    //if we removed the last item from the list
    if(workQueue == NULL)
        workQueueTail = NULL;
    
    pthread_mutex_unlock(mutex); //unlock mutex now that we've removed
    workQueueSize--;

    // fprintf(stderr, "SUCCESS RETURNING takeTaskFromQ id: %d \n", returnedTask->id);

    return returnedTask;
}


void encodeChunk(unsigned char* start, int size, int taskId, pthread_mutex_t* mutex, pthread_cond_t* cond) {

    // fprintf(stderr, "Encoding chunk:%d\n", taskId);

    unsigned char* enc = malloc(size * 2);
    
    int encPtr = 0;
    int curCount = 1;

    if(size == 1) {
        enc[encPtr] = start[0];
        enc[encPtr+1] = 1;
        encPtr+= 2;
    } else {
        for(int i = 1; i < size; i++) {
            // fprintf(stderr, "%d.", taskId);
            if(start[i-1] == start[i]) {
                curCount++;
                continue;
            } else {
                /* whenever we reach the end of a group of same characters, add the char and its amount to
                the next available spot in enc, in a pair. then increase encPtr by 2 to the next blank spot. */
                enc[encPtr] = start[i-1];
                enc[encPtr+1] = curCount;
                encPtr+= 2;
                curCount = 1;
            }
            
        }
        enc[encPtr] = start[size - 1];
        enc[encPtr+1] = curCount;
        encPtr+= 2;
    }

    // fprintf(stderr, "Encoded chunk %d, original size of %d, reduced to %d \n", taskId, size, encPtr);
    // allEncs[i-1] = enc; //save this enc globally
    // encEnds[i-1] = encPtr; //remember the index of the last character count in this enc (NOT INCLUSIVE)
    // //this is the first byte AFTER the arr has ended.
    pthread_mutex_lock(mutex);
    encStarts[taskId] = enc;
    encSizes[taskId] = encPtr;
    addEncodedTaskToQueue(taskId, mutex, cond);
    pthread_mutex_unlock(mutex);
    // pthread_cond_signal(cond);
    // fwrite(enc, 1, encPtr, stdout);
    // fprintf(stderr, "sizesPointer is now:%d \n", sizesPointer[taskId]);

    return;
}


void addEncodedTaskToQueue(int id, pthread_mutex_t* mutex, pthread_cond_t* cond) {
    //create new task struct
    struct encodedTask* newTask;
    newTask = (struct encodedTask*) malloc(sizeof(struct encodedTask));

    newTask->id = id;
    newTask->next = NULL; //default next is NULL because it is added to end of list

    // fprintf(stderr, "addTaskToQ id: %d \n", id);

    //now we need to add it to the queue, and to touch the queue we need to mutex always
    pthread_mutex_lock(mutex);

    //standard linkedlist adding
    if(encodedWorkQueueSize != 0) { //default, at least one item already in linkedlist
        encodedWorkQueueSize++;
        encodedQueueTail->next = newTask;
        encodedQueueTail = newTask;
    } else { //if this is the first thing we're adding
        encodedWorkQueueSize++;
        encodedQueue = newTask;
        encodedQueueTail = newTask;
    }
    pthread_cond_signal(cond);
    pthread_mutex_unlock(mutex); //unlock since we are done editing the workQueue

    // fprintf(stderr, "SUCCESS addTaskToQ id: %d \n", id);
     //signal cond variable
    return;
}

struct encodedTask* findEncodedTaskInQueue(int searchId, pthread_mutex_t* mutex) {
    //im just going to assume that there is a task in the queue if this is called,
    //so it's on the calling function to check that workQueueSize > 0

    struct encodedTask* returnedTask = NULL;

    pthread_mutex_lock(mutex);

    //find the task we want
    struct encodedTask* curEncTask = encodedQueue;

    //this will find and remove it if it's in the middle or the tail, but we still need to check the head
    while(curEncTask->next != NULL) {
        if(curEncTask->next->id == searchId) {
            returnedTask = curEncTask->next;
            //remove from linkedlist
            
            curEncTask->next = curEncTask->next->next;
            if(curEncTask->next == NULL) {
                encodedQueueTail = curEncTask;
            }

            encodedWorkQueueSize--;
            break;
        } else {
            curEncTask = curEncTask->next;
        }
    }

    if(encodedQueue->id == searchId) {
        returnedTask = encodedQueue;
        // encodedQueue = encodedQueue->next; //we should prob remove but it's too much work rn and tbh i dont even think we need to
        if(encodedQueue == NULL) {
            encodedQueueTail = NULL;
        }
        encodedWorkQueueSize--;
    }
    
    pthread_mutex_unlock(mutex); //unlock mutex now that we've removed

    // fprintf(stderr, "SUCCESS RETURNING takeTaskFromQ id: %d \n", returnedTask->id);

    return returnedTask;
}