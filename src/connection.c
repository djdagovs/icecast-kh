/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2000-2004, Jack Moffitt <jack@xiph.org>,
 *                      Michael Smith <msmith@xiph.org>,
 *                      oddsock <oddsock@xiph.org>,
 *                      Karl Heyes <karl@xiph.org>
 *                      and others (see AUTHORS for details).
 * Copyright 2000-2014, Karl Heyes <karl@kheyes.plus.com>
 */

/* -*- c-basic-offset: 4; indent-tabs-mode: nil; -*- */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#ifdef HAVE_POLL
#include <sys/poll.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _MSC_VER
 #include <winsock2.h>
 #include <ws2tcpip.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#ifdef HAVE_SIGNALFD
#include <sys/signalfd.h>
#include <signal.h>
#endif

#include "compat.h"

#include "thread/thread.h"
#include "avl/avl.h"
#include "net/sock.h"
#include "httpp/httpp.h"
#include "timing/timing.h"

#include "cfgfile.h"
#include "global.h"
#include "util.h"
#include "connection.h"
#include "refbuf.h"
#include "client.h"
#include "stats.h"
#include "logging.h"
#include "xslt.h"
#include "fserve.h"
#include "sighandler.h"
#include "slave.h"

#include "yp.h"
#include "source.h"
#include "format.h"
#include "format_mp3.h"
#include "event.h"
#include "admin.h"
#include "auth.h"

#define CATMODULE "connection"

/* Two different major types of source authentication.
   Shoutcast style is used only by the Shoutcast DSP
   and is a crazy version of HTTP.  It looks like :
     Source Client -> Connects to port + 1
     Source Client -> sends encoder password (plaintext)\r\n
     Icecast -> reads encoder password, if ok, sends OK2\r\n, else disconnects
     Source Client -> reads OK2\r\n, then sends http-type request headers
                      that contain the stream details (icy-name, etc..)
     Icecast -> reads headers, stores them
     Source Client -> starts sending MP3 data
     Source Client -> periodically updates metadata via admin.cgi call

   Icecast auth style uses HTTP and Basic Authorization.
*/

static int  shoutcast_source_client (client_t *client);
static int  http_client_request (client_t *client);
static int  _handle_get_request (client_t *client);
static int  _handle_source_request (client_t *client);
static int  _handle_stats_request (client_t *client);

struct banned_entry
{
    char ip[16]; // may want to expand later for ipv6
    union
    {
        time_t timeout;
        struct banned_entry *next;
    } a;
};


static spin_t _connection_lock;
static uint64_t _current_id = 0;
thread_type *conn_tid;
int sigfd;

static int ssl_ok;
#ifdef HAVE_OPENSSL
#ifndef SSL_OP_NO_COMPRESSION
#define SSL_OP_NO_COMPRESSION 0
#endif
static SSL_CTX *ssl_ctx;
static mutex_t *ssl_mutexes = NULL;
static unsigned long ssl_id_function (void);
static void ssl_locking_function (int mode, int n, const char *file, int line);
#endif

int header_timeout;

struct _client_functions shoutcast_source_ops =
{
    shoutcast_source_client,
    client_destroy
};

struct _client_functions http_request_ops =
{
    http_client_request,
    client_destroy
};

struct _client_functions http_req_get_ops =
{
    _handle_get_request,
    client_destroy
};
struct _client_functions http_req_source_ops =
{
    _handle_source_request,
    client_destroy
};

struct _client_functions http_req_stats_ops =
{
    _handle_stats_request,
    client_destroy
};

/* filtering client connection based on IP */
cache_file_contents banned_ip, allowed_ip;
struct banned_entry *ban_entry_removal;

/* filtering listener connection based on useragent */
cache_file_contents useragents;

int connection_running = 0;



static int compare_banned_ip (void *arg, void *a, void *b)
{
    struct banned_entry *this = (struct banned_entry *)a;
    struct banned_entry *that = (struct banned_entry *)b;
    int ret = strcmp (&this->ip[0], &that->ip[0]);

    if (ban_entry_removal == NULL && ret)
    {
        time_t now = *((time_t*)arg);
        if (that->a.timeout && that->a.timeout < now - 60)
        {
            ban_entry_removal = that; // identify possible removal
            DEBUG3 ("now %ld, timer %ld, ip %s", (long)now, (long)that->a.timeout, &that->ip[0]);
        }
    }
    return ret;
}


void connection_initialize(void)
{
    thread_spin_create (&_connection_lock);

    memset (&banned_ip, 0, sizeof (banned_ip));
    memset (&allowed_ip, 0, sizeof (allowed_ip));
    memset (&useragents, 0, sizeof (useragents));

    conn_tid = NULL;
    connection_running = 0;
#ifdef HAVE_OPENSSL
    SSL_load_error_strings();                /* readable error messages */
    SSL_library_init();                      /* initialize library */
    ssl_mutexes = malloc(CRYPTO_num_locks() * sizeof(mutex_t));
    if (ssl_mutexes)
    {
        int i;
        for (i=0; i < CRYPTO_num_locks();  i++)
            thread_mutex_create (&ssl_mutexes[i]);
        CRYPTO_set_id_callback (ssl_id_function);
        CRYPTO_set_locking_callback (ssl_locking_function);
    }
    else
        WARN0("unable to set up internal locking for SSL, memory problem");
#endif
}

void connection_shutdown(void)
{
    connection_listen_sockets_close (NULL, 1);
    thread_spin_destroy (&_connection_lock);
#ifdef HAVE_OPENSSL
    CRYPTO_set_id_callback(NULL);
    CRYPTO_set_locking_callback(NULL);
    if (ssl_mutexes)
    {
        int i;
        for(i = 0; i < CRYPTO_num_locks(); i++)
            thread_mutex_destroy (&ssl_mutexes[i]);
        free (ssl_mutexes);
        ssl_mutexes = NULL;
    }
#endif
}

static uint64_t _next_connection_id(void)
{
    uint64_t id;

    thread_spin_lock (&_connection_lock);
    id = _current_id++;
    thread_spin_unlock (&_connection_lock);

    return id;
}


#ifdef HAVE_OPENSSL
static unsigned long ssl_id_function (void)
{
    return (unsigned long)thread_self();
}

static void ssl_locking_function (int mode, int n, const char *file, int line)
{
    if (mode & CRYPTO_LOCK)
        thread_mutex_lock_c (&ssl_mutexes[n], line, file);
    else
        thread_mutex_unlock_c (&ssl_mutexes[n], line, file);
}

static void get_ssl_certificate (ice_config_t *config)
{
    ssl_ok = 0;
    do
    {
        long ssl_opts;

        ssl_ctx = NULL;
        if (config->cert_file == NULL)
            break;

        ssl_ctx = SSL_CTX_new (SSLv23_server_method());
        ssl_opts = SSL_CTX_get_options (ssl_ctx);
        SSL_CTX_set_options (ssl_ctx, ssl_opts|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3|SSL_OP_NO_COMPRESSION);

        if (SSL_CTX_use_certificate_chain_file (ssl_ctx, config->cert_file) <= 0)
        {
            WARN1 ("Invalid cert file %s", config->cert_file);
            break;
        }
        if (SSL_CTX_use_PrivateKey_file (ssl_ctx, config->cert_file, SSL_FILETYPE_PEM) <= 0)
        {
            WARN1 ("Invalid private key file %s", config->cert_file);
            break;
        }
        if (!SSL_CTX_check_private_key (ssl_ctx))
        {
            ERROR1 ("Invalid %s - Private key does not match cert public key", config->cert_file);
            break;
        }
        if (SSL_CTX_set_cipher_list(ssl_ctx, config->cipher_list) <= 0)
        {
            WARN1 ("Invalid cipher list: %s", config->cipher_list);
        }
        ssl_ok = 1;
        INFO1 ("SSL certificate found at %s", config->cert_file);
        INFO1 ("SSL using ciphers %s", config->cipher_list);
        return;
    } while (0);
    if (ssl_ctx)
    {
        WARN2 ("failed to load cert %s (%s)", config->cert_file, ERR_reason_error_string (ERR_peek_last_error()));
        SSL_CTX_free (ssl_ctx);
        ssl_ctx = NULL;
    }
    INFO0 ("No SSL capability on any configured ports");
}


/* handlers for reading and writing a connection_t when there is ssl
 * configured on the listening port
 */
int connection_read_ssl (connection_t *con, void *buf, size_t len)
{
    int bytes = SSL_read (con->ssl, buf, len);
    int code = SSL_get_error (con->ssl, bytes);
    char err[128];

    switch (code)
    {
        case SSL_ERROR_NONE:
        case SSL_ERROR_ZERO_RETURN:
            break;
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return -1;
        default:
            con->error = 1;
            ERR_error_string (ERR_get_error(), err);
            DEBUG2("error %d, %s", code, err);
            bytes = 0;
    }
    return bytes;
}

int connection_send_ssl (connection_t *con, const void *buf, size_t len)
{
    int bytes = SSL_write (con->ssl, buf, len);
    int code = SSL_get_error (con->ssl, bytes);
    char err[128];

    switch (code)
    {
        case SSL_ERROR_NONE:
        case SSL_ERROR_ZERO_RETURN:
            break;
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return -1;
        default:
            con->error = 1;
            ERR_error_string (ERR_get_error(), err);
            DEBUG2("error %d, %s", code, err);
            return -1;
    }
    con->sent_bytes += bytes;
    return bytes;
}
#else

/* SSL not compiled in, so at least log it */
static void get_ssl_certificate (ice_config_t *config)
{
    ssl_ok = 0;
    INFO0 ("No SSL capability");
}
#endif /* HAVE_OPENSSL */


/* handlers (default) for reading and writing a connection_t, no encrpytion
 * used just straight access to the socket
 */
int connection_read (connection_t *con, void *buf, size_t len)
{
    int bytes = sock_read_bytes (con->sock, buf, len);
    if (bytes == 0)
        con->error = 1;
    if (bytes == -1 && !sock_recoverable (sock_error()))
        con->error = 1;
    return bytes;
}

int connection_send (connection_t *con, const void *buf, size_t len)
{
    int bytes = sock_write_bytes (con->sock, buf, len);
    if (bytes < 0)
    {
        if (!sock_recoverable (sock_error()))
            con->error = 1;
    }
    else
        con->sent_bytes += bytes;
    return bytes;
}


void connection_bufs_init (struct connection_bufs *v, short start)
{
    memset (v, 0, sizeof (struct connection_bufs));
    if (start && start < 500)
    {
        v->block = calloc (start, sizeof (IOVEC));
        v->max = start;
    }
}


void connection_bufs_release (struct connection_bufs *v)
{
    free (v->block);
    memset (v, 0, sizeof (struct connection_bufs));
}


void connection_bufs_flush (struct connection_bufs *v)
{
    v->count = 0;
    v->total = 0;
}


int connection_bufs_append (struct connection_bufs *v, void *buf, unsigned int len)
{
    if (len > 0xFFFFFF)
    {
        ERROR1 ("Sanity check failed, len is %u", len);
        abort();
    }
    if (v->count >= v->max)
    {
       int len = v->max + 16;
       IOVEC *arr = realloc (v->block, (len*sizeof(IOVEC)));
       v->max = len;
       v->block = arr;
    }
    IO_VECTOR_BASE (v->block + v->count) = buf;
    IO_VECTOR_LEN (v->block + v->count) = len;
    v->count++;
    v->total += len;
    return v->total;

}


static int connbufs_locate_start (struct connection_bufs *vects, int skip, IOVEC *old_value, int *offp)
{
    int sum = 0, i = vects->count;
    IOVEC *p = vects->block;

    if (skip < vects->total)
    {
        for (; i; i--)
        {
            if (sum + IO_VECTOR_LEN(p) > skip)
            {
                int offset = skip - sum;
                if (offset)
                {
                    *old_value = *p;
                    IO_VECTOR_BASE(p) += offset;
                    IO_VECTOR_LEN(p) -= offset;
                }
                *offp = offset;
                return p - vects->block;
            }
            sum += IO_VECTOR_LEN(p);
            p++;
        }
    }
    return -1;
}


int connection_bufs_send (connection_t *con, struct connection_bufs *vectors, int skip)
{
    IOVEC *p = vectors->block, old_vals;
    int i = vectors->count,  offset = 0, ret = -1;

    if (skip > vectors->total) abort();
    i = connbufs_locate_start (vectors, skip, &old_vals, &offset);
    p = vectors->block + i;

    if (i >= 0)
    {
        if (not_ssl_connection (con))
        {
            ret = sock_writev (con->sock, p, vectors->count - i);
            if (ret < 0 && !sock_recoverable (sock_error()))
                con->error = 1;
        }
#ifdef HAVE_OPENSSL
        else
        {
            IOVEC *io = p;
            int bytes = 0;
            for (; i < vectors->count; i++, io++)
            {
               int v = connection_send_ssl (con, IO_VECTOR_BASE(io), IO_VECTOR_LEN(io));
               if (v > 0) bytes += v;
               if (v < 0 || v < IO_VECTOR_LEN(io)) break;
            }
            if (bytes > 0)  ret = bytes;
        }
#endif
        if (offset)
            *p = old_vals;
        if (ret > 0)
            con->sent_bytes += ret;
    }
    return ret;
}


int connection_chunk_start (connection_t *con, struct connection_bufs *bufs, char *chunk_hdr, unsigned chunk_sz)
{
    int chunk_hdrlen = snprintf (chunk_hdr, CHUNK_HDR_SZ, "%x\r\n", chunk_sz);

    return connection_bufs_append (bufs, chunk_hdr, chunk_hdrlen);
}


int connection_chunk_end (connection_t *con, struct connection_bufs *bufs, char *chunk_hdr, unsigned chunk_sz)
{
    char *p = strchr (chunk_hdr, '\r');
    if (p && p[1] == '\n')
        return connection_bufs_append (bufs, p, 2);
    ERROR0 ("chunk has no EOL");
    abort();
}



static void add_banned_ip (cache_file_contents *c, const void *e, time_t now)
{
    if (c)
    {
        struct banned_entry *banned;
        const char *ip = e;
#ifdef HAVE_FNMATCH_H
        if (ip [strcspn (ip, "*?[")]) // if wildcard present
        {
            struct cache_list_node *entry = calloc (1, sizeof (*entry));
            entry->content = strdup (ip);
            entry->next = c->wildcards;
            c->wildcards = entry;
            DEBUG1 ("Adding wildcard entry \"%.30s\"", ip);
            return;
        }
#endif
        banned = calloc (1, sizeof (struct banned_entry));
        snprintf (&banned->ip[0], sizeof (banned->ip), "%s", ip);
        banned->a.timeout = now;
        DEBUG1 ("Adding literal entry \"%.30s\"", ip);
        avl_insert (c->contents, banned);
    }
}

void connection_add_banned_ip (const char *ip, int duration)
{
    time_t timeout = 0;
    if (duration > 0)
        timeout = time(NULL) + duration;

    if (banned_ip.contents)
    {
        global_lock();
        add_banned_ip (&banned_ip, ip, timeout);
        global_unlock();
    }
}

void connection_release_banned_ip (const char *ip)
{
    if (banned_ip.contents)
    {
        global_lock();
        avl_delete (banned_ip.contents, (void*)ip, cached_treenode_free);
        global_unlock();
    }
}

void connection_stats (void)
{
    long banned_IPs = 0;
    if (banned_ip.contents)
        banned_IPs = (long)banned_ip.contents->length;
    stats_event_args (NULL, "banned_IPs", "%ld", banned_IPs);
}


time_t cachefile_timecheck = (time_t)0;

/* check specified ip against internal set of banned IPs
 * return -1 for no data, 0 for no match and 1 for match
 */
static int search_banned_ip_locked (char *ip)
{
    if (banned_ip.wildcards)
    {
        struct cache_list_node *entry = banned_ip.wildcards;
        while (entry)
        {
            if (cached_pattern_compare (ip, entry->content) == 0)
                return 1;
            entry = entry->next;
        }
    }
    if (banned_ip.contents)
    {
        void *result;

        ban_entry_removal = NULL;
        if (avl_get_by_key (banned_ip.contents, ip, &result) == 0)
        {
            struct banned_entry *match = result;
            if (match->a.timeout == 0 || match->a.timeout > cachefile_timecheck)
            {
                if (match->a.timeout && cachefile_timecheck + 300 > match->a.timeout)
                    match->a.timeout = cachefile_timecheck + 300;
                return 1;
            }
            avl_delete (banned_ip.contents, ip, cached_treenode_free);
        }
        /* we may of seen another one to remove */
        if (ban_entry_removal)
        {
            INFO1 ("removing %s from ban list for now", &ban_entry_removal->ip[0]);
            avl_delete (banned_ip.contents, &ban_entry_removal->ip[0], cached_treenode_free);
            ban_entry_removal = NULL;
        }
    }
    return 0;
}

static int search_banned_ip (char *ip)
{
    int ret;
    cached_file_recheck (&banned_ip, cachefile_timecheck);
    global_lock();
    ret = search_banned_ip_locked (ip);
    global_unlock();
    return ret;
}


/* return 0 if the passed ip address is not to be handled by icecast, non-zero otherwise */
static int accept_ip_address (char *ip)
{
    cachefile_timecheck = time (NULL);

    if (search_banned_ip (ip) > 0)
    {
        DEBUG1 ("%s banned", ip);
        return 0;
    }
    if (cached_pattern_search (&allowed_ip, ip, cachefile_timecheck) == 0)
    {
        DEBUG1 ("%s is not allowed", ip);
        return 0;
    }
    return 1;
}


int connection_init (connection_t *con, sock_t sock, const char *addr)
{
    if (con)
    {
        struct sockaddr_storage sa;
        socklen_t slen = sizeof (sa);

        con->sock = sock;
        if (sock == SOCK_ERROR)
            return -1;
        con->id = _next_connection_id();
        if (addr)
        {
            con->ip = strdup (addr + (strncmp (addr, "::ffff:", 7) == 0 ? 7: 0));
            return 0;
        }
        if (getpeername (sock, (struct sockaddr *)&sa, &slen) == 0)
        {
            char *ip;
#ifdef HAVE_GETNAMEINFO
            char buffer [200] = "unknown";
            getnameinfo ((struct sockaddr *)&sa, slen, buffer, 200, NULL, 0, NI_NUMERICHOST);
            if (strncmp (buffer, "::ffff:", 7) == 0)
                ip = strdup (buffer+7);
            else
                ip = strdup (buffer);
#else
            int len = 30;
            ip = malloc (len);
            strncpy (ip, inet_ntoa (((struct sockaddr_in*)&sa)->sin_addr), len);
#endif
            if (accept_ip_address (ip))
            {
                con->ip = ip;
                return 0;
            }
            free (ip);
        }
        memset (con, 0, sizeof (connection_t));
        con->sock = SOCK_ERROR;
    }
    return -1;
}


/* prepare connection for interacting over a SSL connection
 */
void connection_uses_ssl (connection_t *con)
{
#ifdef HAVE_OPENSSL
    con->ssl = SSL_new (ssl_ctx);
    SSL_set_accept_state (con->ssl);
    SSL_set_fd (con->ssl, con->sock);
    SSL_set_mode (con->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER|SSL_MODE_ENABLE_PARTIAL_WRITE);
#endif
}

#ifdef HAVE_SIGNALFD
void connection_close_sigfd (void)
{
    if (sigfd >= 0)
        close (sigfd);
    sigfd = -1;
}
#else
#define connection_close_sigfd()    do {}while(0);
#endif

static sock_t wait_for_serversock (void)
{
#ifdef HAVE_POLL
    int i, ret;
    struct pollfd ufds [global.server_sockets + 1];

    for(i=0; i < global.server_sockets; i++) {
        ufds[i].fd = global.serversock[i];
        ufds[i].events = POLLIN;
        ufds[i].revents = 0;
    }
#ifdef HAVE_SIGNALFD
    ufds[i].revents = 0;
    if (sigfd >= 0)
    {
        ufds[i].fd = sigfd;
        ufds[i].events = POLLIN;
        ret = poll(ufds, i+1, 4000);
    }
    else
        ret = poll(ufds, i, 4000);
#else
    ret = poll(ufds, global.server_sockets, 333);
#endif

    if (ret <= 0)
        return SOCK_ERROR;
    else {
        int dst;
#ifdef HAVE_SIGNALFD
        if (ufds[i].revents & POLLIN)
        {
            struct signalfd_siginfo fdsi;
            int ret  = read (sigfd, &fdsi, sizeof(struct signalfd_siginfo));
            if (ret == sizeof(struct signalfd_siginfo))
            {
                switch (fdsi.ssi_signo)
                {
                    case SIGINT:
                    case SIGTERM:
                        DEBUG0 ("signalfd received a termination");
                        global.running = ICE_HALTING;
                        connection_running = 0;
                        break;
                    case SIGHUP:
                        INFO0 ("HUP received, reread scheduled");
                        global.schedule_config_reread = 1;
                        break;
                    default:
                        WARN1 ("unexpected signal (%d)", fdsi.ssi_signo);
                }
            }
        }
        if (ufds[i].revents & (POLLNVAL|POLLERR))
        {
            ERROR0 ("signalfd descriptor became invalid, doing thread restart");
            slave_restart(); // something odd happened
        }
#endif
        for(i=0; i < global.server_sockets; i++) {
            if(ufds[i].revents & POLLIN)
                return ufds[i].fd;
            if(ufds[i].revents & (POLLHUP|POLLERR|POLLNVAL))
            {
                if (ufds[i].revents & (POLLHUP|POLLERR))
                {
                    sock_close (global.serversock[i]);
                    WARN0("Had to close a listening socket");
                }
                global.serversock[i] = SOCK_ERROR;
            }
        }
        /* remove any closed sockets */
        for(i=0, dst=0; i < global.server_sockets; i++)
        {
            if (global.serversock[i] == SOCK_ERROR)
                continue;
            if (i!=dst)
                global.serversock[dst] = global.serversock[i];
            dst++;
        }
        global.server_sockets = dst;
        return SOCK_ERROR;
    }
#else
    fd_set rfds;
    struct timeval tv;
    int i, ret;
    sock_t max = SOCK_ERROR;

    FD_ZERO(&rfds);

    for(i=0; i < global.server_sockets; i++) {
        FD_SET(global.serversock[i], &rfds);
        if (max == SOCK_ERROR || global.serversock[i] > max)
            max = global.serversock[i];
    }

    tv.tv_sec = 0;
    tv.tv_usec = 333000;

    ret = select(max+1, &rfds, NULL, NULL, &tv);
    if(ret < 0) {
        return SOCK_ERROR;
    }
    else if(ret == 0) {
        return SOCK_ERROR;
    }
    else {
        for(i=0; i < global.server_sockets; i++) {
            if(FD_ISSET(global.serversock[i], &rfds))
                return global.serversock[i];
        }
        return SOCK_ERROR; /* Should be impossible, stop compiler warnings */
    }
#endif
}


static client_t *accept_client (void)
{
    client_t *client = NULL;
    sock_t sock, serversock = wait_for_serversock ();
    char addr [200];

    if (serversock == SOCK_ERROR)
        return NULL;

    sock = sock_accept (serversock, addr, 200);
    if (sock == SOCK_ERROR)
    {
        if (sock_recoverable (sock_error()))
            return NULL;
        WARN2 ("accept() failed with error %d: %s", sock_error(), strerror(sock_error()));
        thread_sleep (500000);
        return NULL;
    }
    do
    {
        int i;
        refbuf_t *r;

        if (accept_ip_address (addr) == 0)
            break;
        if (sock_set_blocking (sock, 0) || (sock_set_cork (sock, 1) < 0 && sock_set_nodelay (sock)))
        {
            WARN0 ("failed to set tcp options on client connection, dropping");
            break;
        }
        client = calloc (1, sizeof (client_t));
        if (client == NULL || connection_init (&client->connection, sock, addr) < 0)
            break;

        client->shared_data = r = refbuf_new (PER_CLIENT_REFBUF_SIZE);
        r->len = 0; // for building up the request coming in

        global_lock ();
        client_register (client);

        for (i=0; i < global.server_sockets; i++)
        {
            if (global.serversock[i] == serversock)
            {
                client->server_conn = global.server_conn[i];
                client->server_conn->refcount++;
                if (client->server_conn->ssl && ssl_ok)
                    connection_uses_ssl (&client->connection);
                if (client->server_conn->shoutcast_compat)
                    client->ops = &shoutcast_source_ops;
                else
                    client->ops = &http_request_ops;
                break;
            }
        }
        // long num = global.clients;
        global_unlock ();
        client->flags |= CLIENT_ACTIVE;
        return client;
    } while (0);

    free (client);
    sock_close (sock);
    return NULL;
}


/* shoutcast source clients are handled specially because the protocol is limited. It is
 * essentially a password followed by a series of headers, each on a separate line.  In here
 * we get the password and build a http request like a native source client would do
 */
static int shoutcast_source_client (client_t *client)
{
    do
    {
        connection_t *con = &client->connection;
        if (con->error || con->discon.time <= client->worker->current_time.tv_sec)
            break;

        if (client->shared_data)  /* need to get password first */
        {
            refbuf_t *refbuf = client->shared_data;
            int remaining = PER_CLIENT_REFBUF_SIZE - 2 - refbuf->len, ret, len;
            char *buf = refbuf->data + refbuf->len;
            char *esc_header;
            refbuf_t *r, *resp;
            char header [128];

            if (remaining == 0)
                break;

            ret = client_read_bytes (client, buf, remaining);
            if (ret == 0 || con->error || global.running != ICE_RUNNING)
                break;
            if (ret < 0)
                return 0;

            refbuf->len += ret;
            buf [ret] = '\0';
            len = strcspn (refbuf->data, "\r\n");
            if (refbuf->data [len] == '\0')  /* no EOL yet */
                return 0;

            refbuf->data [len] = '\0';
            snprintf (header, sizeof(header), "source:%s", refbuf->data);
            esc_header = util_base64_encode (header);

            len += 1 + strspn (refbuf->data+len+1, "\r\n");
            r = refbuf_new (PER_CLIENT_REFBUF_SIZE);
            snprintf (r->data, PER_CLIENT_REFBUF_SIZE,
                    "SOURCE %s HTTP/1.0\r\n" "Authorization: Basic %s\r\n%s",
                    client->server_conn->shoutcast_mount, esc_header, refbuf->data+len);
            r->len = strlen (r->data);
            free (esc_header);
            client->respcode = 200;
            resp = refbuf_new (30);
            snprintf (resp->data, 30, "OK2\r\nicy-caps:11\r\n\r\n");
            resp->len = strlen (resp->data);
            resp->associated = r;
            client->refbuf = resp;
            refbuf_release (refbuf);
            client->shared_data = NULL;
            INFO1 ("emulation on %s", client->server_conn->shoutcast_mount);
        }
        format_generic_write_to_client (client);
        if (client->pos == client->refbuf->len)
        {
            refbuf_t *r = client->refbuf;
            client->shared_data = r->associated;
            client->refbuf = NULL;
            r->associated = NULL;
            refbuf_release (r);
            client->ops = &http_request_ops;
            client->pos = 0;
        }
        client->schedule_ms = client->worker->time_ms + 100;
        return 0;
    } while (0);

    refbuf_release (client->shared_data);
    client->shared_data = NULL;
    return -1;
}


// NOTE: stream data may be in the block after headers
//
int setup_source_client_callback (client_t *client)
{
    refbuf_t *buf = client->refbuf;

    if (client->format_data == NULL)
    {
        const char *expect = httpp_getvar (client->parser, "expect");
        int len = buf->len - client->pos;

        if (len)
        {
            refbuf_t *stream = refbuf_new (len);
            memcpy (stream->data, buf->data+client->pos, len);
            buf->associated = stream;
            buf->len -= len;
            DEBUG1 ("found %d bytes of stream data after headers", len);
        }
        if (expect)
        {
           if (strcasecmp (expect, "100-continue") == 0)
           {
               DEBUG0 ("client expects 100 continue");
               snprintf (buf->data, PER_CLIENT_REFBUF_SIZE, "HTTP/1.1 100 Continue\r\n\r\n");
               buf->len = strlen (buf->data);
               client->format_data = buf;
               client->pos = 0;
               client_send_buffer_callback (client, setup_source_client_callback);
               return 0;  // need to send this straight away
           }
           INFO1 ("Received Expect header: %s", expect);
        }
    }
    buf = buf->associated;
    client->refbuf->associated = NULL;
    refbuf_release (client->refbuf);
    client->refbuf = buf;
    client->pos = 0;
    client->format_data = NULL;
    client->ops = &http_req_source_ops;
    return 0;
}


static int http_client_request (client_t *client)
{
    refbuf_t *refbuf = client->shared_data;
    int remaining, ret = -1;

    if (global.running != ICE_RUNNING)
        return -1;
    if (refbuf == NULL)
    {
        client->shared_data = refbuf = refbuf_new (PER_CLIENT_REFBUF_SIZE);
        refbuf->len = 0; // for building up the request coming in
    }
    remaining = PER_CLIENT_REFBUF_SIZE - 1 - refbuf->len;

    if (remaining && client->connection.discon.time > client->worker->current_time.tv_sec)
    {
        char *buf = refbuf->data + refbuf->len;

        ret = client_read_bytes (client, buf, remaining);
        if (ret > 0)
        {
            char *ptr;

            buf [ret] = '\0';
            refbuf->len += ret;
            if (memcmp (refbuf->data, "<policy-file-request/>", 23) == 0)
            {
                fbinfo fb;
                memset (&fb, 0, sizeof(fb));
                fb.mount = "/flashpolicy";
                fb.flags = FS_USE_ADMIN;
                fb.type = FORMAT_TYPE_UNDEFINED;
                client->respcode = 200;
                refbuf_release (refbuf);
                client->shared_data = NULL;
                client->check_buffer = format_generic_write_to_client;
                return fserve_setup_client_fb (client, &fb);
            }
            /* find a blank line */
            do
            {
                buf = refbuf->data;
                ptr = strstr (buf, "\r\n\r\n");
                if (ptr)
                {
                    ptr += 4;
                    break;
                }
                ptr = strstr (buf, "\n\n");
                if (ptr)
                {
                    ptr += 2;
                    break;
                }
                ptr = strstr (buf, "\r\r\n\r\r\n");
                if (ptr)
                {
                    ptr += 6;
                    break;
                }
                client->schedule_ms = client->worker->time_ms + 100;
                return 0;
            } while (0);
            client->refbuf = client->shared_data;
            client->shared_data = NULL;
            client->connection.discon.time = 0;
            client->parser = httpp_create_parser();
            httpp_initialize (client->parser, NULL);
            if (httpp_parse (client->parser, refbuf->data, refbuf->len))
            {
                const char *str;

                if (useragents.filename)
                {
                    const char *agent = httpp_getvar (client->parser, "user-agent");

                    if (agent && cached_pattern_search (&useragents, agent, client->worker->current_time.tv_sec) > 0)
                    {
                        INFO2 ("dropping client at %s because useragent is %.70s",
                                client->connection.ip, agent);
                        return -1;
                    }
                }

                /* headers now parsed, make sure any sent content is next */
                str = httpp_getvar (client->parser, HTTPP_VAR_PROTOCOL);
                if (strcmp("ICE", str) && strcmp("HTTP", str))
                {
                    ERROR2("Bad protocol (%.15s) detected from %s", str, &client->connection.ip[0]);
                    return -1;
                }
                str = httpp_getvar (client->parser, HTTPP_VAR_VERSION);
                if (str && strcmp (str, "1.1") == 0)
                    client->flags |= CLIENT_KEEPALIVE;  // make default for 1.1

                str = httpp_getvar (client->parser, "connection");
                if (str)
                {
                    if (strcasecmp (str, "keep-alive") == 0)
                        client->flags |= CLIENT_KEEPALIVE;
                    else
                        client->flags &= ~CLIENT_KEEPALIVE;
                }

                auth_check_http (client);
                switch (client->parser->req_type)
                {
                    case httpp_req_head:
                    case httpp_req_get:
                        refbuf->len = PER_CLIENT_REFBUF_SIZE;
                        client->ops = &http_req_get_ops;
                        break;
                    case httpp_req_source:
                    case httpp_req_put:
                        client->pos = ptr - refbuf->data;
                        setup_source_client_callback (client);
                        break;
                    case httpp_req_stats:
                        refbuf->len = PER_CLIENT_REFBUF_SIZE;
                        client->ops = &http_req_stats_ops;
                        break;
                    case httpp_req_options:
                        return client_send_options (client);
                    default:
                        WARN1("unhandled request type from %s", client->connection.ip);
                        return client_send_501 (client);
                }
                client->counter = 0;
                return client->ops->process(client);
            }
            /* invalid http request */
            return -1;
        }
        if (ret && client->connection.error == 0)
        {
            /* scale up the retry time, very short initially, usual case */
            uint64_t diff = client->worker->time_ms - client->counter;
            diff >>= 1;
            if (diff > 200)
                diff = 200;
            client->schedule_ms = client->worker->time_ms + 6 + diff;
            return 0;
        }
    }
    refbuf_release (refbuf);
    client->shared_data = NULL;
    return -1;
}


static void *connection_thread (void *arg)
{
    ice_config_t *config;

#ifdef HAVE_SIGNALFD
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGTERM);
    sigfd = signalfd(-1, &mask, 0);
#endif

    config = config_get_config ();
    /* setup the banned/allowed IP filenames from the xml */
    cached_file_init (&banned_ip,  config->banfile,   add_banned_ip, compare_banned_ip);
    cached_file_init (&allowed_ip, config->allowfile, NULL, NULL);
    cached_file_init (&useragents, config->agentfile, NULL, NULL);

    get_ssl_certificate (config);
    connection_setup_sockets (config);
    header_timeout = config->header_timeout;
    config_release_config ();

    connection_running = 1;
    INFO0 ("connection thread started");

    while (connection_running)
    {
        client_t *client = accept_client ();
        if (client)
        {
            /* do a small delay here so the client has chance to send the request after
             * getting a connect. */
            client->counter = client->schedule_ms = timing_get_time();
            client->connection.con_time = client->schedule_ms/1000;
            client->connection.discon.time = client->connection.con_time + header_timeout;
            client->schedule_ms += 6;
            client_add_worker (client);
            stats_event_inc (NULL, "connections");
        }
        if (global.new_connections_slowdown)
            thread_sleep (global.new_connections_slowdown * 5000);
    }
#ifdef HAVE_OPENSSL
    SSL_CTX_free (ssl_ctx);
#endif
    global_lock();
    cached_file_clear (&banned_ip);
    cached_file_clear (&allowed_ip);
    cached_file_clear (&useragents);
    global_unlock();
    connection_close_sigfd ();

    INFO0 ("connection thread finished");

    return NULL;
}


void connection_thread_startup ()
{
#ifdef HAVE_SIGNALFD
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask (SIG_SETMASK, &mask, NULL);
#endif
    connection_running = 0;
    if (conn_tid)
        WARN0("id for connection thread still set");

    conn_tid = thread_create ("connection", connection_thread, NULL, THREAD_ATTACHED);
}


void connection_thread_shutdown ()
{
    if (conn_tid)
    {
        connection_running = 0;
        INFO0("shutting down connection thread");
        thread_join (conn_tid);
        conn_tid = NULL;
    }
}


static int _check_pass_http(http_parser_t *parser, 
        const char *correctuser, const char *correctpass)
{
    /* This will look something like "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==" */
    const char *header = httpp_getvar(parser, "authorization");
    char *userpass, *tmp;
    char *username, *password;

    if(header == NULL)
        return 0;

    if(strncmp(header, "Basic ", 6))
        return 0;

    userpass = util_base64_decode(header+6);
    if(userpass == NULL) {
        WARN1("Base64 decode of Authorization header \"%s\" failed",
                header+6);
        return 0;
    }

    tmp = strchr(userpass, ':');
    if(!tmp) {
        free(userpass);
        return 0;
    }
    *tmp = 0;
    username = userpass;
    password = tmp+1;

    if(strcmp(username, correctuser) || strcmp(password, correctpass)) {
        free(userpass);
        return 0;
    }
    free(userpass);

    return 1;
}

static int _check_pass_icy(http_parser_t *parser, const char *correctpass)
{
    const char *password;

    password = httpp_getvar(parser, HTTPP_VAR_ICYPASSWORD);
    if(!password)
        return 0;

    if (strcmp(password, correctpass))
        return 0;
    else
        return 1;
}

static int _check_pass_ice(http_parser_t *parser, const char *correctpass)
{
    const char *password;

    password = httpp_getvar(parser, "ice-password");
    if(!password)
        password = "";

    if (strcmp(password, correctpass))
        return 0;
    else
        return 1;
}

int connection_check_admin_pass(http_parser_t *parser)
{
    int ret;
    ice_config_t *config = config_get_config();
    char *pass = config->admin_password;
    char *user = config->admin_username;
    const char *protocol;

    if(!pass || !user) {
        config_release_config();
        return 0;
    }

    protocol = httpp_getvar (parser, HTTPP_VAR_PROTOCOL);
    if (protocol && strcmp (protocol, "ICY") == 0)
        ret = _check_pass_icy (parser, pass);
    else 
        ret = _check_pass_http (parser, user, pass);
    config_release_config();
    return ret;
}

int connection_check_relay_pass(http_parser_t *parser)
{
    int ret;
    ice_config_t *config = config_get_config();
    char *pass = config->relay_password;
    char *user = config->relay_username;

    if(!pass || !user) {
        config_release_config();
        return 0;
    }

    ret = _check_pass_http(parser, user, pass);
    config_release_config();
    return ret;
}


/* return 0 for failed, 1 for ok
 */
int connection_check_pass (http_parser_t *parser, const char *user, const char *pass)
{
    int ret;
    const char *protocol;

    if(!pass) {
        WARN0("No source password set, rejecting source");
        return -1;
    }

    protocol = httpp_getvar(parser, HTTPP_VAR_PROTOCOL);
    if(protocol != NULL && !strcmp(protocol, "ICY")) {
        ret = _check_pass_icy(parser, pass);
    }
    else {
        ret = _check_pass_http(parser, user, pass);
        if (!ret)
        {
            ice_config_t *config = config_get_config_unlocked();
            if (config->ice_login)
            {
                ret = _check_pass_ice(parser, pass);
                if(ret)
                    WARN0("Source is using deprecated icecast login");
            }
        }
    }
    return ret;
}


static void _check_for_x_forwarded_for(ice_config_t *config, client_t *client)
{
    do {
        const char *hdr = httpp_getvar (client->parser, "x-forwarded-for");
        struct xforward_entry *xforward = config->xforward;
        if (hdr == NULL) break;
        while (xforward)
        {
            if (strcmp (xforward->ip, client->connection.ip) == 0)
            {
                int len = strcspn (hdr, ",") + 1;
                char *ip = malloc (len);

                snprintf (ip, len, "%s",  hdr);
                free (client->connection.ip);
                client->connection.ip = ip;
                DEBUG2 ("x-forward match for %s, using %s instead", xforward->ip, ip);
                break;
            }
            xforward = xforward->next;
        }
    } while(0);
}


static int _handle_source_request (client_t *client)
{
    const char *uri = httpp_getvar (client->parser, HTTPP_VAR_URI);
    ice_config_t *config = NULL;

    INFO1("Source logging in at mountpoint \"%s\"", uri);

    /* use x-forwarded-for ip address if available and authorized */
    config = config_get_config();
    _check_for_x_forwarded_for(config, client);
    config_release_config();
    
    if (uri[0] != '/')
    {
        WARN0 ("source mountpoint not starting with /");
        return client_send_401 (client, NULL);
    }
    switch (auth_check_source (client, uri))
    {
        case 0:         /* authenticated from config file */
            return source_startup (client, uri);
        case 1:         /* auth pending */
            break;
        default:        /* failed */
            INFO1("Source (%s) attempted to login with invalid or missing password", uri);
            return client_send_401 (client, NULL);
    }

    return 0;
}


static int _handle_stats_request (client_t *client)
{
    if (connection_check_admin_pass (client->parser))
        stats_add_listener (client, STATS_ALL);
    else
    {
        const char *uri = httpp_getvar (client->parser, HTTPP_VAR_URI);

        if (strcmp (uri, "/admin/streams") == 0 && connection_check_relay_pass (client->parser))
            stats_add_listener (client, STATS_SLAVE|STATS_GENERAL);
        else
            return auth_add_listener (uri, client);
    }
    return 0;
}


static void check_for_filtering (ice_config_t *config, client_t *client, char *uri)
{
    char *pattern = config->access_log.exclude_ext;
    char *extension = strrchr (uri, '.');
    const char *type = httpp_get_query_param (client->parser, "type");

    if ((extension && strcmp (extension+1, "flv") == 0) || 
        (type && (strcmp (type, ".flv") == 0 || strcmp (type, ".fla") == 0)))
    {
        client->flags |= CLIENT_WANTS_FLV;
        DEBUG0 ("listener has requested FLV");
    }
    if (extension == NULL || uri == NULL)
        return;

    extension++;
    if (pattern == NULL)
        return;
    while (*pattern)
    {
        int len = strcspn (pattern, " ");
        if (strncmp (extension, pattern, len) == 0 && extension[len] == '\0')
        {
            client->flags |= CLIENT_SKIP_ACCESSLOG;
            return;
        }
        pattern += len;
        len = strspn (pattern, " "); /* find next pattern */
        pattern += len;
    }
}


static int _handle_get_request (client_t *client)
{
    char *serverhost = NULL;
    int serverport = 0, ret = 0;
    aliases *alias;
    ice_config_t *config;
    char *uri = util_normalise_uri (httpp_getvar (client->parser, HTTPP_VAR_URI));
    int client_limit_reached = 0;

    if (uri == NULL)
        return client_send_400 (client, "invalid request URI");

    DEBUG1 ("start with %s", uri);
    config = config_get_config();
    check_for_filtering (config, client, uri);
    if (client->server_conn)
    {
        serverhost = client->server_conn->bind_address;
        serverport = client->server_conn->port;
    }
    _check_for_x_forwarded_for(config, client);

    alias = config->aliases;

    /* there are several types of HTTP GET clients
    ** media clients, which are looking for a source (eg, URI = /stream.ogg)
    ** stats clients, which are looking for /admin/stats.xml
    ** and directory server authorizers, which are looking for /GUID-xxxxxxxx 
    ** (where xxxxxx is the GUID in question) - this isn't implemented yet.
    ** we need to handle the latter two before the former, as the latter two
    ** aren't subject to the limits.
    */
    /* TODO: add GUID-xxxxxx */

    /* Handle aliases */
    while(alias) {
        if(strcmp(uri, alias->source) == 0 && (alias->port == -1 || alias->port == serverport) && (alias->bind_address == NULL || (serverhost != NULL && strcmp(alias->bind_address, serverhost) == 0))) {
            char *newuri = strdup (alias->destination);
            DEBUG2 ("alias has made %s into %s", uri, newuri);
            free (uri);
            uri = newuri;
            break;
        }
        alias = alias->next;
    }
    if (global.clients > config->client_limit)
    {
        client_limit_reached = 1;
        WARN3 ("server client limit reached (%d/%d) for %s", config->client_limit, global.clients, client->connection.ip);
    }
    config_release_config();

    stats_event_inc(NULL, "client_connections");

    if (strcmp (uri, "/admin.cgi") == 0 || strncmp("/admin/", uri, 7) == 0)
        ret = admin_handle_request (client, uri);
    else
    {
        /* drop non-admin GET requests here if clients limit reached */
        if (client_limit_reached)
            ret = client_send_403 (client, "Too many clients connected");
        else
            ret = auth_add_listener (uri, client);
    }
    free (uri);
    return ret;
}


/* close any open listening sockets 
 */
void connection_listen_sockets_close (ice_config_t *config, int all_sockets)
{
    if (global.serversock)
    {
        int old = 0, new = 0, cur = global.server_sockets;
        for (; old < cur; old++)
        {
            // close all listening sockets unless privileged ones are to stay open
            // and it is still present in the config.
            if (config && all_sockets == 0 && global.server_conn [old]->port < 1024)
            {
                listener_t *listener = config->listen_sock;
                while (listener)
                {
                    if (listener->port == global.server_conn [old]->port)
                    {
                        const char *new_bind = listener->bind_address ? listener->bind_address : "",
                              *old_bind = global.server_conn[old]->bind_address ? global.server_conn[old]->bind_address :  "";

                        if (strcmp (new_bind, old_bind) == 0)
                            break;
                    }
                    listener = listener->next;
                }
                if (listener)
                {
                    INFO2 ("Leaving port %d (%s) open", listener->port,
                            listener->bind_address ? listener->bind_address : "");
                    if (new < old)
                    {
                        global.server_conn [new] = global.server_conn [old];
                        global.serversock [new] = global.serversock [old];
                    }
                    new++;
                    continue;
                }
            }
            if (global.server_conn [old]->bind_address)
                INFO2 ("Closing port %d on %s", global.server_conn [old]->port, global.server_conn [old]->bind_address);
            else
                INFO1 ("Closing port %d", global.server_conn [old]->port);
            sock_close (global.serversock [old]);
            global.serversock [old] = SOCK_ERROR;
            config_clear_listener (global.server_conn [old]);
            global.server_sockets--;
        }
        if (global.server_sockets == 0)
        {
            free (global.serversock);
            global.serversock = NULL;
            free (global.server_conn);
            global.server_conn = NULL;
        }
    }
}


int connection_setup_sockets (ice_config_t *config)
{
    int count = 0;
    listener_t *listener, **prev;
    void *tmp;

    if (global.server_sockets >= config->listen_sock_count)
        return 0;
    global_lock();

    tmp = realloc (global.serversock, (config->listen_sock_count*sizeof (sock_t)));
    if (tmp) global.serversock = tmp;

    tmp = realloc (global.server_conn, (config->listen_sock_count*sizeof (listener_t*)));
    if (tmp) global.server_conn = tmp;

    listener = config->listen_sock;
    prev = &config->listen_sock;
    count = global.server_sockets;
    if (count)
        INFO1 ("%d listening sockets already open", count);
    while (listener)
    {
        int successful = 0;

        if (count > config->listen_sock_count)
        {
            ERROR2("sockets seem odd (%d,%d), skipping", count, config->listen_sock_count);
            break;
        }
        do
        {
            sock_t sock = sock_get_server_socket (listener->port, listener->bind_address);
            if (sock == SOCK_ERROR)
                break;
            /* some win32 setups do not do TCP win scaling well, so allow an override */
            if (listener->so_sndbuf)
                sock_set_send_buffer (sock, listener->so_sndbuf);
            if (listener->so_mss)
                sock_set_mss (sock, listener->so_mss);
            if (sock_listen (sock, listener->qlen) == SOCK_ERROR)
            {
                sock_close (sock);
                break;
            }
            sock_set_blocking (sock, 0);
            successful = 1;
            global.serversock [count] = sock;
            global.server_conn [count] = listener;
            listener->refcount++;
            count++;
        } while(0);
        if (successful == 0)
        {
            if (listener->bind_address)
                ERROR2 ("Could not create listener socket on port %d bind %s",
                        listener->port, listener->bind_address);
            else
                ERROR1 ("Could not create listener socket on port %d", listener->port);
            /* remove failed connection */
            *prev = config_clear_listener (listener);
            listener = *prev;
            continue;
        }
        if (listener->bind_address)
            INFO2 ("listener socket on port %d address %s", listener->port, listener->bind_address);
        else
            INFO1 ("listener socket on port %d", listener->port);
        prev = &listener->next;
        listener = listener->next;
    }
    global.server_sockets = count;
    global_unlock();

    if (count)
        INFO1 ("%d listening sockets setup complete", count);
    else
        ERROR0 ("No listening sockets established");
    return count;
}


void connection_close(connection_t *con)
{
    if (con->sock != SOCK_ERROR)
        sock_close (con->sock);
    free (con->ip);
#ifdef HAVE_OPENSSL
    if (con->ssl) { SSL_shutdown (con->ssl); SSL_free (con->ssl); }
#endif
    memset (con, 0, sizeof (connection_t));
    con->sock = SOCK_ERROR;
}

