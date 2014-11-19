/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <unistd.h>

#include <sys/epoll.h>

#include <nc_core.h>
#include <nc_event.h>
#include <nc_conf.h>
#include <nc_server.h>
#include <nc_proxy.h>
#include <nc_process.h>

static uint32_t ctx_id; /* context generation */

static struct context *
core_ctx_create(struct instance *nci)
{
    rstatus_t status;
    struct context *ctx;

    ctx = nc_alloc(sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->id = ++ctx_id;
    ctx->cf = NULL;
    ctx->stats = NULL;
    array_null(&ctx->pool);
    ctx->ep = -1;
    ctx->nevent = EVENT_SIZE_HINT;
    ctx->max_timeout = nci->stats_interval;
    ctx->timeout = ctx->max_timeout;
    ctx->event = NULL;

    /* parse and create configuration */
    ctx->cf = conf_create(nci->conf_filename);
    if (ctx->cf == NULL) {
        nc_free(ctx);
        return NULL;
    }

    /* initialize server pool from configuration */
    status = server_pool_init(&ctx->pool, &ctx->cf->pool, ctx);
    if (status != NC_OK) {
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }


    //create socket pair
    //TODO check whether child can send message to master
    //TODO move these codes to function
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, ctx->channel) == -1){
        log_error("[master] sockpair create domain socket failed");
    }
    //set noblock
    status = nc_set_nonblocking(ctx->channel[0]);
    if (status < 0) {
        log_error("set nonblock on p %d ", ctx->channel[0]);
        return NC_ERROR;
    }
    status = nc_set_nonblocking(ctx->channel[1]);
    if (status < 0) {
        log_error("set nonblock on p %d ", ctx->channel[0]);
        return NC_ERROR;
    }

    /* create stats per server pool */
    ctx->stats = stats_create(ctx,nci->stats_port, nci->stats_addr, nci->stats_interval,
                              nci->hostname, &ctx->pool);
    if (ctx->stats == NULL) {
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }

    /* initialize event handling for client, proxy and server */
    status = event_init(ctx, EVENT_SIZE_HINT);
    if (status != NC_OK) {
        stats_destroy(ctx->stats);
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }

    //TODO not preconnect here, will process it in each process

    /* preconnect? servers in server pool */
    //status = server_pool_preconnect(ctx);
    //if (status != NC_OK) {
    //    server_pool_disconnect(ctx);
    //    event_deinit(ctx);
    //    stats_destroy(ctx->stats);
    //    server_pool_deinit(&ctx->pool);
    //    conf_destroy(ctx->cf);
    //    nc_free(ctx);
    //    return NULL;
    //}

    /* initialize proxy per server pool */
    status = proxy_init(ctx);
    if (status != NC_OK) {
        server_pool_disconnect(ctx);
        event_deinit(ctx);
        stats_destroy(ctx->stats);
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }




     


    pid_t pid; 
    int i = 0;
    //TODO need change 8 to MARCRO
    for(i =0; i< 8; ++i){
       pid = fork();
       switch (pid) {
       case -1:
           log_error("fork() failed: %s", strerror(errno));
           return NULL;

       case 0:
           //TODO do child process
           process_loop(ctx,i);
           exit(1);
           break;

       default:
           /* parent terminates */
           break;
       }

    } 

    log_debug(LOG_VVERB, "created ctx %p id %"PRIu32"", ctx, ctx->id);

    return ctx;
}

static void
core_ctx_destroy(struct context *ctx)
{
    log_debug(LOG_VVERB, "destroy ctx %p id %"PRIu32"", ctx, ctx->id);
    proxy_deinit(ctx);
    server_pool_disconnect(ctx);
    event_deinit(ctx);
    stats_destroy(ctx->stats);
    server_pool_deinit(&ctx->pool);
    conf_destroy(ctx->cf);
    nc_free(ctx);
}

struct context *
core_start(struct instance *nci)
{
    struct context *ctx;

    mbuf_init(nci);
    msg_init();
    conn_init();

    ctx = core_ctx_create(nci);
    if (ctx != NULL) {
        nci->ctx = ctx;
        return ctx;
    }

    conn_deinit();
    msg_deinit();
    mbuf_deinit();

    return NULL;
}

void
core_stop(struct context *ctx)
{
    conn_deinit();
    msg_deinit();
    mbuf_deinit();
    core_ctx_destroy(ctx);
}

static rstatus_t
core_recv(struct context *ctx, struct conn *conn)
{
    rstatus_t status;

    status = conn->recv(ctx, conn);
    if (status != NC_OK) {
        log_debug(LOG_INFO, "recv on %c %d failed: %s",
                  conn->client ? 'c' : (conn->proxy ? 'p' : 's'), conn->sd,
                  strerror(errno));
    }

    return status;
}

static rstatus_t
core_send(struct context *ctx, struct conn *conn)
{
    rstatus_t status;

    status = conn->send(ctx, conn);
    if (status != NC_OK) {
        log_debug(LOG_INFO, "send on %c %d failed: %s",
                  conn->client ? 'c' : (conn->proxy ? 'p' : 's'), conn->sd,
                  strerror(errno));
    }

    return status;
}

static void
core_close(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    char type, *addrstr;

    ASSERT(conn->sd > 0);

    if (conn->client) {
        type = 'c';
        addrstr = nc_unresolve_peer_desc(conn->sd);
    } else {
        type = conn->proxy ? 'p' : 's';
        addrstr = nc_unresolve_addr(conn->addr, conn->addrlen);
    }
    log_debug(LOG_NOTICE, "close %c %d '%s' on event %04"PRIX32" eof %d done "
              "%d rb %zu sb %zu%c %s", type, conn->sd, addrstr, conn->events,
              conn->eof, conn->done, conn->recv_bytes, conn->send_bytes,
              conn->err ? ':' : ' ', conn->err ? strerror(conn->err) : "");

    status = event_del_conn(nc_processes[nc_current_process_slot].ep, conn);
    if (status < 0) {
        log_warn("event del conn e %d %c %d failed, ignored: %s", nc_processes[nc_current_process_slot].ep,
                 type, conn->sd, strerror(errno));
    }

    conn->close(ctx, conn);
}

static void
core_error(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    char type = conn->client ? 'c' : (conn->proxy ? 'p' : 's');

    status = nc_get_soerror(conn->sd);
    if (status < 0) {
        log_warn("get soerr on %c %d failed, ignored: %s", type, conn->sd,
                  strerror(errno));
    }
    conn->err = errno;

    core_close(ctx, conn);
}

static void
core_timeout(struct context *ctx)
{
    for (;;) {
        struct msg *msg;
        struct conn *conn;
        int64_t now, then;

        msg = msg_tmo_min();
        if (msg == NULL) {
            ctx->timeout = ctx->max_timeout;
            return;
        }

        /* skip over req that are in-error or done */

        if (msg->error || msg->done) {
            msg_tmo_delete(msg);
            continue;
        }

        /*
         * timeout expired req and all the outstanding req on the timing
         * out server
         */

        conn = msg->tmo_rbe.data;
        then = msg->tmo_rbe.key;

        now = nc_msec_now();
        if (now < then) {
            int delta = (int)(then - now);
            ctx->timeout = MIN(delta, ctx->max_timeout);
            return;
        }

        log_debug(LOG_INFO, "req %"PRIu64" on s %d timedout", msg->id, conn->sd);

        msg_tmo_delete(msg);
        conn->err = ETIMEDOUT;

        core_close(ctx, conn);
    }
}

static void
core_core(struct context *ctx, struct conn *conn, uint32_t events)
{
    rstatus_t status;

    log_debug(LOG_VVERB, "event %04"PRIX32" on %c %d", events,
              conn->client ? 'c' : (conn->proxy ? 'p' : 's'), conn->sd);

    conn->events = events;

    /* error takes precedence over read | write */
    if (events & EPOLLERR) {
        core_error(ctx, conn);
        return;
    }

    /* read takes precedence over write */
    if (events & (EPOLLIN | EPOLLHUP)) {
        status = core_recv(ctx, conn);
        if (status != NC_OK || conn->done || conn->err) {
            core_close(ctx, conn);
            return;
        }
    }

    if (events & EPOLLOUT) {
        status = core_send(ctx, conn);
        if (status != NC_OK || conn->done || conn->err) {
            core_close(ctx, conn);
            return;
        }
    }
}

#define MSG_DATA "xyz"
#define MSG_LEN sizeof(MSG_DATA)

static void send_message(int fd) {
    int ret;
    int i;
 
    struct msghdr msghdr;
    struct iovec iov[1];
    union {
        struct cmsghdr cm;
        char data[CMSG_SPACE(sizeof(int))];
    } cmsg;
 
 
    cmsg.cm.cmsg_len = CMSG_LEN(sizeof(int));
    cmsg.cm.cmsg_level = SOL_SOCKET;
    cmsg.cm.cmsg_type = SCM_RIGHTS;
    *(int*)CMSG_DATA(&(cmsg.cm)) = NULL;

    char buf[128];
    sprintf(buf,"come from pid=%d",getpid());
    iov[0].iov_base = buf;
    iov[0].iov_len = strlen(buf);
    //iov[1].iov_base = MSG_DATA;
    //iov[1].iov_len = MSG_LEN;
    //iov[2].iov_base = MSG_DATA;
    //iov[2].iov_len = MSG_LEN;
    //iov[3].iov_base = MSG_DATA;
    //iov[3].iov_len = MSG_LEN;
 
    msghdr.msg_name = NULL;
    msghdr.msg_namelen = 0;
    msghdr.msg_iov = iov;
    msghdr.msg_iovlen = 1;
    msghdr.msg_control = (caddr_t)&cmsg;
    msghdr.msg_controllen = sizeof(cmsg);
 
    log_error( "to send %d pid = %d", MSG_LEN,getpid() );
 
    for( i=0; i<2; i++ )
    {
        ret = sendmsg( fd, &msghdr, MSG_DONTWAIT );
        if( ret < 0 )
        {
            log_error( "sendmsg failed" );
            continue;
        }
 
    }
 
    return 0;
}

rstatus_t
core_loop(struct context *ctx)
{
    int i, nsd;

    nsd = event_wait(nc_processes[nc_current_process_slot].ep, ctx->event, ctx->nevent, ctx->timeout);
    if (nsd < 0) {
        return nsd;
    }

    for (i = 0; i < nsd; i++) {
        struct epoll_event *ev = &ctx->event[i];

        core_core(ctx, ev->data.ptr, ev->events);
    }

    core_timeout(ctx);

    stats_swap(ctx->stats);

   
    //TODO just send msg here for test
    send_message(ctx->channel[1]);
 
    return NC_OK;
}
