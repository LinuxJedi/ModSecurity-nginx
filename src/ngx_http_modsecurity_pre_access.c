/*
 * ModSecurity connector for nginx, http://www.modsecurity.org/
 * Copyright (c) 2015 Trustwave Holdings, Inc. (http://www.trustwave.com/)
 *
 * You may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * If any of the files related to licensing are missing or if you have any
 * other questions related to licensing please contact Trustwave Holdings, Inc.
 * directly using the email address security@modsecurity.org.
 *
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include <nginx.h>
#include "ngx_http_modsecurity_pre_access.h"


void ngx_http_modsecurity_request_read(ngx_http_request_t *r)
{
    ngx_http_modsecurity_ctx_t *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_modsecurity);

#if defined(nginx_version) && nginx_version >= 8011
    r->main->count--;
#endif

    if (ctx->waiting_more_body)
    {
        ctx->waiting_more_body = 0;
        ngx_http_core_run_phases(r);
    }
}


ngx_int_t ngx_http_modsecurity_pre_access_handler(ngx_http_request_t *r)
{
#if 1
    ngx_http_modsecurity_ctx_t *ctx = NULL;
    ngx_http_modsecurity_loc_conf_t *cf;

    dd("catching a new _preaccess_ pahase handler");

    cf = ngx_http_get_module_loc_conf(r, ngx_http_modsecurity);
    if (cf == NULL || cf->enable != 1)
    {
        dd("ModSecurity not enabled... returning");
        return NGX_DECLINED;
    }
    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_POST) {
        dd("ModSecurity is not ready to deal with anything different from " \
            "POST or GET");
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_modsecurity);

    dd("recovering ctx: %p", ctx);

    if (ctx == NULL)
    {
        dd("ctx is null; Nothing we can do, returning an error.");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ctx->waiting_more_body == 1)
    {
        dd("waiting for more data before proceed. / count: %d",
            r->main->count);

        return NGX_DONE;
    }

    if (ctx->body_requested == 0)
    {
        ngx_int_t rc = NGX_OK;

        ctx->body_requested = 1;

        dd("asking for the request body, if any. Count: %d",
            r->main->count);
        /**
         * TODO: Check if there is any benefit to use request_body_in_single_buf set to 1.
         *
         *       saw some module using this request_body_in_single_buf
         *       but not sure what exactly it does, same for the others options below.
         *
         * r->request_body_in_single_buf = 1;
         */
        r->request_body_in_single_buf = 1;
        r->request_body_in_persistent_file = 1;
        r->request_body_in_clean_file = 1;

        rc = ngx_http_read_client_request_body(r,
            ngx_http_modsecurity_request_read);
        if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
#if (nginx_version < 1002006) ||                                             \
    (nginx_version >= 1003000 && nginx_version < 1003009)
            r->main->count--;
#endif

            return rc;
        }
        if (rc == NGX_AGAIN)
        {
            dd("nginx is asking us to wait for more data.");

            ctx->waiting_more_body = 1;
            return NGX_DONE;
        }
    }

    if (ctx->waiting_more_body == 0)
    {
        int ret = 0;
        int already_inspected = 0;

        dd("request body is ready to be processed");

        ngx_chain_t *chain = r->request_body->bufs;

        /**
         * TODO: Speed up the analysis by sending chunk while they arrive.
         *
         * Notice that we are waiting for the full request body to 
         * start to process it, it may not be necessary. We may send
         * the chunks to ModSecurity while nginx keep calling this
         * function.
         */


        if (r->request_body->temp_file != NULL) {
            ngx_str_t file_path = r->request_body->temp_file->file.name;
            const char *file_name = ngx_str_to_char(file_path, r->pool);
            /*
             * Request body was saved to a file, probably we don't have a
             * copy of it in memory.
             */
            dd("request body inspection: file -- %s", file_name);

            msc_request_body_from_file(ctx->modsec_transaction, file_name);

            already_inspected = 1;
        } else {
            dd("inspection request body in memory.");
        }

        while (chain && !already_inspected)
        {
            u_char *data = chain->buf->start;

            msc_append_request_body(ctx->modsec_transaction, data,
                chain->buf->end - data);

            if (chain->buf->last_buf)
            {
                break;
            }
            chain = chain->next;

            /**
             * ModSecurity may perform stream inspection on this buffer,
             * it may ask for a intervention in consequence of that.
             * 
             */
            ret = ngx_http_modsecurity_process_intervention(ctx->modsec_transaction, r);
            if (ret > 0)
            {
                return ret;
            }
        }

        /**
         * At this point, all the request body was sent to ModSecurity 
         * and we want to make sure that all the request body inspection
         * happened; consequently we have to check if ModSecurity have
         * returned any kind of intervention.
         */

        msc_process_request_body(ctx->modsec_transaction);
        ret = ngx_http_modsecurity_process_intervention(ctx->modsec_transaction, r);
        if (ret > 0)
        {
            return ret;
        }
    }

    dd("Nothing to add on the body inspection, reclaiming a NGX_DECLINED");
#endif
    return NGX_DECLINED;
}

