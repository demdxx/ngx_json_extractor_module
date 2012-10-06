/*
 * Copyright (c) 2012 Dmitry Ponomarev <demdxx@gmail.com>
 *
 * Nginx JSON extractor is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef _NGX_JSON_EXTRACTOR_MODULE_H_
#define _NGX_JSON_EXTRACTOR_MODULE_H_

typedef struct {
    uintptr_t   data;
    ngx_uint_t  data_index;
    ngx_uint_t  index;
} je_item_t;

typedef struct {
    ngx_str_t   prefix;
    ngx_str_t   separator;
#if (NGX_JSON_EXTRACTOR_USE_DEFAULT_VALUE)
    ngx_str_t   default_val;
#endif
    ngx_array_t json_cache;
} ngx_json_extractor_loc_t;

extern ngx_module_t  ngx_json_extractor_module;

#endif // _NGX_JSON_EXTRACTOR_MODULE_H_