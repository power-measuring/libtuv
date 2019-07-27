
/* We lean on the fact that POLL{IN,OUT,ERR,HUP} correspond with their
 * EPOLL* counterparts.  We use the POLL* variants in this file because that
 * is what libuv uses elsewhere and it avoids a dependency on <sys/epoll.h>.
 */

#include "uv.h"
#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <net/if.h>
#include <sys/param.h>
#include <sys/prctl.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define HAVE_IFADDRS_H 1

#ifdef __UCLIBC__
# if __UCLIBC_MAJOR__ < 0 && __UCLIBC_MINOR__ < 9 && __UCLIBC_SUBLEVEL__ < 32
#  undef HAVE_IFADDRS_H
# endif
#endif

#ifdef HAVE_IFADDRS_H
# if defined(__ANDROID__)
#  include "android-ifaddrs.h"
# else
#  include <ifaddrs.h>
# endif
# include <sys/socket.h>
# include <net/ethernet.h>
# include <netpacket/packet.h>
#endif /* HAVE_IFADDRS_H */

/* Available from 2.6.32 onwards. */
#ifndef CLOCK_MONOTONIC_COARSE
# define CLOCK_MONOTONIC_COARSE 6
#endif

/* This is rather annoying: CLOCK_BOOTTIME lives in <linux/time.h> but we can't
 * include that file because it conflicts with <time.h>. We'll just have to
 * define it ourselves.
 */
#ifndef CLOCK_BOOTTIME
# define CLOCK_BOOTTIME 7
#endif
static void uv__add_pollfd(uv_loop_t* loop, struct pollfd* pe) {
  int i;
  int exist = 0;
  int free_idx = -1;
  for (i = 0; i < loop->npollfds; ++i) {
    struct pollfd* cur = &loop->pollfds[i];
    if (cur->fd == pe->fd) {
      cur->events = pe->events;
      exist = 1;
      break;
    }
    if (cur->fd == -1) {
      free_idx = i;
    }
  }
  if (!exist) {
    if (free_idx == -1) {
      free_idx = loop->npollfds++;
      if (free_idx >= TUV_POLL_EVENTS_SIZE)
      {
        TDLOG("uv__add_pollfd abort, because loop->npollfds (%d) reached maximum size", free_idx);
        ABORT();
      }
    }
    struct pollfd* cur = &loop->pollfds[free_idx];

    cur->fd = pe->fd;
    cur->events = pe->events;
    cur->revents = 0;
  }
}
static void uv__rem_pollfd(uv_loop_t* loop, struct pollfd* pe) {
  int i = 0;
  while (i < loop->npollfds) {
    struct pollfd* cur = &loop->pollfds[i];
    if (cur->fd == pe->fd) {
      *cur = loop->pollfds[--loop->npollfds];
    } else {
      ++i;
    }
  }
}

int uv__platform_loop_init(uv_loop_t* loop) {
  loop->npollfds = 0;
  return 0;
}

void uv__platform_loop_delete(uv_loop_t* loop) {
  loop->npollfds = 0;
}


//-----------------------------------------------------------------------------

void uv__platform_invalidate_fd(uv_loop_t* loop, int fd) {
  int i;
  int nfd = loop->npollfds;
  for (i = 0; i < nfd; ++i) {
    struct pollfd* pfd = &loop->pollfds[i];
    if (fd == pfd->fd) {
      pfd->fd = -1;
    }
  }
}
void uv__io_poll(uv_loop_t* loop, int timeout) {
  struct pollfd pfd;
  struct pollfd* pe;
  QUEUE* q;
  uv__io_t* w;
  uint64_t base;
  uint64_t diff;
  int nevents;
  int count;
  int nfd;
  int i;
//puts("poll begins! ");
  if (loop->nfds == 0) {
    assert(QUEUE_EMPTY(&loop->watcher_queue));
    return;
  }

  while (!QUEUE_EMPTY(&loop->watcher_queue)) {
    q = QUEUE_HEAD(&loop->watcher_queue);
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);

    w = QUEUE_DATA(q, uv__io_t, watcher_queue);
    assert(w->pevents != 0);
    assert(w->fd >= 0);
    assert(w->fd < (int)loop->nwatchers);

    pfd.fd = w->fd;
    pfd.events = w->pevents;
    uv__add_pollfd(loop, &pfd);

    w->events = w->pevents;
  }

  assert(timeout >= -1);
  base = loop->time;
  count = 5;
  
  for (;;) {
//      for(int i = 0; i < loop->npollfds; i++) {
//          printf("poll nfds: %d, fd %d\n", i,loop->pollfds[i]);
//        }
    nfd = poll(loop->pollfds, loop->npollfds, timeout);

    SAVE_ERRNO(uv__update_time(loop));

    if (nfd == 0) {
      assert(timeout != -1);
      return;
    }

    if (nfd == -1) {
      int err = errno;
      if (err == EAGAIN) {
          errno = 0;
      }
      else if (err != EINTR) {
        // poll of which the watchers is null should be removed
        TDLOG("uv__io_poll abort for errno(%d)", err);
        goto handle_poll;
      }
      if (timeout == -1) {
        continue;
      }
      if (timeout == 0) {
        return;
      }
      goto update_timeout;
    }

handle_poll:
    nevents = 0;

    for (i = 0; i < loop->npollfds; ++i) {
      pe = &loop->pollfds[i];
      w = loop->watchers[pe->fd];

      if (w == NULL) {
        uv__rem_pollfd(loop, pe);
        --i;
        continue;
      }

      if (pe->fd >= 0 && (pe->revents & (POLLIN | POLLOUT | POLLHUP))) {    
        w->cb(loop, w, pe->revents);
        ++nevents;
      }
    }

    if (nevents != 0) {
      if (--count != 0) {
        timeout = 0;
        continue;
      }
      return;
    }
    if (timeout == 0) {
      return;
    }
    if (timeout == -1) {
      continue;
    }
update_timeout:
    assert(timeout > 0);

    diff = loop->time - base;
    if (diff >= (uint64_t)timeout) {
      return;
    }
    timeout -= diff;
  }
}


uint64_t uv__hrtime(uv_clocktype_t type) {
  static clock_t fast_clock_id = -1;
  struct timespec t;
  clock_t clock_id;

  /* Prefer CLOCK_MONOTONIC_COARSE if available but only when it has
   * millisecond granularity or better.  CLOCK_MONOTONIC_COARSE is
   * serviced entirely from the vDSO, whereas CLOCK_MONOTONIC may
   * decide to make a costly system call.
   */
  /* TODO(bnoordhuis) Use CLOCK_MONOTONIC_COARSE for UV_CLOCK_PRECISE
   * when it has microsecond granularity or better (unlikely).
   */
  if (type == UV_CLOCK_FAST && fast_clock_id == -1) {
    if (clock_getres(CLOCK_MONOTONIC_COARSE, &t) == 0 &&
        t.tv_nsec <= 1 * 1000 * 1000) {
      fast_clock_id = CLOCK_MONOTONIC_COARSE;
    } else {
      fast_clock_id = CLOCK_MONOTONIC;
    }
  }

  clock_id = CLOCK_MONOTONIC;
  if (type == UV_CLOCK_FAST)
    clock_id = fast_clock_id;

  if (clock_gettime(clock_id, &t))
    return 0;  /* Not really possible. */

  return t.tv_sec * (uint64_t) 1e9 + t.tv_nsec;
}

// { TUV_CHANGES@20180803:
//   Made signal build time configurable }
#if TUV_FEATURE_PROCESS
int uv_exepath(char* buffer, size_t* size) {
  ssize_t n;

  if (buffer == NULL || size == NULL || *size == 0)
    return -EINVAL;

  n = *size - 1;
  if (n > 0)
    n = readlink("/proc/self/exe", buffer, n);

  if (n == -1)
    return -errno;

  buffer[n] = '\0';
  *size = n;

  return 0;
}
#endif /* TUV_FEATURE_PROCESS */
