/*
 *  Copyright (C) 2004-2008 Christos Tsantilas
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA.
 */

#include "common.h"
#include "c-icap.h"
#include "service.h"
#include "header.h"
#include "body.h"
#include "simple_api.h"
#include "debug.h"

#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>

#define ASSETS_PATH "/var/log/c-icap/assets"
#define FILENAME_MAX_LEN 64

static const char *get_extension_from_content_type(ci_request_t * req);
static char *generate_unique_filename(const char *ext);

int tsnlab_echo_init_service(ci_service_xdata_t * srv_xdata, struct ci_server_conf *server_conf);
int tsnlab_echo_check_preview_handler(char *preview_data, int preview_data_len, ci_request_t *);
int tsnlab_echo_end_of_data_handler(ci_request_t * req);
void *tsnlab_echo_init_request_data(ci_request_t * req);
void tsnlab_echo_close_service();
void tsnlab_echo_release_request_data(void *data);
int tsnlab_echo_io(char *wbuf, int *wlen, char *rbuf, int *rlen, int iseof, ci_request_t * req);

CI_DECLARE_MOD_DATA ci_service_module_t service = {
    "tsnlab_echo",                      /* mod_name, The module name */
    "TSNLAB Echo demo service",         /* mod_short_descr,  Module short description */
    ICAP_RESPMOD | ICAP_REQMOD,         /* mod_type, The service type is responce or request modification */
    tsnlab_echo_init_service,           /* mod_init_service. Service initialization */
    NULL,                               /* post_init_service. Service initialization after c-icap configured. Not used here */
    tsnlab_echo_close_service,          /* mod_close_service. Called when service shutdowns. */
    tsnlab_echo_init_request_data,      /* mod_init_request_data */
    tsnlab_echo_release_request_data,   /* mod_release_request_data */
    tsnlab_echo_check_preview_handler,  /* mod_check_preview_handler */
    tsnlab_echo_end_of_data_handler,    /* mod_end_of_data_handler */
    tsnlab_echo_io,                     /* mod_service_io */
    NULL, NULL
};

struct tsnlab_echo_req_data {
    ci_simple_file_t *file_t;
    ci_ring_buf_t *body;
    int eof;
};

int tsnlab_echo_init_service(ci_service_xdata_t * srv_xdata,
                      struct ci_server_conf *server_conf)
{
    ci_debug_printf(5, "Initialization of tsnlab_echo module......\n");

    /*Tell to the icap clients that we can support up to 1024 size of preview data*/
    ci_service_set_preview(srv_xdata, 1024);

    /*Tell to the icap clients that we support 204 responses*/
    ci_service_enable_204(srv_xdata);

    /*Tell to the icap clients to send preview data for all files*/
    ci_service_set_transfer_preview(srv_xdata, "*");

    /*Tell to the icap clients that we want the X-Authenticated-User and X-Authenticated-Groups headers
      which contains the username and the groups in which belongs.  */
    ci_service_set_xopts(srv_xdata,  CI_XAUTHENTICATEDUSER|CI_XAUTHENTICATEDGROUPS);

    /* Create assets directory if it doesn't exist */
    if (access(ASSETS_PATH, F_OK) == -1) {
        if (mkdir(ASSETS_PATH, 0755) < 0 && errno != EEXIST) {
            ci_debug_printf(1, "TSNLAB: Failed to create assets directory: %s, error: %s\n", ASSETS_PATH, strerror(errno));
            return CI_ERROR;
        }
    }

    /* Set owner and group to c-icap */
    if (access(ASSETS_PATH, F_OK) == 0) {
        struct passwd *pwd = getpwnam("c-icap");
        struct group *grp = getgrnam("c-icap");
        if (pwd && grp) {
            if (chown(ASSETS_PATH, pwd->pw_uid, grp->gr_gid) < 0) {
                ci_debug_printf(1, "TSNLAB: Failed to chown %s to c-icap:c-icap: %s\n", ASSETS_PATH, strerror(errno));
            }
        } else {
            ci_debug_printf(1, "TSNLAB: c-icap user or group not found, skip chown\n");
        }
    }

    return CI_OK;
}

void tsnlab_echo_close_service()
{
    ci_debug_printf(5,"Service shutdown!\n");
}

void *tsnlab_echo_init_request_data(ci_request_t * req)
{
    struct tsnlab_echo_req_data *tsnlab_echo_data;

    tsnlab_echo_data = malloc(sizeof(struct tsnlab_echo_req_data));
    if (!tsnlab_echo_data) {
        ci_debug_printf(1, "Memory allocation failed inside tsnlab_echo_init_request_data!\n");
        return NULL;
    }

    tsnlab_echo_data->file_t = NULL;
    if (ci_req_hasbody(req))
        tsnlab_echo_data->body = ci_ring_buf_new(4096);
    else
        tsnlab_echo_data->body = NULL;
    tsnlab_echo_data->eof = 0;

    return tsnlab_echo_data;
}

void tsnlab_echo_release_request_data(void *data)
{
    struct tsnlab_echo_req_data *tsnlab_echo_data = (struct tsnlab_echo_req_data *)data;

    if (tsnlab_echo_data->body)
        ci_ring_buf_destroy(tsnlab_echo_data->body);
    if (tsnlab_echo_data->file_t)
        ci_simple_file_release(tsnlab_echo_data->file_t);

    free(tsnlab_echo_data);
}

int tsnlab_echo_check_preview_handler(char *preview_data, int preview_data_len,
                               ci_request_t * req)
{
    int ret;
    struct tsnlab_echo_req_data *tsnlab_echo_data = ci_service_data(req);
    
    if (!ci_req_hasbody(req)) {
        return CI_MOD_ALLOW204;
    }

    ci_req_unlock_data(req);

    if (!preview_data_len)
        return CI_MOD_CONTINUE;

    /* In most real world services we should decide here if we must modify/process
    or not the encupsulated HTTP object and return CI_MOD_CONTINUE or
    CI_MOD_ALLOW204 respectively. The decision can be taken examining the http
    object headers or/and the preview_data buffer.

    In this example service we just use the whattodo static variable to decide
    if we want to process or not the HTTP object. */

    if (req->type == ICAP_REQMOD) {
        return CI_MOD_ALLOW204;
    }

    if (preview_data_len && req->type == ICAP_RESPMOD) {
        char *filename;
        const char *ext;

        tsnlab_echo_data->eof = ci_req_hasalldata(req);
        ext = get_extension_from_content_type(req);
 
        /** 
         * If not html, return 204.
         * We only support html files for now.
         *
         * TODO: Add support for other file types 
         */
        if (strcmp(ext, "html")) {
            return CI_MOD_ALLOW204;
        }

        filename = generate_unique_filename(ext);
        if (!filename) {
            return CI_ERROR;
        }
        
        /* Create file in assets directory */
        tsnlab_echo_data->file_t = ci_simple_file_named_new((char *)ASSETS_PATH, filename, 0);
        free(filename);

        if (!tsnlab_echo_data->file_t) {
            ci_debug_printf(1, "Failed to create file in %s\n", ASSETS_PATH);
            return CI_ERROR;
        }

        if (tsnlab_echo_data->body)
            ci_ring_buf_write(tsnlab_echo_data->body, preview_data, preview_data_len);

        ret = ci_simple_file_write(tsnlab_echo_data->file_t, preview_data, preview_data_len, tsnlab_echo_data->eof);
        if (ret < 0) {
            ci_debug_printf(1, "Failed to write to file in %s\n", ASSETS_PATH);
            return CI_ERROR;
        }

        if (tsnlab_echo_data->eof) {
            ci_simple_file_release(tsnlab_echo_data->file_t);
            return CI_MOD_DONE;
        }
    }

    return CI_MOD_CONTINUE;
}

int tsnlab_echo_end_of_data_handler(ci_request_t * req)
{
    struct tsnlab_echo_req_data *tsnlab_echo_data = ci_service_data(req);

    if (tsnlab_echo_data->file_t)
        ci_debug_printf(1, "File Length: %ld\n", tsnlab_echo_data->file_t->endpos);

    if (tsnlab_echo_data->file_t) {
        ci_simple_file_release(tsnlab_echo_data->file_t);
        tsnlab_echo_data->file_t = NULL;
    }

    tsnlab_echo_data->eof = 1;

    return CI_MOD_DONE;
}

int tsnlab_echo_io(char *wbuf, int *wlen, char *rbuf, int *rlen, int iseof,
            ci_request_t * req)
{
    int ret;
    struct tsnlab_echo_req_data *tsnlab_echo_data = ci_service_data(req);

    ret = CI_OK;

    /* Same as echo_io: inbound data goes into the ring buffer */
    if (rlen && rbuf && req->type == ICAP_RESPMOD && req->hasbody) {
        int rd = *rlen;

        if (tsnlab_echo_data->body) {
            *rlen = ci_ring_buf_write(tsnlab_echo_data->body, rbuf, rd);
            if (*rlen < 0)
                ret = CI_ERROR;
        }

        if (ret == CI_OK && *rlen > 0) {
            if (!tsnlab_echo_data->file_t) {
                char *filename;
                const char *ext = get_extension_from_content_type(req);

                filename = generate_unique_filename(ext);
                if (!filename) {
                    return CI_ERROR;
                }
                tsnlab_echo_data->file_t = ci_simple_file_named_new((char *)ASSETS_PATH, filename, 0);
                free(filename);

                if (!tsnlab_echo_data->file_t) {
                    ci_debug_printf(1, "Failed to create file in %s\n", ASSETS_PATH);
                    return CI_ERROR;
                }
            }

            if (ci_simple_file_write(tsnlab_echo_data->file_t, rbuf, *rlen, iseof) < 0) {
                ci_debug_printf(1, "Failed to write to file in %s\n", ASSETS_PATH);
                return CI_ERROR;
            }
        }
    }

    /* Echo client output from ring buffer only (no ci_simple_file_read) */
    if (wbuf && wlen && tsnlab_echo_data->body) {
        *wlen = ci_ring_buf_read(tsnlab_echo_data->body, wbuf, *wlen);
        if (*wlen == 0 && tsnlab_echo_data->eof == 1)
            *wlen = CI_EOF;
    }

    return ret;
}

static char *generate_unique_filename(const char *ext)
{
    struct timespec ts;
    struct tm tm;
    struct timeval tv;
    char *filename;

    filename = malloc(FILENAME_MAX_LEN);
    if (!filename) {
        ci_debug_printf(5, "Failed to allocate filename buffer\n");
        return NULL;
    }

    if (gettimeofday(&tv, NULL) < 0) {
        free(filename);
        ci_debug_printf(5, "gettimeofday failed\n");
        return NULL;
    }
    ts.tv_sec = tv.tv_sec;
    ts.tv_nsec = (long)tv.tv_usec * 1000;

    if (!localtime_r(&ts.tv_sec, &tm)) {
        free(filename);
        ci_debug_printf(5, "localtime_r failed\n");
        return NULL;
    }

    unsigned int random_number = rand();

    snprintf(filename, FILENAME_MAX_LEN, "%04d%02d%02d_%02d%02d%02d_%09ld_%08X.%s",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            (long)ts.tv_nsec,
            random_number,
            ext ? ext : "bin");

    return filename;
}

static const char *get_extension_from_content_type(ci_request_t * req)
{
    const char *ct;

    if (req->type == ICAP_RESPMOD)
        ct = ci_http_response_get_header(req, "Content-Type");
    else
        ct = ci_http_request_get_header(req, "Content-Type");

    if (!ct)
        return "bin";

    if (strcasestr(ct, "text/html"))
        return "html";
    if (strcasestr(ct, "text/plain"))
        return "txt";
    if (strcasestr(ct, "text/css"))
        return "css";
    if (strcasestr(ct, "text/javascript") || strcasestr(ct, "application/javascript"))
        return "js";
    if (strcasestr(ct, "application/json"))
        return "json";
    if (strcasestr(ct, "image/jpeg") || strcasestr(ct, "image/jpg"))
        return "jpg";
    if (strcasestr(ct, "image/png"))
        return "png";
    if (strcasestr(ct, "image/gif"))
        return "gif";
    if (strcasestr(ct, "image/webp"))
        return "webp";
    if (strcasestr(ct, "application/xml") || strcasestr(ct, "text/xml"))
        return "xml";
    if (strcasestr(ct, "application/pdf"))
        return "pdf";

    return "bin";
}