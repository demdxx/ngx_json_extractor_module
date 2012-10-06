Nginx JSON extractor
====================

 Copyright (c) 2012 Dmitry Ponomarev <demdxx@gmail.com> <br />
 The module allows you to deploy JSON into local variables Nginx server.
    
 @license MIT http://opensource.org/licenses/MIT <br />
 @libs libjansson https://github.com/akheron/jansson

Example
-------

Names of extract vars work as selectors.

```sh
location /test {
    json_ignore_prefix pfx_;
    json_extract "{\"status\": \"ok\", \"content\": {\"autor\": \"John Dawkins\", \"book\": \"Rogues and Marauders\" }}"
        $pfx_status $pfx_content__autor $pfx_content__book;
    add_header "Status" $pfx_status;
    add_header "Autor"  $pfx_content__autor;
    add_header "Book"   $pfx_content__book;
    ...
}
```

Install
-------

Like any other module Nginx.

```sh
./configure --add-module=<path-to-module>/ngx_json_extractor_module
make
```
