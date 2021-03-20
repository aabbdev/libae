/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __AE_H__
#define __AE_H__

#define AE_OK 0
#define AE_ERR -1

#define AE_NONE 0       /* No events registered. */
#define AE_READABLE 1   /* Fire when descriptor is readable. */
#define AE_WRITABLE 2   /* Fire when descriptor is writable. */
#define AE_BARRIER 4    /* With WRITABLE, never fire the event if the
                           READABLE event already fired in the same event
                           loop iteration. Useful when you want to persist
                           things to disk before sending replies, and want
                           to do that in a group fashion. */

#define AE_FILE_EVENTS 1
#define AE_TIME_EVENTS 2
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT 4
#define AE_CALL_AFTER_SLEEP 8

#define AE_NOMORE -1
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

namespace ae
{
    /* Types and data structures */
    typedef void FileProc(int fd, void *clientData, int mask);
    typedef int TimeProc(long long id, void *clientData);
    typedef void EventFinalizerProc(void *clientData);
    typedef void BeforeSleepProc();

    /* File event structure */
    typedef struct FileEvent
    {
        int mask; /* one of AE_(READABLE|WRITABLE) */
        FileProc *rfileProc;
        FileProc *wfileProc;
        void *clientData;
    } FileEvent;

    /* Time event structure */
    typedef struct TimeEvent {
        long long id; /* time event identifier. */
        long when_sec; /* seconds */
        long when_ms; /* milliseconds */
        TimeProc *timeProc;
        EventFinalizerProc *finalizerProc;
        void *clientData;
        struct TimeEvent *prev;
        struct TimeEvent *next;
    } TimeEvent;

    /* A fired event */
    typedef struct FiredEvent
    {
        int fd;
        int mask;
    } FiredEvent;

    /* State of an event based program */
    /*
        TODO: To be work with private/public/..
     */
    class EventLoop
    {
    public:
        EventLoop(int setsize);
        ~EventLoop();
        void Start();
        void Stop();
        int CreateFileEvent(int fd, int mask,
                            FileProc *proc, void *clientData);
        void DeleteFileEvent(int fd, int mask);
        int GetFileEvents(int fd);
        long long CreateTimeEvent(long long milliseconds,
                                  TimeProc *proc, void *clientData,
                                  EventFinalizerProc *finalizerProc);
        int DeleteTimeEvent(long long id);
        int ProcessEvents(int flags);
        int Wait(int fd, int mask, long long milliseconds);
        void SetBeforeSleepProc(BeforeSleepProc *beforesleep);
        void SetAfterSleepProc(BeforeSleepProc *aftersleep);
        int GetSetSize();
        int ResizeSetSize(int setsize);
        TimeEvent *SearchNearestTimer();
        int processTimeEvents();
        int maxfd;   /* highest file descriptor currently registered */
        int setsize; /* max number of file descriptors tracked */
        long long timeEventNextId;
        time_t lastTime;     /* Used to detect system clock skew */
        FileEvent *events; /* Registered events */
        FiredEvent *fired; /* Fired events */
        TimeEvent *timeEventHead;
        int stop;
        void* apidata; /* This is used for polling API specific data */
        BeforeSleepProc *beforesleep;
        BeforeSleepProc *aftersleep;
        char* GetApiName();
    };
}
#endif
