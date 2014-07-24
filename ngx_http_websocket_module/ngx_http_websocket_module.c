
/*
 * Copyright (C) FattyHong
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_channel.h>
#include <ngx_http.h>
#if (NGX_HAVE_SHA1)
#include <ngx_sha1.h>
#endif


typedef struct ngx_http_websocket_subscriber_s ngx_http_websocket_subscriber_t;


typedef struct {
    size_t                              shm_size;
} ngx_http_websocket_main_conf_t;


typedef struct {
    ngx_pid_t                           pid;
    ngx_queue_t                         worker_msgs;
} ngx_http_websocket_worker_data_t;


typedef struct {
    ngx_rbtree_t                        tree;
    ngx_rbtree_t                        hash;
    ngx_http_websocket_worker_data_t    ipc[NGX_MAX_PROCESSES];
} ngx_http_websocket_shm_data_t;


typedef struct {
    ngx_rbtree_node_t                   node;
    ngx_str_t                           id;
	ngx_queue_t							pid_groups;
} ngx_http_websocket_group_t;


typedef struct {
    ngx_queue_t                         queue;
    ngx_queue_t                         subscribers;
    ngx_int_t                           slot;
} ngx_http_websocket_pid_group_t;


struct ngx_http_websocket_subscriber_s {
    ngx_queue_t                         queue;
    ngx_uint_t                          id;
    ngx_int_t                           slot;
    ngx_http_request_t                 *request;
	ngx_http_websocket_group_t		   *group;
};


typedef struct {
    ngx_http_websocket_subscriber_t    *subscriber;
} ngx_http_websocket_ctx_t;


typedef struct {
    ngx_str_t                           raw;
    ngx_uint_t                          nrefs;
} ngx_http_websocket_msg_t;


typedef struct {
    ngx_queue_t                         queue;
    ngx_http_websocket_msg_t           *msg;
    ngx_queue_t                        *subscribers;
} ngx_http_websocket_worker_msg_t;


typedef struct {
    unsigned char                       fin:1;
    unsigned char                       rsv1:1;
    unsigned char                       rsv2:1;
    unsigned char                       rsv3:1;
    unsigned char                       opcode:4;
    unsigned char                       mask:1;
    unsigned char                       mask_key[4];
    uint64_t                            payload_len;
    u_char                             *payload;
} ngx_http_websocket_frame_t;


static ngx_int_t ngx_http_websocket_module_init(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_websocket_process_init(ngx_cycle_t *cycle);
static void ngx_http_websocket_channel_handler(ngx_event_t *ev);
static void ngx_http_websocket_process_worker_msg();
static ngx_int_t ngx_http_websocket_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_websocket_broadcast(ngx_slab_pool_t *shpool,
    ngx_http_websocket_shm_data_t *d, ngx_http_websocket_group_t *group,
    u_char *text, size_t len, ngx_log_t *log);
static ngx_int_t ngx_http_websocket_broadcast_msg(ngx_slab_pool_t *shpool,
    ngx_http_websocket_shm_data_t *d, ngx_http_websocket_group_t *group,
    ngx_http_websocket_msg_t *msg, ngx_log_t *log);
static ngx_http_websocket_msg_t *ngx_http_websocket_create_msg(
	ngx_slab_pool_t *shpool, u_char *text, size_t len);
static ngx_http_websocket_group_t *ngx_http_websocket_find_group(
	ngx_rbtree_t *tree, ngx_str_t *id, uint32_t hash);
static ngx_http_websocket_group_t *ngx_http_websocket_create_group_locked(
    ngx_slab_pool_t *shpool, ngx_http_websocket_shm_data_t *d, 
	ngx_str_t *id, uint32_t hash);
static ngx_http_websocket_subscriber_t *ngx_http_websocket_subscriber_prepare(
    ngx_slab_pool_t *shpool, ngx_http_websocket_shm_data_t *d,
    ngx_http_websocket_group_t *group, ngx_http_request_t *r);
static ngx_int_t ngx_http_websocket_subscriber_assign_group_locked(
    ngx_slab_pool_t *shpool, ngx_http_websocket_subscriber_t *subscriber,
    ngx_http_websocket_group_t *group);
static void ngx_http_websocket_cleanup_request(void *data);
static ngx_int_t ngx_http_websocket_send_header(ngx_http_request_t *r);
static ngx_str_t * ngx_http_websocket_get_header(ngx_http_request_t *r, 
	u_char *name);
static ngx_table_elt_t * ngx_http_websocket_add_header(ngx_http_request_t *r,
    u_char *name, u_char *value);
static void ngx_http_websocket_reading(ngx_http_request_t *r);
static u_char *ngx_http_websocket_generate_accept_value(
	ngx_http_request_t *r, ngx_str_t *sec_key);
static ngx_int_t ngx_http_websocket_recv(ngx_http_request_t *r, 
	u_char *buf, size_t len);
static ngx_int_t ngx_http_websocket_send(ngx_http_request_t *r, 
	u_char *text, size_t len);
static uint64_t ngx_http_websocket_htonll(uint64_t value);
static uint64_t ngx_http_websocket_ntohll(uint64_t value);
static ngx_int_t ngx_http_websocket_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static ngx_int_t ngx_http_websocket_postconfig(ngx_conf_t *cf);
static void *ngx_http_websocket_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_websocket_init_main_conf(ngx_conf_t *cf, void *parent);
static char *ngx_http_websocket(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_command_t  ngx_http_websocket_commands[] = {

	{ ngx_string("websocket"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_websocket,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("websocket_shm_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_websocket_main_conf_t, shm_size),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_websocket_module_ctx = {
    NULL,                                       /* preconfiguration */
    ngx_http_websocket_postconfig,              /* postconfiguration */

    ngx_http_websocket_create_main_conf,        /* create main configuration */
    ngx_http_websocket_init_main_conf,          /* init main configuration */

    NULL,                                       /* create server configuration */
    NULL,                                       /* merge server configuration */

    NULL,                                       /* create location configuration */
    NULL                                        /* merge location configuration */
};


ngx_module_t  ngx_http_websocket_module = {
    NGX_MODULE_V1,
    &ngx_http_websocket_module_ctx,             /* module context */
    ngx_http_websocket_commands,                /* module directives */
    NGX_HTTP_MODULE,                            /* module type */
    NULL,                                       /* init master */
    ngx_http_websocket_module_init,             /* init module */
    ngx_http_websocket_process_init,            /* init process */
    NULL,                                       /* init thread */
    NULL,                                       /* exit thread */
    NULL,                                       /* exit process */
    NULL,                                       /* exit master */
    NGX_MODULE_V1_PADDING
};


#define NGX_HTTP_WEBSOCKET_UPGRADE          "Upgrade"
#define NGX_HTTP_WEBSOCKET_CONNECTION       "Connection"
#define NGX_HTTP_WEBSOCKET_SEC_VERSION      "Sec-WebSocket-Version"
#define NGX_HTTP_WEBSOCKET_SEC_KEY          "Sec-WebSocket-Key"
#define NGX_HTTP_WEBSOCKET_WEBSOCKET        "WebSocket"
#define NGX_HTTP_WEBSOCKET_SIGN_KEY         "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define NGX_HTTP_WEBSOCKET_ACCEPT           "Sec-WebSocket-Accept"

#define NGX_HTTP_WEBSOCKET_FRAME_HEADER_MAX_LENGTH      144
#define NGX_HTTP_WEBSOCKET_LAST_FRAME                   0x8
#define NGX_HTTP_WEBSOCKET_TEXT_OPCODE                  0x1
#define NGX_HTTP_WEBSOCKET_CLOSE_OPCODE                 0x8
#define NGX_HTTP_WEBSOCKET_PING_OPCODE                  0x9


static ngx_slab_pool_t             *ngx_http_websocket_shpool;
static ngx_socket_t                 ngx_http_websocket_channels[NGX_MAX_PROCESSES][2];
static ngx_channel_t                ngx_http_websocket_cmd_message = {31, 0, 0, -1};


static ngx_int_t
ngx_http_websocket_module_init(ngx_cycle_t *cycle)
{
    ngx_int_t               i, s, last;
    u_long                  on;
    ngx_socket_t           *channel;
    ngx_core_conf_t        *ccf;

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    s = 0;
    last = ngx_last_process;

    for(i = 0; i < ccf->worker_processes; i++) {

        while (s < last && ngx_processes[s].pid != -1) {
            s++;
        }

        channel = ngx_http_websocket_channels[s];

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "socketpair() failed on socketpair with websocket module");
            return NGX_ERROR;
        }

        if (ngx_nonblocking(channel[0]) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          ngx_nonblocking_n " failed on socketpair with websocket module");
            ngx_close_channel(channel, cycle->log);
            return NGX_ERROR;
        }

        if (ngx_nonblocking(channel[1]) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          ngx_nonblocking_n " failed on socketpair with websocket module");
            ngx_close_channel(channel, cycle->log);
            return NGX_ERROR;
        }

        on = 1;

        if (ioctl(channel[0], FIOASYNC, &on) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "ioctl(FIOASYNC) failed on socketpair with websocket module");
            ngx_close_channel(channel, cycle->log);
            return NGX_ERROR;
        }

        if (fcntl(channel[0], F_SETOWN, ngx_pid) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "fcntl(F_SETOWN) failed on socketpair with websocket module");
            ngx_close_channel(channel, cycle->log);
            return NGX_ERROR;
        }

        if (fcntl(channel[0], F_SETFD, FD_CLOEXEC) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "fcntl(FD_CLOEXEC) failed on socketpair with websocket module");
            ngx_close_channel(channel, cycle->log);
            return NGX_ERROR;
        }

        if (fcntl(channel[1], F_SETFD, FD_CLOEXEC) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "fcntl(FD_CLOEXEC) failed with websocket module");
            ngx_close_channel(channel, cycle->log);
            return NGX_ERROR;
        }

        s++;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_websocket_process_init(ngx_cycle_t *cycle)
{
    ngx_slab_pool_t                        *shpool;
    ngx_http_websocket_shm_data_t          *d;

    if ((ngx_process != NGX_PROCESS_SINGLE) &&
        (ngx_process != NGX_PROCESS_WORKER))
    {
        return NGX_OK;
    }

    shpool = ngx_http_websocket_shpool;
    d = (ngx_http_websocket_shm_data_t *) shpool->data;

    ngx_shmtx_lock(&shpool->mutex);

    d->ipc[ngx_process_slot].pid = ngx_pid;
    ngx_queue_init(&d->ipc[ngx_process_slot].worker_msgs);

    ngx_shmtx_unlock(&shpool->mutex);

    if (ngx_add_channel_event(cycle, ngx_http_websocket_channels[ngx_process_slot][1],
                              NGX_READ_EVENT, ngx_http_websocket_channel_handler)
        == NGX_ERROR)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "failed to register channel handler with websocket module worker");
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_websocket_channel_handler(ngx_event_t *ev)
{
    ngx_int_t           n;
    ngx_channel_t       ch;
    ngx_connection_t   *c;

    if (ev->timedout) {
        ev->timedout = 0;
        return;
    }
    c = ev->data;

    while (1) {
        n = ngx_read_channel(c->fd, &ch, sizeof(ch), ev->log);

        if (n == NGX_ERROR) {

            if (ngx_event_flags & NGX_USE_EPOLL_EVENT) {
                ngx_del_conn(c, 0);
            }

            ngx_close_connection(c);
            return;
        }

        if ((ngx_event_flags & NGX_USE_EVENTPORT_EVENT) &&
            (ngx_add_event(ev, NGX_READ_EVENT, 0) == NGX_ERROR)) {
            return;
        }

        if (n == NGX_AGAIN) {
            return;
        }

        if (ch.command == ngx_http_websocket_cmd_message.command) {
            ngx_http_websocket_process_worker_msg();
        }
    }
}


static void
ngx_http_websocket_process_worker_msg()
{
    ngx_int_t                           	rc;
    ngx_slab_pool_t                    	   *shpool;
    ngx_http_websocket_shm_data_t      	   *d;
    ngx_http_websocket_worker_data_t   	   *wd;
    ngx_http_websocket_msg_t           	   *msg;
    ngx_http_websocket_worker_msg_t    	   *worker_msg;
    ngx_http_websocket_subscriber_t    	   *subscriber;
    ngx_http_request_t                 	   *request;
    ngx_queue_t                        	   *qm, *qs;

    shpool = ngx_http_websocket_shpool;
    d = shpool->data;
    wd = &d->ipc[ngx_process_slot];

    for (qm = ngx_queue_head(&wd->worker_msgs);
         qm != ngx_queue_sentinel(&wd->worker_msgs);
         qm = ngx_queue_next(qm))
    {
        worker_msg = ngx_queue_data(qm, ngx_http_websocket_worker_msg_t, queue);
        msg = worker_msg->msg;

        for (qs = ngx_queue_head(worker_msg->subscribers);
             qs != ngx_queue_sentinel(worker_msg->subscribers);
             qs = ngx_queue_next(qs))
        {
            subscriber = ngx_queue_data(qs, ngx_http_websocket_subscriber_t, queue);
            request = subscriber->request;

            rc = ngx_http_websocket_send(request, msg->raw.data, msg->raw.len);
            if (rc != NGX_OK) {
                ngx_http_finalize_request(request, NGX_DONE);
            }
        }

        ngx_shmtx_lock(&shpool->mutex);

        ngx_queue_remove(qm);
        ngx_slab_free_locked(shpool, worker_msg);

        worker_msg->msg->nrefs--;
        if (worker_msg->msg->nrefs == 0) {
            ngx_slab_free_locked(shpool, worker_msg->msg);
        }

        ngx_shmtx_unlock(&shpool->mutex);
    }
}


static ngx_int_t
ngx_http_websocket_handler(ngx_http_request_t *r)
{
    ngx_int_t                           rc;
    uint32_t                            hash;
    ngx_slab_pool_t                    *shpool;
    ngx_http_websocket_shm_data_t      *d;
    ngx_http_websocket_ctx_t           *ctx;
    ngx_http_websocket_group_t         *group;
    ngx_http_websocket_subscriber_t    *subscriber;
    ngx_str_t                           group_id;
    ngx_str_t                          *ws_upgrade, *ws_connection;
    ngx_str_t                          *sec_version, *sec_key;
    u_char                             *sec_accept;
    ngx_int_t                           version;
    ngx_http_websocket_main_conf_t     *wmcf;

    if (!(r->method & NGX_HTTP_GET)) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    wmcf = ngx_http_get_module_main_conf(r, ngx_http_websocket_module);

    shpool = ngx_http_websocket_shpool;
    d = shpool->data;

    ws_upgrade = ngx_http_websocket_get_header(r, (u_char *) NGX_HTTP_WEBSOCKET_UPGRADE);
    ws_connection = ngx_http_websocket_get_header(r, (u_char *) NGX_HTTP_WEBSOCKET_CONNECTION);
    sec_version = ngx_http_websocket_get_header(r, (u_char *) NGX_HTTP_WEBSOCKET_SEC_VERSION);
    sec_key = ngx_http_websocket_get_header(r, (u_char *) NGX_HTTP_WEBSOCKET_SEC_KEY);

    if (ws_upgrade == NULL || ws_connection == NULL ||
        sec_version == NULL || sec_key == NULL)
    {
        return NGX_HTTP_BAD_REQUEST;
    }

    version = ngx_atoi(sec_version->data, sec_version->len);
    if ((version != 8) && (version != 13)) {
        return NGX_HTTP_BAD_REQUEST;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_websocket_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_websocket_module);

    rc = ngx_http_arg(r, (u_char *) "group_id", ngx_strlen("group_id"), &group_id);
    if (rc == NGX_DECLINED) {
        group_id.data = (u_char *) "default";
        group_id.len = sizeof("default") - 1;
    }

    hash = ngx_crc32_short(group_id.data, group_id.len);

    group = ngx_http_websocket_find_group(&d->tree, &group_id, hash);

    if (group == NULL) {
        ngx_shmtx_lock(&shpool->mutex);

        group = ngx_http_websocket_create_group_locked(shpool, d, &group_id, hash);
        if (group == NULL) {
            ngx_shmtx_unlock(&shpool->mutex);
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_shmtx_unlock(&shpool->mutex);
    }

    subscriber = ngx_http_websocket_subscriber_prepare(shpool, d, group, r);
    if (subscriber == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->subscriber = subscriber;

    sec_accept = ngx_http_websocket_generate_accept_value(r, sec_key);
    if (sec_accept == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    r->headers_out.status_line.data = (u_char *) "101 Switching Protocols";
    r->headers_out.status_line.len = sizeof("101 Switching Protocols") - 1;

    ngx_http_websocket_add_header(r, (u_char *) NGX_HTTP_WEBSOCKET_UPGRADE,
                                   (u_char *) NGX_HTTP_WEBSOCKET_WEBSOCKET);

    ngx_http_websocket_add_header(r, (u_char *) NGX_HTTP_WEBSOCKET_CONNECTION,
                                   (u_char *) NGX_HTTP_WEBSOCKET_UPGRADE);

    ngx_http_websocket_add_header(r, (u_char *) NGX_HTTP_WEBSOCKET_ACCEPT,
                                   sec_accept);

    r->header_only = 1;
    r->headers_out.status = NGX_HTTP_OK;

    rc = ngx_http_websocket_send_header(r);

    r->read_event_handler = ngx_http_websocket_reading;

    return rc;
}


static ngx_int_t
ngx_http_websocket_broadcast_msg(ngx_slab_pool_t *shpool, ngx_http_websocket_shm_data_t *d,
    ngx_http_websocket_group_t *group, ngx_http_websocket_msg_t *msg, ngx_log_t *log)
{
    ngx_int_t                               rc;
    ngx_http_websocket_worker_data_t       *wd;
    ngx_http_websocket_pid_group_t         *pid_group;
    ngx_http_websocket_worker_msg_t        *worker_msg;
    ngx_flag_t                              emptys[NGX_MAX_PROCESSES];
    ngx_int_t                               slot;
    ngx_queue_t                            *q;

    ngx_shmtx_lock(&shpool->mutex);

    for (q = ngx_queue_head(&group->pid_groups);
         q != ngx_queue_sentinel(&group->pid_groups);
         q = ngx_queue_next(q))
    {
        pid_group = ngx_queue_data(q, ngx_http_websocket_pid_group_t, queue);

        slot = pid_group->slot;

        wd = &d->ipc[slot];
        emptys[slot] = ngx_queue_empty(&wd->worker_msgs);

        worker_msg = ngx_slab_alloc_locked(shpool, sizeof(ngx_http_websocket_worker_msg_t));
        if (worker_msg == NULL) {
            ngx_shmtx_unlock(&shpool->mutex);
            return NGX_ERROR;
        }

        worker_msg->msg = msg;
        worker_msg->subscribers = &pid_group->subscribers;
        msg->nrefs++;

        ngx_queue_insert_tail(&wd->worker_msgs, &worker_msg->queue);
    }

    ngx_shmtx_unlock(&shpool->mutex);


    for (q = ngx_queue_head(&group->pid_groups);
         q != ngx_queue_sentinel(&group->pid_groups);
         q = ngx_queue_next(q))
    {
        pid_group = ngx_queue_data(q, ngx_http_websocket_pid_group_t, queue);

        slot = pid_group->slot;

        if (emptys[slot]) {
            rc = ngx_write_channel(ngx_http_websocket_channels[slot][0],
                                   &ngx_http_websocket_cmd_message,
                                   sizeof(ngx_channel_t), log);

            if (rc == NGX_ERROR) {
                return NGX_ERROR;
            }
        }
    }

    return NGX_OK;
}


static ngx_int_t ngx_http_websocket_broadcast(ngx_slab_pool_t *shpool,
    ngx_http_websocket_shm_data_t *d, ngx_http_websocket_group_t *group,
    u_char *text, size_t len, ngx_log_t *log)
{
    ngx_int_t                       	rc;
    ngx_http_websocket_msg_t           *msg;

    msg = ngx_http_websocket_create_msg(shpool, text, len);
    if (msg == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_http_websocket_broadcast_msg(shpool, d, group, msg, log);
    return rc;
}


static ngx_http_websocket_msg_t *
ngx_http_websocket_create_msg(ngx_slab_pool_t *shpool, u_char *text, size_t len)
{
    ngx_http_websocket_msg_t       *msg;
    u_char                          payload_len_16_byte;
    u_char                          payload_len_64_byte;
    uint16_t                        len_net16;
    uint64_t                        len_net64;
    size_t                          frame_len;
    u_char                          last_frame_byte;
    u_char                         *last;

    last_frame_byte = NGX_HTTP_WEBSOCKET_TEXT_OPCODE  | (NGX_HTTP_WEBSOCKET_LAST_FRAME << 4);

    frame_len = NGX_HTTP_WEBSOCKET_FRAME_HEADER_MAX_LENGTH + len;

    ngx_shmtx_lock(&shpool->mutex);

    msg = ngx_slab_alloc_locked(shpool, sizeof(ngx_http_websocket_msg_t));
    if (msg == NULL) {
        ngx_shmtx_unlock(&shpool->mutex);
        return NULL;
    }

    msg->raw.data = ngx_slab_alloc_locked(shpool, frame_len);
    if (msg->raw.data == NULL) {
        ngx_slab_free_locked(shpool, msg);
        ngx_shmtx_unlock(&shpool->mutex);
        return NULL;
    }

    payload_len_16_byte = 126;
    payload_len_64_byte = 127;

    last = ngx_copy(msg->raw.data, &last_frame_byte, 1);

    if (len <= 125) {
        last = ngx_copy(last, &len, 1);

    } else if (len < (1 << 16)) {
        last = ngx_copy(last, &payload_len_16_byte, sizeof(payload_len_16_byte));
        len_net16 = htons(len);
        last = ngx_copy(last, &len_net16, 2);

    } else {
        last = ngx_copy(last, &payload_len_64_byte, sizeof(payload_len_64_byte));
        len_net64 = ngx_http_websocket_htonll(len);
        last = ngx_copy(last, &len_net64, 8);
    }

    last = ngx_copy(last, text, len);

    msg->raw.len = last - msg->raw.data;

    msg->nrefs = 0;

    ngx_shmtx_unlock(&shpool->mutex);

    return msg;
}


static ngx_http_websocket_group_t *
ngx_http_websocket_find_group(ngx_rbtree_t *tree, ngx_str_t *id, uint32_t hash)
{
    ngx_int_t                           rc;
    ngx_rbtree_node_t                  *node, *sentinel;
    ngx_http_websocket_group_t         *group;

    node = tree->root;
    sentinel = tree->sentinel;

    while ((node != NULL) && (node != sentinel)) {
        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        group = (ngx_http_websocket_group_t *) node;

        rc = ngx_memn2cmp(id->data, group->id.data, id->len, group->id.len);
        if (rc == 0) {
            return group;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}


static ngx_http_websocket_group_t *
ngx_http_websocket_create_group_locked(ngx_slab_pool_t *shpool,
    ngx_http_websocket_shm_data_t *d, ngx_str_t *id, uint32_t hash)
{
    ngx_http_websocket_group_t          *group;

    group = ngx_slab_alloc_locked(shpool, sizeof(ngx_http_websocket_group_t));
    if (group == NULL) {
        return NULL;
    }

    group->id.data = ngx_slab_alloc_locked(shpool, id->len);
    if (group->id.data == NULL) {
        ngx_slab_free_locked(shpool, group);
        return NULL;
    }

    group->id.len = id->len;
    ngx_memcpy(group->id.data, id->data, id->len);

	ngx_queue_init(&group->pid_groups);

    group->node.key = hash;
    ngx_rbtree_insert(&d->tree, &group->node);

    return group;
}


static ngx_http_websocket_subscriber_t *
ngx_http_websocket_subscriber_prepare(ngx_slab_pool_t *shpool, ngx_http_websocket_shm_data_t *d,
    ngx_http_websocket_group_t *group, ngx_http_request_t *r)
{
    ngx_int_t                                   rc;
    ngx_http_websocket_subscriber_t            *subscriber;
    ngx_http_cleanup_t                         *cln;

    subscriber = ngx_palloc(r->pool, sizeof(ngx_http_websocket_subscriber_t));
    if (subscriber == NULL) {
        return NULL;
    }

    subscriber->slot = ngx_process_slot;
    subscriber->request = r;
    subscriber->group = group;

    ngx_shmtx_lock(&shpool->mutex);

    rc = ngx_http_websocket_subscriber_assign_group_locked(shpool, subscriber, group);
    if (rc == NGX_ERROR) {
        ngx_shmtx_unlock(&shpool->mutex);
        return NULL;
    }

    ngx_shmtx_unlock(&shpool->mutex);

    r->read_event_handler = ngx_http_test_reading;
    r->write_event_handler = ngx_http_request_empty_handler;

    r->main->count++;

    cln = ngx_http_cleanup_add(r, 0);
    if (cln == NULL) {
        return NULL;
    }

    cln->handler = ngx_http_websocket_cleanup_request;
    cln->data = subscriber;

    return subscriber;
}


static ngx_int_t
ngx_http_websocket_subscriber_assign_group_locked(ngx_slab_pool_t *shpool,
    ngx_http_websocket_subscriber_t *subscriber, ngx_http_websocket_group_t *group)
{
    ngx_flag_t                          found;
    ngx_http_websocket_pid_group_t     *pid_group;
    ngx_queue_t                        *q;

    found = 0;

    for (q = ngx_queue_head(&group->pid_groups);
         q != ngx_queue_sentinel(&group->pid_groups);
         q = ngx_queue_next(q))
    {
        pid_group = ngx_queue_data(q, ngx_http_websocket_pid_group_t, queue);

        if (pid_group->slot == subscriber->slot) {
            found = 1;
            break;
        }
    }

    if (found == 0) {
        pid_group = ngx_slab_alloc_locked(shpool, sizeof(ngx_http_websocket_pid_group_t));
        if (pid_group == NULL) {
            return NGX_ERROR;
        }

        ngx_queue_insert_tail(&group->pid_groups, &pid_group->queue);

        pid_group->slot = ngx_process_slot;

        ngx_queue_init(&pid_group->subscribers);
    }

    ngx_queue_insert_tail(&pid_group->subscribers, &subscriber->queue);

    return NGX_OK;
}


static void
ngx_http_websocket_cleanup_request(void *data)
{
    ngx_http_websocket_subscriber_t        *subscriber = data;

    ngx_slab_pool_t                        *shpool;
    ngx_http_websocket_shm_data_t          *d;
    ngx_http_websocket_group_t             *group;
    ngx_http_request_t                     *request;

    shpool = ngx_http_websocket_shpool;
    d = shpool->data;

    group = subscriber->group;
    request = subscriber->request;
}


static ngx_int_t
ngx_http_websocket_send_header(ngx_http_request_t *r)
{
    size_t                     len;
    ngx_str_t                 *status_line;
    ngx_buf_t                 *b;
    ngx_uint_t                 i;
    ngx_chain_t                out;
    ngx_list_part_t           *part;
    ngx_table_elt_t           *header;

    if (r->header_sent) {
        return NGX_OK;
    }

    r->header_sent = 1;

    if (r != r->main) {
        return NGX_OK;
    }

    if (r->http_version < NGX_HTTP_VERSION_10) {
        return NGX_OK;
    }

    if (r->method == NGX_HTTP_HEAD) {
        r->header_only = 1;
    }

    if (r->headers_out.last_modified_time != -1) {
        if (r->headers_out.status != NGX_HTTP_OK
            && r->headers_out.status != NGX_HTTP_PARTIAL_CONTENT
            && r->headers_out.status != NGX_HTTP_NOT_MODIFIED)
        {
            r->headers_out.last_modified_time = -1;
            r->headers_out.last_modified = NULL;
        }
    }

    len = sizeof("HTTP/1.x ") - 1 + sizeof(CRLF) - 1
          /* the end of the header */
          + sizeof(CRLF) - 1;

    /* status line */

    status_line = NULL;
    if (r->headers_out.status_line.len) {
        len += r->headers_out.status_line.len;
        status_line = &r->headers_out.status_line;
    }

    part = &r->headers_out.headers.part;
    header = part->elts;

    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        if (header[i].hash == 0) {
            continue;
        }

        len += header[i].key.len + sizeof(": ") - 1 + header[i].value.len + sizeof(CRLF) - 1;
    }

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_ERROR;
    }

    /* "HTTP/1.x " */
    b->last = ngx_cpymem(b->last, "HTTP/1.1 ", sizeof("HTTP/1.x ") - 1);

    /* status line */
    if (status_line) {
        b->last = ngx_copy(b->last, status_line->data, status_line->len);
    }
    *b->last++ = CR; *b->last++ = LF;

    part = &r->headers_out.headers.part;
    header = part->elts;

    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        if (header[i].hash == 0) {
            continue;
        }

        b->last = ngx_copy(b->last, header[i].key.data, header[i].key.len);
        *b->last++ = ':'; *b->last++ = ' ';

        b->last = ngx_copy(b->last, header[i].value.data, header[i].value.len);
        *b->last++ = CR; *b->last++ = LF;
    }

    /* the end of HTTP header */
    *b->last++ = CR; *b->last++ = LF;

    r->header_size = b->last - b->pos;

    if (r->header_only) {
        b->last_buf = 1;
    }

    out.buf = b;
    out.next = NULL;
    b->flush = 1;

    return ngx_http_write_filter(r, &out);
}


static ngx_str_t *
ngx_http_websocket_get_header(ngx_http_request_t *r, u_char *name)
{
    size_t                      len;
    ngx_table_elt_t            *h;
    ngx_list_part_t            *part;
    ngx_uint_t                  i;

    len = ngx_strlen(name);

    part = &r->headers_in.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            h = part->elts;
            i = 0;
        }

        if ((h[i].key.len == len) &&
            (ngx_strncasecmp(h[i].key.data, name, len) == 0))
        {
            return &h[i].value;
        }
    }

    return NULL;
}


static ngx_table_elt_t *
ngx_http_websocket_add_header(ngx_http_request_t *r, u_char *name, u_char *value)
{
    ngx_table_elt_t  *h;

    h = ngx_list_push(&r->headers_out.headers);

    if (h == NULL) {
        return NULL;
    }

    h->hash = 1;
    h->key.data = name;
    h->key.len = ngx_strlen(name);
    h->value.data = value;
    h->value.len = ngx_strlen(value);

    return NULL;
}


static void
ngx_http_websocket_reading(ngx_http_request_t *r)
{
    ngx_int_t                           rc;
    ngx_slab_pool_t                    *shpool;
    ngx_http_websocket_shm_data_t      *d;
    ngx_http_websocket_group_t         *group;
    ngx_http_websocket_subscriber_t    *subscriber;
    ngx_http_websocket_ctx_t           *ctx;
    u_char                              buf[8];
    ngx_event_t                        *rev;
    ngx_connection_t                   *c;
    ngx_http_websocket_frame_t          frame;
    u_char                              close_last_frame_byte[2];
    u_char                              ping_last_frame_byte[2];
    uint64_t                            i;
    ngx_pool_t                         *temp_pool;

    ctx = ngx_http_get_module_ctx(r, ngx_http_websocket_module);

    shpool = ngx_http_websocket_shpool;
    d = shpool->data;

    subscriber = ctx->subscriber;
    group = subscriber->group;

    c = r->connection;
    rev = c->read;

    temp_pool = NULL;

#if (NGX_HAVE_KQUEUE)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {

        if (!rev->pending_eof) {
            return;
        }

        rev->eof = 1;
        c->error = 1;
        err = rev->kq_errno;

        goto closed;
    }

#endif

    if (ngx_http_websocket_recv(r, buf, 2) == NGX_ERROR) {
        goto closed;
    }

    frame.fin  = (buf[0] >> 7) & 1;
    frame.rsv1 = (buf[0] >> 6) & 1;
    frame.rsv2 = (buf[0] >> 5) & 1;
    frame.rsv3 = (buf[0] >> 4) & 1;
    frame.opcode = buf[0] & 0xf;
    frame.mask = (buf[1] >> 7) & 1;
    frame.payload_len = buf[1] & 0x7f;

    if (frame.payload_len == 126) {

        if (ngx_http_websocket_recv(r, buf, 2) == NGX_ERROR) {
            goto closed;
        }

        uint16_t len;
        ngx_memcpy(&len, buf, 2);
        frame.payload_len = ntohs(len);

    } else if (frame.payload_len == 127) {

        if (ngx_http_websocket_recv(r, buf, 8) == NGX_ERROR) {
            goto closed;
        }

        uint64_t len;
        ngx_memcpy(&len, buf, 8);
        frame.payload_len = ngx_http_websocket_ntohll(len);
    }

    if (frame.mask && (ngx_http_websocket_recv(r, frame.mask_key, 4)
                       == NGX_ERROR))
    {
        goto closed;
    }

    if (frame.payload_len > 0) {

        temp_pool = ngx_create_pool(4096, r->connection->log);
        if (temp_pool == NULL) {
            ngx_http_finalize_request(r, NGX_OK);
            return;
        }

        frame.payload = ngx_pcalloc(temp_pool, frame.payload_len);
        if (frame.payload == NULL) {
            goto closed;
        }

        if (ngx_http_websocket_recv(r, frame.payload, (size_t) frame.payload_len)
            == NGX_ERROR)
        {
            goto closed;
        }

        if (frame.opcode == NGX_HTTP_WEBSOCKET_TEXT_OPCODE) {
            if (frame.mask) {
                for (i = 0; i < frame.payload_len; i++) {
                    frame.payload[i] = frame.payload[i] ^ frame.mask_key[i % 4];
                }
            }

            rc = ngx_http_websocket_broadcast(shpool, d, group, frame.payload,
                                              frame.payload_len, r->connection->log);
            if (rc == NGX_ERROR) {
                ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
                ngx_destroy_pool(temp_pool);
                return;
            }

            ping_last_frame_byte[0] = NGX_HTTP_WEBSOCKET_PING_OPCODE | (NGX_HTTP_WEBSOCKET_LAST_FRAME << 4);
            ping_last_frame_byte[1] = 0x00;
            ngx_http_websocket_send(r, ping_last_frame_byte, 2);
        }

        ngx_destroy_pool(temp_pool);
    }

    if (frame.opcode == NGX_HTTP_WEBSOCKET_CLOSE_OPCODE) {
        close_last_frame_byte[0] = NGX_HTTP_WEBSOCKET_CLOSE_OPCODE | (NGX_HTTP_WEBSOCKET_LAST_FRAME << 4);
        close_last_frame_byte[1] = 0x00;
        ngx_http_websocket_send(r, close_last_frame_byte, 2);
        ngx_http_finalize_request(r, NGX_DONE);
    }

    /* aio does not call this handler */

    if ((ngx_event_flags & NGX_USE_LEVEL_EVENT) && rev->active) {

        if (ngx_del_event(rev, NGX_READ_EVENT, 0) != NGX_OK) {
            ngx_http_finalize_request(r, NGX_OK);
        }
    }

    return;

closed:

    if (temp_pool != NULL) {
        ngx_destroy_pool(temp_pool);
    }

    ngx_http_finalize_request(r, NGX_OK);
}


static u_char *
ngx_http_websocket_generate_accept_value(ngx_http_request_t *r, ngx_str_t *sec_key)
{
    ngx_str_t    *accept_value;
    ngx_str_t     sha1_signed;
    ngx_sha1_t    sha1;

    sha1_signed.data = ngx_pcalloc(r->pool, SHA_DIGEST_LENGTH);
    if (sha1_signed.data == NULL) {
        return NULL;
    }

    sha1_signed.len = SHA_DIGEST_LENGTH;

    ngx_sha1_init(&sha1);
    ngx_sha1_update(&sha1, sec_key->data, sec_key->len);
    ngx_sha1_update(&sha1, NGX_HTTP_WEBSOCKET_SIGN_KEY, ngx_strlen(NGX_HTTP_WEBSOCKET_SIGN_KEY));
    ngx_sha1_final(sha1_signed.data, &sha1);

    accept_value = ngx_pcalloc(r->pool, sizeof(ngx_str_t));
    if (accept_value == NULL) {
        return NULL;
    }

    accept_value->len = ngx_base64_encoded_length(SHA_DIGEST_LENGTH) + 1;
    accept_value->data = ngx_pcalloc(r->pool, accept_value->len);
    if (accept_value->data == NULL) {
        return NULL;
    }
    accept_value->data[accept_value->len] = '\0';

    ngx_encode_base64(accept_value, &sha1_signed);

    return accept_value->data;
}


static ngx_int_t
ngx_http_websocket_recv(ngx_http_request_t *r, u_char *buf, size_t len)
{
    size_t                  n;
    ngx_connection_t       *c;
   
    c = r->connection;

    n = c->recv(c, buf, len);

    if (n == len) {
        return NGX_OK;
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_websocket_send(ngx_http_request_t *r, u_char *text, size_t len)
{
    size_t                  n;
    ngx_connection_t       *c;

    c = r->connection;

    n = c->send(c, text, len);

    if (n == len) {
        return NGX_OK;
    }

    return NGX_ERROR;
}


static uint64_t
ngx_http_websocket_htonll(uint64_t value)
{
    int 			num;
	uint32_t 		high_part, low_part;

	num = 42;

    if (*(char *)&num == 42) {
        high_part = htonl((uint32_t) (value >> 32));
        low_part = htonl((uint32_t) (value & 0xFFFFFFFFLL));

        return (((uint64_t)low_part) << 32) | high_part;

    } else {
        return value;
    }
}


static uint64_t
ngx_http_websocket_ntohll(uint64_t value)
{
    int 			num;
	uint32_t 		high_part, low_part;
		
	num = 42;

    if (*(char *)&num == 42) {
        high_part = ntohl((uint32_t) (value >> 32));
        low_part = ntohl((uint32_t) (value & 0xFFFFFFFFLL));

        return (((uint64_t)low_part) << 32) | high_part;

    } else {
        return value;
    }
}


static ngx_int_t
ngx_http_websocket_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_slab_pool_t                        *shpool;
    ngx_http_websocket_shm_data_t          *d;
    ngx_rbtree_node_t                      *sentinel, *key;

    if (data) {
        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    d = ngx_slab_alloc(shpool, sizeof(ngx_http_websocket_shm_data_t));
    if (d == NULL) {
        return NGX_ERROR;
    }

    shpool->data = d;

    sentinel = ngx_slab_alloc(shpool, sizeof(ngx_rbtree_node_t));
    if (sentinel == NULL) {
        return NGX_ERROR;
    }

    key = ngx_slab_alloc(shpool, sizeof(ngx_rbtree_node_t));
    if (key == NULL) {
        return NGX_ERROR;
    }

    ngx_rbtree_init(&d->tree, sentinel, ngx_rbtree_insert_value);
    ngx_rbtree_init(&d->hash, key, ngx_rbtree_insert_value);

    ngx_http_websocket_shpool = shpool;

    return NGX_OK;
}


static ngx_int_t
ngx_http_websocket_postconfig(ngx_conf_t *cf)
{
#ifndef NGX_HAVE_SHA1
    ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                      "sha1 support is needed with websocket module");

    return NGX_ERROR;
#endif

    ngx_http_websocket_main_conf_t         *conf;
    size_t                              	size, limit;
    ngx_str_t                           	name;
    ngx_shm_zone_t                     	   *shm_zone;

    conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_websocket_module);

    limit = 32 * ngx_pagesize;
    size = ngx_align(conf->shm_size, ngx_pagesize);

    if (size < limit) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                          "The websocket_shm_size value must be at least %udKiB", limit >> 10);

        return NGX_ERROR;
    }

    ngx_str_set(&name, "websocket_zone");

    shm_zone = ngx_shared_memory_add(cf, &name, size,
                                     &ngx_http_websocket_module);
    if (shm_zone == NULL) {
        return NGX_ERROR;
    }

    if (shm_zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "\"%V\" is already used", &name);
        return NGX_ERROR;
    }

    shm_zone->init = ngx_http_websocket_init_zone;
    shm_zone->data = (void *) 1;

    return NGX_OK;
}


static void *
ngx_http_websocket_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_websocket_main_conf_t    *mcf;

    mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_websocket_main_conf_t));
    if (mcf == NULL) {
        return NGX_CONF_ERROR;
    }

    mcf->shm_size = NGX_CONF_UNSET_SIZE;

    return mcf;
}


static char *
ngx_http_websocket_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_websocket_main_conf_t     *mcf = conf;

    ngx_conf_init_size_value(mcf->shm_size, 100 * ngx_pagesize);

    return NGX_CONF_OK;
}


static char *
ngx_http_websocket(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_websocket_handler;

    return NGX_CONF_OK;
}
