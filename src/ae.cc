/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "ae.h"
#include "utils.h"
#include "config.h"
using namespace ae;

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
#ifdef HAVE_EPOLL
#include "ae_epoll.c"
#else
#ifdef HAVE_KQUEUE
#include "ae_kqueue.c"
#else
#include "ae_select.c"
#endif
#endif
#endif

EventLoop::EventLoop(int setsize)
{
    int i;
    this->events = (FileEvent *)malloc(sizeof(FileEvent) * setsize);
    this->fired = (FiredEvent *)malloc(sizeof(FiredEvent) * setsize);
    if (this->events == NULL || this->fired == NULL)
        goto err;
    this->setsize = setsize;
    this->lastTime = time(NULL);
    this->timeEventHead = NULL;
    this->timeEventNextId = 0;
    this->stop = 0;
    this->maxfd = -1;
    this->beforesleep = NULL;
    this->aftersleep = NULL;
    if (aeApiCreate(this) == -1)
        goto err;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    for (i = 0; i < setsize; i++)
        this->events[i].mask = AE_NONE;
    return;
err:
    delete this;
}
EventLoop::~EventLoop()
{
    aeApiFree(this);
    free(this->events);
    free(this->fired);
}
void EventLoop::Start()
{
    this->stop = 0;
    while (!this->stop)
    {
        if (this->beforesleep != NULL)
            this->beforesleep();
        this->ProcessEvents(AE_ALL_EVENTS | AE_CALL_AFTER_SLEEP);
    }
}

/* Return the current set size. */
int EventLoop::GetSetSize()
{
    return this->setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
int EventLoop::ResizeSetSize(int setsize)
{
    int i;
    if (this->setsize == setsize)
        return AE_OK;
    if (this->maxfd >= setsize)
        return AE_ERR;
    if (aeApiResize(this, setsize) == -1)
        return AE_ERR;

    this->events = (FileEvent *)realloc(this->events, sizeof(FileEvent) * setsize);
    this->fired = (FiredEvent *)realloc(this->fired, sizeof(FiredEvent) * setsize);
    this->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    for (i = this->maxfd + 1; i < setsize; i++)
        this->events[i].mask = AE_NONE;
    return AE_OK;
}

void EventLoop::Stop()
{
    this->stop = 1;
}

int EventLoop::CreateFileEvent(int fd, int mask,
                               FileProc *proc, void *clientData)
{
    if (fd >= this->setsize)
    {
        errno = ERANGE;
        return AE_ERR;
    }
    FileEvent *fe = &this->events[fd];

    if (aeApiAddEvent(this, fd, mask) == -1)
        return AE_ERR;
    fe->mask |= mask;
    if (mask & AE_READABLE)
        fe->rfileProc = proc;
    if (mask & AE_WRITABLE)
        fe->wfileProc = proc;
    fe->clientData = clientData;
    if (fd > this->maxfd)
        this->maxfd = fd;
    return AE_OK;
}

void EventLoop::DeleteFileEvent(int fd, int mask)
{
    if (fd >= this->setsize)
        return;
    FileEvent *fe = &this->events[fd];
    if (fe->mask == AE_NONE)
        return;

    /* We want to always remove AE_BARRIER if set when AE_WRITABLE
     * is removed. */
    if (mask & AE_WRITABLE)
        mask |= AE_BARRIER;

    aeApiDelEvent(this, fd, mask);
    fe->mask = fe->mask & (~mask);
    if (fd == this->maxfd && fe->mask == AE_NONE)
    {
        /* Update the max fd */
        int j;

        for (j = this->maxfd - 1; j >= 0; j--)
            if (this->events[j].mask != AE_NONE)
                break;
        this->maxfd = j;
    }
}

int EventLoop::GetFileEvents(int fd)
{
    if (fd >= this->setsize)
        return 0;
    FileEvent *fe = &this->events[fd];

    return fe->mask;
}

long long EventLoop::CreateTimeEvent(long long milliseconds,
                                     TimeProc *proc, void *clientData,
                                     EventFinalizerProc *finalizerProc)
{
    long long id = this->timeEventNextId++;
    TimeEvent *te;

    te = (TimeEvent *)malloc(sizeof(*te));
    if (te == NULL)
        return AE_ERR;
    te->id = id;
    AddMillisecondsToNow(milliseconds, &te->when_sec, &te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->prev = NULL;
    te->next = this->timeEventHead;
    if (te->next)
        te->next->prev = te;
    this->timeEventHead = te;
    return id;
}

int EventLoop::DeleteTimeEvent(long long id)
{
    TimeEvent *te = this->timeEventHead;
    while (te)
    {
        if (te->id == id)
        {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
TimeEvent *EventLoop::SearchNearestTimer()
{
    TimeEvent *te = this->timeEventHead;
    TimeEvent *nearest = NULL;

    while (te)
    {
        if (!nearest || te->when_sec < nearest->when_sec ||
            (te->when_sec == nearest->when_sec &&
             te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* Process time events */
int EventLoop::processTimeEvents()
{
    int processed = 0;
    TimeEvent *te;
    long long maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    if (now < this->lastTime)
    {
        te = this->timeEventHead;
        while (te)
        {
            te->when_sec = 0;
            te = te->next;
        }
    }
    this->lastTime = now;

    te = this->timeEventHead;
    maxId = this->timeEventNextId - 1;
    while (te)
    {
        long now_sec, now_ms;
        long long id;

        /* Remove events scheduled for deletion. */
        if (te->id == AE_DELETED_EVENT_ID)
        {
            TimeEvent *next = te->next;
            if (te->prev)
                te->prev->next = te->next;
            else
                this->timeEventHead = te->next;
            if (te->next)
                te->next->prev = te->prev;
            if (te->finalizerProc)
                te->finalizerProc(te->clientData);
            free(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
        if (te->id > maxId)
        {
            te = te->next;
            continue;
        }
        InternalGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            id = te->id;
            retval = te->timeProc(id, te->clientData);
            processed++;
            if (retval != AE_NOMORE)
            {
                AddMillisecondsToNow(retval, &te->when_sec, &te->when_ms);
            }
            else
            {
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        te = te->next;
    }
    return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * if flags has AE_CALL_AFTER_SLEEP set, the aftersleep callback is called.
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */
int EventLoop::ProcessEvents(int flags)
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS))
        return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (this->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT)))
    {
        int j;
        TimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = this->SearchNearestTimer();
        if (shortest)
        {
            long now_sec, now_ms;

            InternalGetTime(&now_sec, &now_ms);
            tvp = &tv;

            /* How many milliseconds we need to wait for the next
             * time event to fire? */
            long long ms =
                (shortest->when_sec - now_sec) * 1000 +
                shortest->when_ms - now_ms;

            if (ms > 0)
            {
                tvp->tv_sec = ms / 1000;
                tvp->tv_usec = (ms % 1000) * 1000;
            }
            else
            {
                tvp->tv_sec = 0;
                tvp->tv_usec = 0;
            }
        }
        else
        {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            if (flags & AE_DONT_WAIT)
            {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            }
            else
            {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }

        /* Call the multiplexing API, will return only on timeout or when
         * some event fires. */
        numevents = aeApiPoll(this, tvp);

        /* After sleep callback. */
        if (this->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            this->aftersleep();

        for (j = 0; j < numevents; j++)
        {
            FileEvent *fe = &this->events[this->fired[j].fd];
            int mask = this->fired[j].mask;
            int fd = this->fired[j].fd;
            int fired = 0; /* Number of events fired for current fd. */

            /* Normally we execute the readable event first, and the writable
             * event laster. This is useful as sometimes we may be able
             * to serve the reply of a query immediately after processing the
             * query.
             *
             * However if AE_BARRIER is set in the mask, our application is
             * asking us to do the reverse: never fire the writable event
             * after the readable. In such a case, we invert the calls.
             * This is useful when, for instance, we want to do things
             * in the beforeSleep() hook, like fsynching a file to disk,
             * before replying to a client. */
            int invert = fe->mask & AE_BARRIER;

            /* Note the "fe->mask & mask & ..." code: maybe an already
             * processed event removed an element that fired and we still
             * didn't processed, so we check if the event is still valid.
             *
             * Fire the readable event if the call sequence is not
             * inverted. */
            if (!invert && fe->mask & mask & AE_READABLE)
            {
                fe->rfileProc(fd, fe->clientData, mask);
                fired++;
            }

            /* Fire the writable event. */
            if (fe->mask & mask & AE_WRITABLE)
            {
                if (!fired || fe->wfileProc != fe->rfileProc)
                {
                    fe->wfileProc(fd, fe->clientData, mask);
                    fired++;
                }
            }

            /* If we have to invert the call, fire the readable event now
             * after the writable one. */
            if (invert && fe->mask & mask & AE_READABLE)
            {
                if (!fired || fe->wfileProc != fe->rfileProc)
                {
                    fe->rfileProc(fd, fe->clientData, mask);
                    fired++;
                }
            }

            processed++;
        }
    }
    /* Check time events */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents();

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int EventLoop::Wait(int fd, int mask, long long milliseconds)
{
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE)
        pfd.events |= POLLIN;
    if (mask & AE_WRITABLE)
        pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds)) == 1)
    {
        if (pfd.revents & POLLIN)
            retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT)
            retmask |= AE_WRITABLE;
        if (pfd.revents & POLLERR)
            retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP)
            retmask |= AE_WRITABLE;
        return retmask;
    }
    else
    {
        return retval;
    }
}

void EventLoop::SetBeforeSleepProc(BeforeSleepProc *beforesleep)
{
    this->beforesleep = beforesleep;
}
void EventLoop::SetAfterSleepProc(BeforeSleepProc *aftersleep)
{
    this->aftersleep = aftersleep;
}
