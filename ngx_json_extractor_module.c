/*
 * Copyright (c) 2012 Dmitry Ponomarev <demdxx@gmail.com>
 *
 * Nginx JSON extractor is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <jansson.h>

#if HAVE_LOCALE_H
#include <locale.h>
#endif

#ifndef NGX_JSON_EXTRACTOR_USE_DEFAULT_VALUE
#define NGX_JSON_EXTRACTOR_USE_DEFAULT_VALUE 1
#endif

#ifndef NGX_JSON_EXTRACTOR_CLEANUP_SPIKE
#define NGX_JSON_EXTRACTOR_CLEANUP_SPIKE 1
#endif

#include "ngx_json_extractor_module.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JSON_REJECT_DUPLICATES
    #define JSON_REJECT_DUPLICATES 0x1
#endif

#ifndef JSON_DISABLE_EOF_CHECK
    #define JSON_DISABLE_EOF_CHECK 0x2
#endif

#ifndef JSON_DECODE_ANY
    #define JSON_DECODE_ANY        0x4
#endif

#define l_isspace(c) ((c) == ' ' || (c) == '\n' || (c) == '\r' || (c) == '\t')

#define JSON_VAR_GEN_NAME_FORMAT "jsone_%p"

/**
 * Return a pointer to the first non-whitespace character of str.
 * Modifies str so that all trailing whitespace characters are
 * replaced by '\0'.
 */
static char *strip(char *str) {
    size_t length;
    char *result = str;
    while(*result && l_isspace(*result))
        result++;

    length = strlen(result);
    if(length == 0)
        return result;

    while(l_isspace(result[length - 1]))
        result[--length] = '\0';

    return result;
}


// Module inits
static ngx_int_t ngx_json_extractor_module_postinit(ngx_conf_t *cf);

#if (!NGX_JSON_EXTRACTOR_CLEANUP_SPIKE)
static ngx_int_t ngx_json_extractor_module_handler(ngx_http_request_t *r);
#endif

static void ngx_json_extractor_module_cleanup_handler(void *data);

// location configuration inits
static void *ngx_json_extractor_create_loc_conf(ngx_conf_t *cf);
static char *ngx_json_extractor_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

// Config Commands
static char * ngx_http_json_extract(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

// Variable accessors
static ngx_int_t ngx_http_json_extract_var(ngx_http_request_t *r, 
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_json_desc(ngx_http_request_t *r, 
    ngx_http_variable_value_t *v, uintptr_t data);

// Helpers
static json_t *get_json(ngx_http_request_t *r, uintptr_t data);
static ngx_int_t add_json_item(ngx_json_extractor_loc_t *lc,
    ngx_int_t index, ngx_uint_t data_index, uintptr_t data, void *pool);
static u_char *get_json_item(ngx_http_request_t *r, u_char *text,
    json_t *json);
static ngx_http_variable_t *get_head_var(ngx_http_request_t *r,
    ngx_http_variable_value_t *v);
static je_item_t *get_item_by_data(ngx_json_extractor_loc_t *olcf,
    uintptr_t data);


static ngx_command_t ngx_json_extractor_commands[] = {

    { ngx_string("json_ignore_prefix"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_json_extractor_loc_t, prefix),
      NULL },

    { ngx_string("json_name_seaprator"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_json_extractor_loc_t, separator),
      NULL },

#if (NGX_JSON_EXTRACTOR_USE_DEFAULT_VALUE)
    { ngx_string("json_default_value"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_json_extractor_loc_t, default_val),
      NULL },
#endif

    { ngx_string("json_extract"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_2MORE,
      ngx_http_json_extract,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};

static ngx_http_module_t ngx_json_extractor_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_json_extractor_module_postinit,    /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_json_extractor_create_loc_conf,    /* create location configuration */
    ngx_json_extractor_merge_loc_conf      /* merge location configuration */
};

ngx_module_t ngx_json_extractor_module = {
    NGX_MODULE_V1,
   &ngx_json_extractor_module_ctx,         /* module context */
    ngx_json_extractor_commands,           /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

/**
 * Alloc and copy string
 * @param pool
 * @param src
 * @param len
 * @return string or NULL
 */
static u_char *
ngx_http_json_pstrdup(ngx_pool_t *pool, u_char *src, ngx_int_t len)
{
    u_char  *dst;
    dst = ngx_pcalloc(pool, len + 1);
    if (dst == NULL) {
        return NULL;
    }
    ngx_memcpy(dst, src, len);
    return dst;
}

///////////////////////////////////////////////////////////////////////////////
/// MODULE ////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

static ngx_int_t
ngx_json_extractor_module_postinit(ngx_conf_t *cf)
{
#if (!NGX_JSON_EXTRACTOR_CLEANUP_SPIKE)
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    if (NULL==cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers.elts) {
        // Init handlers array
        if (NGX_OK!=ngx_array_init(
            &cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers,
            cf->pool, 1, sizeof(ngx_http_handler_pt)))
            return NGX_ERROR;
    }
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers);

    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_json_extractor_module_handler;
#endif
    return NGX_OK;
}

#if (!NGX_JSON_EXTRACTOR_CLEANUP_SPIKE)

static ngx_int_t
ngx_json_extractor_module_handler(ngx_http_request_t *r)
{
    ngx_pool_cleanup_t    *cln;

    cln = ngx_pool_cleanup_add(r->pool, 0);

    if(cln == NULL)
        return NGX_ERROR;

    // Set handler
    cln->handler = ngx_json_extractor_module_cleanup_handler;

    // Init data
    cln->data = r;

    return NGX_OK;
}

#endif

/**
 * Cleanup hundler, free all JSON objects
 * @param data ngx_http_request_t*
 */
static void
ngx_json_extractor_module_cleanup_handler(void *data)
{
    ngx_uint_t i;
    ngx_http_request_t *r;
    ngx_http_variable_value_t *vv;
    ngx_json_extractor_loc_t *olcf;

    r = data;
    olcf = ngx_http_get_module_loc_conf(r, ngx_json_extractor_module);

    if (NULL!=olcf->json_cache.elts) {
        for (i=0 ; i<olcf->json_cache.nelts ; i++) {
            vv = ngx_http_get_indexed_variable(r,
                    ((je_item_t *)olcf->json_cache.elts)[i].index);
            if (NULL!=vv)
                json_delete((json_t *)vv->data);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// LOCATION CONFIGS //////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

static void *
ngx_json_extractor_create_loc_conf(ngx_conf_t *cf)
{
    ngx_json_extractor_loc_t  *olcf;

    olcf = ngx_pcalloc(cf->pool, sizeof(ngx_json_extractor_loc_t));
    if (NULL == olcf) {
        return NULL;
    }
    return olcf;
}

static char *
ngx_json_extractor_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_json_extractor_loc_t *prev = parent;
    ngx_json_extractor_loc_t *conf = child;

    ngx_conf_merge_str_value(conf->prefix,      prev->prefix,      "");
    ngx_conf_merge_str_value(conf->separator,   prev->separator,   "");

#if (NGX_JSON_EXTRACTOR_USE_DEFAULT_VALUE)
    ngx_conf_merge_str_value(conf->default_val, prev->default_val, "");
#endif

    return NGX_CONF_OK;
}

///////////////////////////////////////////////////////////////////////////////
/// COMMANDS //////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

/**
 * Extract json vars
 * @param nginx config
 * @param cmd
 * @param conf
 * @return nginx state
 */
static char *
ngx_http_json_extract(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_uint_t                       i;
    ngx_str_t                        n;
    ngx_int_t                        index;
    ngx_str_t                       *value;
    ngx_http_variable_t             *v;

    ngx_json_extractor_loc_t   *olcf = conf;
    value = cf->args->elts;
    
    // Generate new temp var
    n.len = snprintf(NULL, 0, JSON_VAR_GEN_NAME_FORMAT, value[1].data);
    n.data = ngx_pcalloc(cf->pool, n.len + 1);
    
    if (NULL==n.data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Invalid memory alloc");
        return NGX_CONF_ERROR;
    }
    sprintf((char *)n.data, JSON_VAR_GEN_NAME_FORMAT, value[1].data);
    
    // Add JSON handle var
    v = ngx_http_add_variable(cf, &n, NGX_HTTP_VAR_CHANGEABLE);
    if (NULL == v) {
        return NGX_CONF_ERROR;
    }

    index = ngx_http_get_variable_index(cf, &n);
    if (NGX_ERROR == index) {
        return NGX_CONF_ERROR;
    }
    
    v->index        = index;
    v->data         = (uintptr_t) value[1].data;
    v->get_handler  = ngx_http_json_desc;
    index           = NGX_CONF_UNSET_UINT;
    
    // If it varname
    if ('$'==value[1].data[0]) {
        n.len = value[1].len-1;
        n.data = value[1].data+1;
        index = ngx_http_get_variable_index(cf, &n);
        if (NGX_ERROR == index) {
            return NGX_CONF_ERROR;
        }
    }
    
    // Add item link
    if (NGX_OK!=add_json_item(olcf, v->index, index, v->data, cf->pool)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Invalid memory alloc");
        return NGX_CONF_ERROR;
    }
    
    // Process values
    for (i=2 ; i<cf->args->nelts ; i++) {
        if ('$' != value[i].data[0]) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "Invalid variable name: [%V]", &value[i]);
            return NGX_CONF_ERROR;
        }
        
        value[i].data++;
        value[i].len--;
    
        v = ngx_http_add_variable(cf, &value[i], NGX_HTTP_VAR_CHANGEABLE);
        if (NULL == v) {
            return NGX_CONF_ERROR;
        }
    
        index = ngx_http_get_variable_index(cf, &value[i]);
        if (NGX_ERROR == index) {
            return NGX_CONF_ERROR;
        }
        
        v->index        = index;
        v->data         = (uintptr_t) value[1].data;
        v->get_handler  = ngx_http_json_extract_var;
    }
    return NGX_CONF_OK;
}

///////////////////////////////////////////////////////////////////////////////
/// VARIABLE //////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

/**
 * Get JSON value
 * @param r
 * @param v
 * @param data
 * @return NGX_[STATUS]
 */
static ngx_int_t
ngx_http_json_extract_var(ngx_http_request_t *r, 
    ngx_http_variable_value_t *v, uintptr_t data)
{
    if (0==data)
        return NGX_ERROR;
    
    ngx_http_variable_t *vv;
    json_t *j;
    u_char *stext, *val;
    ngx_json_extractor_loc_t *olcf;

    olcf = ngx_http_get_module_loc_conf(r, ngx_json_extractor_module);

    vv = get_head_var(r, v);
    if (NULL==vv) {
        ngx_log_error_core(NGX_LOG_EMERG, r->connection->log, 0,
            "Can`t find var");
        return NGX_ERROR;
    }

    // Get JSON loaded context
    j = get_json(r, data);
    if (NULL==j) {
        ngx_log_error_core(NGX_LOG_EMERG, r->connection->log, 0,
            "JSON extract error: %s", data);
        return NGX_ERROR;
    }

    // Search var
    stext = ngx_http_json_pstrdup(r->pool, vv->name.data, vv->name.len);
    
    // Add offset if have prefix
    if (olcf->prefix.data!=NULL && olcf->prefix.len>0) {
        if (stext == (u_char *)ngx_strstr(stext, olcf->prefix.data))
            stext += olcf->prefix.len;
    }
    
    ngx_pfree(r->pool, stext);
    
    // Get JSON item text var
    val = get_json_item(r, stext, j);
    if (NULL==val) {

#if (NGX_JSON_EXTRACTOR_USE_DEFAULT_VALUE)
        v->len = olcf->default_val.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = olcf->default_val.data;
#else
        ngx_log_error_core(NGX_LOG_EMERG, r->connection->log, 0,
            "Failed value: %V", data);
        return NGX_ERROR;
#endif

    } else {
        v->len = ngx_strlen(val);
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = val;
    }
    return NGX_OK;
}

/**
 * Get JSON object
 * @param r
 * @param v
 * @param data
 * @return NGX_[STATUS]
 */
static ngx_int_t
ngx_http_json_desc(ngx_http_request_t *r, 
    ngx_http_variable_value_t *v, uintptr_t data)
{
    if (data==0)
        return NGX_ERROR;

    int flags = JSON_DECODE_ANY;
    json_t *json;
    json_error_t error;
    char *nm;

#if (NGX_JSON_EXTRACTOR_CLEANUP_SPIKE)
    /**
     * @SPIKE:
     * It did not work to get to work in a place where it
     * is written in the documentation. I hope this problem
     * will be solved, and then we can remove the flag NGX_JSON_EXTRACTOR_CLEANUP_SPIKE
     */

    ngx_pool_cleanup_t *cln = r->pool->cleanup;

    for ( ; cln ; cln = cln->next ) {
        if (ngx_json_extractor_module_cleanup_handler == cln->handler)
            goto pool_exist;
    }

    cln = ngx_pool_cleanup_add(r->pool, 0);

    if(cln == NULL)
        return NGX_ERROR;

    // Set handler
    cln->handler = ngx_json_extractor_module_cleanup_handler;

    // Init data
    cln->data = r;

pool_exist:

#endif

    nm = (char *)data;
    
    // If this is varname
    if ('$'==*nm) {
        je_item_t *jit;
        ngx_http_variable_value_t *v;
        ngx_json_extractor_loc_t *olcf;

        olcf = ngx_http_get_module_loc_conf(r, ngx_json_extractor_module);

        jit = get_item_by_data(olcf, data);
        
        if (NULL==jit || NGX_CONF_UNSET_UINT==jit->data_index) {
            ngx_log_error_core(NGX_LOG_EMERG, r->connection->log, 0,
                "Invalid JSON descriptor");
            return NGX_ERROR;
        }

        v = ngx_http_get_indexed_variable(r, jit->data_index);
        if (NULL == v) {
            return NGX_ERROR;
        }

        nm = (char *)v->data;
    }
    
    json = json_loads(strip(nm), flags, &error);

    if (NULL==json) {
        ngx_log_error_core(NGX_LOG_EMERG, r->connection->log, 0,
            "JSON string parse error: line[%d] column[%d] position[%d]\n%s",
                error.line, error.column, error.position, error.text);
        return NGX_ERROR;
    }

    v->len = 0;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->data = (u_char *)json;

    return NGX_OK;
}

///////////////////////////////////////////////////////////////////////////////
/// Helpers ///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

/**
 * Get JSON object from cache
 * @param r
 * @param v
 * @param data
 * @return JSON link
 */
static json_t *
get_json(ngx_http_request_t *r, uintptr_t data)
{
    ngx_uint_t i;
    ngx_http_variable_value_t *vv;
    ngx_json_extractor_loc_t *olcf;

    olcf = ngx_http_get_module_loc_conf(r, ngx_json_extractor_module);
    
    if (NULL!=olcf->json_cache.elts) {
        for (i=0 ; i<olcf->json_cache.nelts ; i++) {
            if (data==((je_item_t *)olcf->json_cache.elts)[i].data) {
                vv = ngx_http_get_indexed_variable(r,
                    ((je_item_t *)olcf->json_cache.elts)[i].index);
                return NULL==vv ? NULL : (json_t *)vv->data;
            }
        }
    }
    return NULL;
}

/**
 * Add JSON link to local config
 * @param lc
 * @param index
 * @param data
 * @param pool
 * @return NGX_[STATUS]
 */
static ngx_int_t
add_json_item(ngx_json_extractor_loc_t *lc,
    ngx_int_t index, ngx_uint_t data_index, uintptr_t data, void* pool)
{
    je_item_t  *it;

    if (NULL==lc->json_cache.elts) {
        if (ngx_array_init(&lc->json_cache, pool, 1, sizeof(je_item_t))!=NGX_OK)
            return NGX_ERROR;
    }

    it = ngx_array_push(&lc->json_cache);
    if (NULL==it)
        return NGX_ERROR;
    
    it->index       = index;
    it->data        = data;
    it->data_index  = data_index;
    
    return NGX_OK;
}

/**
 * Get string JSON val
 * @param r
 * @param text - search string "key1__key2__keyN"
 * @param json - searched object
 * @return JSON value as string or NULL
 */
static u_char *
get_json_item(ngx_http_request_t *r, u_char *text, json_t *json)
{
    if (!json_is_object(json))
        return NULL;

    u_char *separator, *end, *txt;
    json_t* val;
    ngx_json_extractor_loc_t *olcf;

    olcf = ngx_http_get_module_loc_conf(r, ngx_json_extractor_module);

    // Finde end of selector
    separator = olcf->separator.len>0
              ? olcf->separator.data
              : (u_char*)"__";
    end = (u_char*)ngx_strstr(text, separator);
    
    // Make next chan
    if (NULL!=end) {
        *end = '\0';
        end += olcf->separator.len>0 ? olcf->separator.len : 2;
    }
    
    // Get JSON value node
    val = json_object_get(json, (char *)text);

    if (NULL==val)
        return NULL;

    if (NULL!=end && '\0'!=*end) {
        return get_json_item(r, end, val);
    }

    // Convert value to string
    if (json_is_true(val)) {
        return (u_char *)"1";
    } else if (json_is_false(val)) {
        return (u_char *)"0";
    } else if (json_is_null(val)) {
        return (u_char *)"";
    } else if (json_is_string(val)) {
        txt = (u_char *)json_string_value(val);
        return ngx_http_json_pstrdup(r->pool, txt, ngx_strlen(txt));
    }

    txt = (u_char *)json_dumps(val, JSON_ENCODE_ANY);
    end = ngx_http_json_pstrdup(r->pool, txt, ngx_strlen(txt));
    free(txt);
    return end;
}

static ngx_http_variable_t *
get_head_var(ngx_http_request_t *r, ngx_http_variable_value_t *v)
{
    ngx_uint_t i;
    ngx_http_core_main_conf_t  *cmcf;
    
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);
    
    for (i=0 ; i<cmcf->variables.nelts ; i++) {
        if (v == r->variables+i)
            return ((ngx_http_variable_t*)cmcf->variables.elts)+i;
    }
    return NULL;
}

static je_item_t *
get_item_by_data(ngx_json_extractor_loc_t *olcf, uintptr_t data)
{
    ngx_uint_t i;
    for (i=0 ; i<olcf->json_cache.nelts ; i++) {
        if (data==((je_item_t *)olcf->json_cache.elts)[i].data)
            return ((je_item_t *)olcf->json_cache.elts)+i;
    }
    return NULL;
}

#ifdef __cplusplus
} // extern "C"
#endif