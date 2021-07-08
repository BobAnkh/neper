/*
 * Copyright 2016 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "stream.h"

#include "coef.h"
#include "common.h"
#include "flow.h"
#include "print.h"
#include "socket.h"
#include "stats.h"
#include "thread.h"

static void *stream_alloc(struct thread *t)
{
        const struct options *opts = t->opts;

        if (!t->f_mbuf) {
                t->f_mbuf = malloc_or_die(opts->buffer_size, t->cb);
                if (opts->enable_write)
                        fill_random(t->f_mbuf, opts->buffer_size);
        }
        return t->f_mbuf;
}

static uint32_t stream_events(struct thread *t)
{
        const struct options *opts = t->opts;

        uint32_t events = EPOLLRDHUP;
        if (opts->enable_write)
                events |= EPOLLOUT;
        if (opts->enable_read)
                events |= EPOLLIN;
        if (opts->edge_trigger)
                events |= EPOLLET;
        return events;
}

void stream_handler(struct flow *f, uint32_t events)
{
        static const uint64_t NSEC_PER_SEC = 1000*1000*1000;

        struct neper_stat *stat = flow_stat(f);
        struct thread *t = flow_thread(f);
        void *mbuf = flow_mbuf(f);
        int fd = flow_fd(f);
        const struct options *opts = t->opts;
        ssize_t n;

        if (events & (EPOLLHUP | EPOLLRDHUP))
                return flow_delete(f);

        if (events & EPOLLIN)
                do {
                        do {
                                n = recv(fd, mbuf, opts->buffer_size,
                                         opts->recv_flags);
                        } while(n == -1 && errno == EINTR);
                        if (n == -1) {
                                if (errno != EAGAIN)
                                        PLOG_ERROR(t->cb, "read");
                                break;
                        }
                        if (n == 0) {
                                flow_delete(f);
                                return;
                        }
                        stat->event(t, stat, n, false, NULL);
                } while (opts->edge_trigger);

        if (events & EPOLLOUT)
                do {
                        n = write(fd, mbuf, opts->buffer_size);
                        if (n == -1) {
                                if (errno != EAGAIN)
                                        PLOG_ERROR(t->cb, "write");
                                return;
                        }
                        if (opts->delay) {
                                struct timespec ts;
                                ts.tv_sec  = opts->delay / NSEC_PER_SEC;
                                ts.tv_nsec = opts->delay % NSEC_PER_SEC;
                                nanosleep(&ts, NULL);
                        }
                } while (opts->edge_trigger);
}

int stream_report(struct thread *ts)
{
        const struct options *opts = ts[0].opts;
        const char *path = opts->all_samples;
        struct callbacks *cb = ts[0].cb;
        FILE *csv = NULL;

        if (!opts->enable_read)
                return 0;

        if (path)
                csv = print_header(path, "bytes_read,bytes_read/s", "\n", cb);

        struct neper_coef *coef = neper_stat_print(ts, csv, NULL);
        if (!coef) {
                LOG_ERROR(ts->cb, "%s: not able to find coef", __func__);
                return -1;
        }

        const struct rate_conversion *units = opts->throughput_opt;
        if (units) {
                double thru = coef->thruput(coef);
                /* This is only run by the control thread */
                struct options *w_opts = (struct options *)opts;
                w_opts->local_rate = 8*thru; /* bits/s */
                if (!units->unit)
                        units = auto_unit(thru, units, cb);
                thru /= units->bytes_per_second;
                PRINT(cb, "throughput", "%.2f", thru);
                PRINT(cb, "throughput_units", "%s", units->unit);
        }

        if (csv)
                fclose(csv);
        coef->fini(coef);

        return 0;
}

static struct neper_stat *neper_stream_init(struct flow *f)
{
        return neper_stat_init(f, NULL, 0);
}

void stream_flow_init(struct thread *t, int fd)
{
        const struct flow_create_args args = {
                .thread  = t,
                .fd      = fd,
                .events  = stream_events(t),
                .opaque  = NULL,
                .handler = stream_handler,
                .stat    = neper_stream_init,
                .mbuf_alloc = stream_alloc
        };

        flow_create(&args);
}
