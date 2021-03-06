/*
 * Copyright (C) 2015 Roy O'Young <roy2220@outlook.com>.
 */


#include "IOPoller.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

#include "Utility.h"
#include "Async.h"
#include "Logging.h"


struct IOEvent
{
    struct ListItem listItem;
    int fd;
    uint32_t flags;
    uint32_t pendingFlags;
    struct ListItem watchListHeads[2];
};


static int xepoll_create1(int);
static void xclose(int);
static void xepoll_ctl(int, int, int, struct epoll_event *);


static const uint32_t IOEventFlags[2] = {
    [IOReadable] = EPOLLIN,
    [IOWritable] = EPOLLOUT
};


void
IOPoller_Initialize(struct IOPoller *self)
{
    assert(self != NULL);
    self->fd = xepoll_create1(0);
    Vector_Initialize(&self->eventVector, sizeof(struct IOEvent *));
    MemoryPool_Initialize(&self->eventMemoryPool, sizeof(struct IOEvent));
    List_Initialize(&self->dirtyEventListHead);
}


void
IOPoller_Finalize(const struct IOPoller *self)
{
    assert(self != NULL);
    xclose(self->fd);
    Vector_Finalize(&self->eventVector);
    MemoryPool_Finalize(&self->eventMemoryPool);
}


bool
IOPoller_SetWatch(struct IOPoller *self, struct IOWatch *watch, int fd, enum IOCondition condition
                  , uintptr_t data, void (*callback)(uintptr_t))
{
    assert(self != NULL);
    assert(watch != NULL);
    assert(fd >= 0);
    assert(condition == IOReadable || condition == IOWritable);
    assert(callback != NULL);

    if (fd >= Vector_GetLength(&self->eventVector)) {
        if (!Vector_SetLength(&self->eventVector, fd + 1, true)) {
            return false;
        }
    }

    struct IOEvent **events = Vector_GetElements(&self->eventVector);
    struct IOEvent *event = events[fd];

    if (event == NULL) {
        event = MemoryPool_AllocateBlock(&self->eventMemoryPool);

        if (event == NULL) {
            return false;
        }

        event->fd = fd;
        event->flags = 0;
        event->pendingFlags = 0;
        List_Initialize(&event->watchListHeads[0]);
        List_Initialize(&event->watchListHeads[1]);
        List_Initialize(&event->listItem);
        events[fd] = event;
    }

    watch->condition = condition;
    watch->data = data;
    watch->callback = callback;
    List_InsertBack(&event->watchListHeads[condition], &watch->listItem);

    if ((event->pendingFlags & IOEventFlags[condition]) == 0) {
        event->pendingFlags |= IOEventFlags[condition];

        if (List_IsEmpty(&event->listItem)) {
            List_InsertBack(&self->dirtyEventListHead, &event->listItem);
        }
    }

    return true;
}


void
IOPoller_ClearWatch(struct IOPoller *self, const struct IOWatch *watch)
{
    assert(self != NULL);
    assert(watch != NULL);
    ListItem_Remove(&watch->listItem);

    if (ListItem_GetPrev(&watch->listItem) == ListItem_GetNext(&watch->listItem)) {
        enum IOCondition condition = watch->condition;
        struct IOEvent *event = CONTAINER_OF(ListItem_GetPrev(&watch->listItem), struct IOEvent
                                             , watchListHeads[condition]);
        event->pendingFlags &= ~IOEventFlags[condition];

        if (List_IsEmpty(&event->listItem)) {
            List_InsertBack(&self->dirtyEventListHead, &event->listItem);
        }
    }
}


void
IOPoller_ClearWatches(struct IOPoller *self, int fd)
{
    assert(self != NULL);
    assert(fd >= 0);

    if (fd >= Vector_GetLength(&self->eventVector)) {
        return;
    }

    struct IOEvent **events = Vector_GetElements(&self->eventVector);
    struct IOEvent *event = events[fd];

    if (event == NULL) {
        return;
    }

    List_Initialize(&event->watchListHeads[0]);
    List_Initialize(&event->watchListHeads[1]);
    event->pendingFlags = 0;

    if (List_IsEmpty(&event->listItem)) {
        List_InsertBack(&self->dirtyEventListHead, &event->listItem);
    }

    if (event->flags != 0) {
        struct epoll_event ev;
        xepoll_ctl(self->fd, EPOLL_CTL_DEL, fd, &ev);
        event->flags = 0;
    }
}


bool
IOPoller_Tick(struct IOPoller *self, int timeout, struct Async *async)
{
    assert(self != NULL);
    assert(async != NULL);
    struct ListItem *eventListItem = List_GetBack(&self->dirtyEventListHead);

    if (eventListItem != &self->dirtyEventListHead) {
        do {
            struct IOEvent *event = CONTAINER_OF(eventListItem, struct IOEvent, listItem);
            eventListItem = ListItem_GetPrev(eventListItem);

            if (event->flags != event->pendingFlags) {
                int op;

                if (event->flags == 0) {
                    op = EPOLL_CTL_ADD;
                } else if (event->pendingFlags == 0) {
                    op = EPOLL_CTL_DEL;
                } else {
                    op = EPOLL_CTL_MOD;
                }

                struct epoll_event ev = {
                    .events = event->pendingFlags,
                    .data.ptr = event
                };

                xepoll_ctl(self->fd, op, event->fd, &ev);
                event->flags = event->pendingFlags;
            }

            if (event->flags == 0) {
                struct IOEvent **events = Vector_GetElements(&self->eventVector);
                events[event->fd] = NULL;
                MemoryPool_FreeBlock(&self->eventMemoryPool, event);
            } else {
                List_Initialize(&event->listItem);
            }
        } while (eventListItem != &self->dirtyEventListHead);

        List_Initialize(&self->dirtyEventListHead);
    }

    struct epoll_event evs[8192];
    int n = epoll_wait(self->fd, evs, LENGTH_OF(evs), timeout);

    if (n < 0) {
        if (errno == EINTR) {
            return false;
        }

        LOG_FATAL_ERROR("`epoll_wait()` failed: %s", strerror(errno));
    }

    int i;

    for (i = 0; i < n; ++i) {
        struct IOEvent *event = evs[i].data.ptr;

        if ((evs[i].events & (IOEventFlags[0] | EPOLLERR | EPOLLHUP)) != 0) {
            struct ListItem *watchListItem;

            FOR_EACH_LIST_ITEM(watchListItem, &event->watchListHeads[0]) {
                struct IOWatch *watch = CONTAINER_OF(watchListItem, struct IOWatch, listItem);

                if (!Async_AddCall(async, watch->callback, watch->data)) {
                    return false;
                }
            }
        }

        if ((evs[i].events & (IOEventFlags[1] | EPOLLERR | EPOLLHUP)) != 0) {
            struct ListItem *watchListItem;

            FOR_EACH_LIST_ITEM(watchListItem, &event->watchListHeads[1]) {
                struct IOWatch *watch = CONTAINER_OF(watchListItem, struct IOWatch, listItem);

                if (!Async_AddCall(async, watch->callback, watch->data)) {
                    return false;
                }
            }
        }
    }

    return true;
}


static int
xepoll_create1(int flags)
{
    int fd = epoll_create1(flags);

    if (fd < 0) {
        LOG_FATAL_ERROR("`epoll_create1()` failed: %s", strerror(errno));
    }

    return fd;
}


static void
xclose(int fd)
{
    int res;

    do {
        res = close(fd);
    } while (res < 0 && errno == EINTR);

    if (res < 0) {
        LOG_ERROR("`close()` failed: %s", strerror(errno));
    }
}


static void
xepoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    if (epoll_ctl(epfd, op, fd, event) < 0) {
        LOG_FATAL_ERROR("`epoll_ctl()` failed: %s", strerror(errno));
    }
}
