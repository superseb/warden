#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <unistd.h>

#include "status_writer.h"
#include "util.h"

typedef enum {
  STATE_CREATED,
  STATE_STARTED,
  STATE_DONE,
} status_writer_state_t;

typedef struct status_sink_s status_sink_t;

struct status_sink_s {
  int fd;

  LIST_ENTRY(status_sink_s) next_sink;
};

LIST_HEAD(status_sink_head, status_sink_s);

struct status_writer_s {
  uint8_t               status;
  status_writer_state_t state;
  pthread_mutex_t       state_lock;

  int accept_fd;                /* where new connections are created */
  int acceptor_stop_pipe[2];

  struct status_sink_head sinks; /* where data is written to */
};

static status_sink_t *status_sink_alloc(int sink_fd) {
  status_sink_t *sink = NULL;

  assert(sink_fd >= 0);

  sink = calloc(1, sizeof(*sink));
  assert(NULL != sink);

  sink->fd = sink_fd;

  return sink;
}

static void status_sink_free(status_sink_t *sink) {
  assert(NULL != sink);

  free(sink);
}

static void status_writer_free_sinks(status_writer_t *sw) {
  status_sink_t *cur = NULL;
  status_sink_t *prev = NULL;

  assert(NULL != sw);

  cur = LIST_FIRST(&(sw->sinks));

  while (NULL != cur) {
    prev = cur;
    cur = LIST_NEXT(cur, next_sink);
    LIST_REMOVE(prev, next_sink);
    status_sink_free(prev);
  }
}

status_writer_t *status_writer_alloc(int accept_fd) {
  status_writer_t *sw = NULL;
  int err = 0;

  assert(accept_fd >= 0);

  sw = calloc(1, sizeof(*sw));
  assert(NULL != sw);

  sw->accept_fd = accept_fd;
  err = set_nonblocking(sw->accept_fd);
  assert(!err);

  LIST_INIT(&(sw->sinks));

  if (-1 == pipe(sw->acceptor_stop_pipe)) {
    perror("pipe()");
    assert(0);
  }
  err = set_nonblocking(sw->acceptor_stop_pipe[0]);
  assert(!err);

  sw->state = STATE_CREATED;
  err = pthread_mutex_init(&(sw->state_lock), NULL);
  assert(!err);
  sw->status = -1;

  return sw;
}

void status_writer_run(status_writer_t *sw) {
  uint8_t events, hup;
  uint8_t status, sink_fd;
  status_sink_t *sink = NULL;

  assert(NULL != sw);

  checked_lock(&(sw->state_lock));

  assert(STATE_CREATED == sw->state);
  sw->state = STATE_STARTED;

  checked_unlock(&(sw->state_lock));

  while (1) {
    events = wait_readable_or_stop(sw->accept_fd, sw->acceptor_stop_pipe[0]);

    if (events & MUXER_READABLE) {
      sink_fd = accept(sw->accept_fd, NULL, NULL);
      if (-1 == sink_fd) {
        perror("accept()");
      } else {
        sink = status_sink_alloc(sink_fd);
        LIST_INSERT_HEAD(&(sw->sinks), sink, next_sink);
      }
    }

    if (events & MUXER_STOP) {
      close(sw->accept_fd);
      break;
    }
  }

  checked_lock(&(sw->state_lock));

  status = sw->status;

  checked_unlock(&(sw->state_lock));

  /* Write out the status to each sink */
  LIST_FOREACH(sink, &(sw->sinks), next_sink) {
    atomic_write(sink->fd, &(status), sizeof(uint8_t), &hup);
    close(sink->fd);
  }

  close(sw->acceptor_stop_pipe[0]);
  close(sw->acceptor_stop_pipe[1]);
}

void status_writer_finish(status_writer_t *sw, uint8_t status) {
  uint8_t hup;

  checked_lock(&(sw->state_lock));

  assert(STATE_STARTED == sw->state);

  sw->state = STATE_DONE;
  sw->status = status;

  checked_unlock(&(sw->state_lock));

  /* Notify thread executing status_writer_run() */
  atomic_write(sw->acceptor_stop_pipe[1], "x", 1, &hup);
}

void status_writer_free(status_writer_t *sw) {
  assert(NULL != sw);

  pthread_mutex_destroy(&(sw->state_lock));
  status_writer_free_sinks(sw);
  free(sw);
}