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

int tsnlab_echo_init_service(ci_service_xdata_t * srv_xdata,
                      struct ci_server_conf *server_conf);
int tsnlab_echo_check_preview_handler(char *preview_data, int preview_data_len,
                               ci_request_t *);
int tsnlab_echo_end_of_data_handler(ci_request_t * req);
void *tsnlab_echo_init_request_data(ci_request_t * req);
void tsnlab_echo_close_service();
void tsnlab_echo_release_request_data(void *data);
int tsnlab_echo_io(char *wbuf, int *wlen, char *rbuf, int *rlen, int iseof,
            ci_request_t * req);


CI_DECLARE_MOD_DATA ci_service_module_t service = {
    "tsnlab_echo",                         /* mod_name, The module name */
    "TSNLAB Echo demo service",            /* mod_short_descr,  Module short description */
    ICAP_RESPMOD | ICAP_REQMOD,     /* mod_type, The service type is responce or request modification */
    tsnlab_echo_init_service,              /* mod_init_service. Service initialization */
    NULL,                           /* post_init_service. Service initialization after c-icap
                    configured. Not used here */
    tsnlab_echo_close_service,           /* mod_close_service. Called when service shutdowns. */
    tsnlab_echo_init_request_data,         /* mod_init_request_data */
    tsnlab_echo_release_request_data,      /* mod_release_request_data */
    tsnlab_echo_check_preview_handler,     /* mod_check_preview_handler */
    tsnlab_echo_end_of_data_handler,       /* mod_end_of_data_handler */
    tsnlab_echo_io,                        /* mod_service_io */
    NULL,
    NULL
};

/*
  The tsnlab_echo_req_data structure will store the data required to serve an ICAP request.
*/
struct tsnlab_echo_req_data {
    /*the body data*/
    ci_ring_buf_t *body;
    /*flag for marking the eof*/
    int eof;

#if 1
    ci_simple_file_t *file_t;
#endif
};


/* This function will be called when the service loaded  */
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

    return CI_OK;
}

/* This function will be called when the service shutdown */
void tsnlab_echo_close_service()
{
    ci_debug_printf(5,"Service shutdown!\n");
    /*Nothing to do*/
}

/*This function will be executed when a new request for tsnlab_echo service arrives. This function will
  initialize the required structures and data to serve the request.
 */
void *tsnlab_echo_init_request_data(ci_request_t * req)
{
    struct tsnlab_echo_req_data *tsnlab_echo_data;

    /*Allocate memory fot the tsnlab_echo_data*/
    tsnlab_echo_data = malloc(sizeof(struct tsnlab_echo_req_data));
    if (!tsnlab_echo_data) {
        ci_debug_printf(1, "Memory allocation failed inside tsnlab_echo_init_request_data!\n");
        return NULL;
    }

    /*If the ICAP request encuspulates a HTTP objects which contains body data
      and not only headers allocate a ci_cached_file_t object to store the body data.
     */
    if (ci_req_hasbody(req)) {
        tsnlab_echo_data->body = ci_ring_buf_new(4096);
#if 1 // jihoon
        #define DIR_PATH "/var/log/c-icap/"
        if (req->type == ICAP_RESPMOD) {
            tsnlab_echo_data->file_t = ci_simple_file_new(0);
            //tsnlab_echo_data->file_t = ci_simple_file_named_new(DIR_PATH, "0000.txt", 0);
            if(!tsnlab_echo_data->file_t)
                ci_debug_printf(1, "TSNLAB: failed create the file: fd=%d\n", tsnlab_echo_data->file_t->fd);
        }
        else {
            tsnlab_echo_data->file_t = NULL;
        }
#endif
    }
    else {
        tsnlab_echo_data->body = NULL;
#if 1 // jihoon
        tsnlab_echo_data->file_t = NULL;
#endif
    }

    tsnlab_echo_data->eof = 0;
    /*Return to the c-icap server the allocated data*/
    return tsnlab_echo_data;
}

/*This function will be executed after the request served to release allocated data*/
void tsnlab_echo_release_request_data(void *data)
{
    /*The data points to the tsnlab_echo_req_data struct we allocated in function tsnlab_echo_init_service */
    struct tsnlab_echo_req_data *tsnlab_echo_data = (struct tsnlab_echo_req_data *)data;

    /*if we had body data, release the related allocated data*/
    if (tsnlab_echo_data->body)
        ci_ring_buf_destroy(tsnlab_echo_data->body);
#if 1 // jihoon
    if (tsnlab_echo_data->file_t) {
        ci_simple_file_destroy(tsnlab_echo_data->file_t);
        //ci_simple_file_release(tsnlab_echo_data->file_t);
    }
#endif

    free(tsnlab_echo_data);
}


static int whattodo = 0;
int tsnlab_echo_check_preview_handler(char *preview_data, int preview_data_len,
                               ci_request_t * req)
{
    ci_off_t content_len;

    /*Get the tsnlab_echo_req_data we allocated using the  tsnlab_echo_init_service  function*/
    struct tsnlab_echo_req_data *tsnlab_echo_data = ci_service_data(req);

    /*If there are is a Content-Length header in encupsulated Http object read it
     and display a debug message (used here only for debuging purposes)*/
    content_len = ci_http_content_length(req);
    ci_debug_printf(1, "TSNLAB: We expect to read :%" PRINTF_OFF_T " body data\n",
                    (CAST_OFF_T) content_len);

    /*If there are not body data in HTTP encapsulated object but only headers
      respond with Allow204 (no modification required) and terminate here the
      ICAP transaction */
    if (!ci_req_hasbody(req)) {
        ci_debug_printf(1, "TSNLAB: not ci_req_hasbody to CI_MOD_ALLOW204\n");
        return CI_MOD_ALLOW204;
    }

    /*Unlock the request body data so the c-icap server can send data before
      all body data has received */
    ci_req_unlock_data(req);

    /*If there are not preview data tell to the client to continue sending data
      (http object modification required). */
    if (!preview_data_len)
        return CI_MOD_CONTINUE;

    /* In most real world services we should decide here if we must modify/process
    or not the encupsulated HTTP object and return CI_MOD_CONTINUE or
    CI_MOD_ALLOW204 respectively. The decision can be taken examining the http
    object headers or/and the preview_data buffer.

    In this example service we just use the whattodo static variable to decide
    if we want to process or not the HTTP object.
         */
    if (whattodo == 0) {
        whattodo = 1;
        ci_debug_printf(1, "TSNLAB: tsnlab_echo service will process the request\n");

        /*if we have preview data and we want to proceed with the request processing
          we should store the preview data. There are cases where all the body
          data of the encapsulated HTTP object included in preview data. Someone can use
          the ci_req_hasalldata macro to  identify these cases*/
        if (preview_data_len) {
            ci_ring_buf_write(tsnlab_echo_data->body, preview_data, preview_data_len);
            tsnlab_echo_data->eof = ci_req_hasalldata(req);
#if 1 // jihoon
            ci_debug_printf(1, "TSNLAB: exist preview_data_len\n");
            ci_simple_file_write(tsnlab_echo_data->file_t, preview_data, preview_data_len, tsnlab_echo_data->eof);
#endif
        }
        return CI_MOD_CONTINUE;
    } else {
        whattodo = 0;
        /*Nothing to do just return an allow204 (No modification) to terminate here
         the ICAP transaction */
        ci_debug_printf(1, "Allow 204...\n");
        return CI_MOD_ALLOW204;
    }
}

/* This function will called if we returned CI_MOD_CONTINUE in  tsnlab_echo_check_preview_handler
 function, after we read all the data from the ICAP client*/
int tsnlab_echo_end_of_data_handler(ci_request_t * req)
{
    struct tsnlab_echo_req_data *tsnlab_echo_data = ci_service_data(req);

#if 1 // jihoon
    ci_debug_printf(1, "File Length: %ld\n", tsnlab_echo_data->file_t->endpos);
#endif

    /*mark the eof*/
    tsnlab_echo_data->eof = 1;
    /*and return CI_MOD_DONE */
    return CI_MOD_DONE;
}

/* This function will called if we returned CI_MOD_CONTINUE in  tsnlab_echo_check_preview_handler
   function, when new data arrived from the ICAP client and when the ICAP client is
   ready to get data.
*/
int tsnlab_echo_io(char *wbuf, int *wlen, char *rbuf, int *rlen, int iseof,
            ci_request_t * req)
{
    int ret;
    struct tsnlab_echo_req_data *tsnlab_echo_data = ci_service_data(req);
    ret = CI_OK;

    /* Start echo: rbuf(origin) to wbuf(echo) */

    /*write the data read from icap_client to the tsnlab_echo_data->body*/
    if (rlen && rbuf) {

#if 1 // jihoon
        if (req->hasbody && req->type == ICAP_RESPMOD) {
            ret = ci_simple_file_write(tsnlab_echo_data->file_t, rbuf, *rlen, iseof);
            //ci_debug_printf(1, "read data to file: %d\n", ret);
        }
#endif

        *rlen = ci_ring_buf_write(tsnlab_echo_data->body, rbuf, *rlen);
        if (*rlen < 0)
            ret = CI_ERROR;
    }

    /*read some data from the tsnlab_echo_data->body and put them to the write buffer to be send
     to the ICAP client*/
    if (wbuf && wlen) {
        *wlen = ci_ring_buf_read(tsnlab_echo_data->body, wbuf, *wlen);
        if (*wlen == 0 && tsnlab_echo_data->eof == 1)
            *wlen = CI_EOF;
    }

    return ret;
}
