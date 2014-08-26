websocket
=========

This's a nginx module that implements websocket protocol.

- install
```
{nginx_dir}/configure --add-module={module_dir}/ngx_http_websocket_module && make && make install
```

- conf
```
worker_processes  1;

events {
	worker_connections   1024;
}


http {
    include       mime.types;
    default_type  application/octet-stream;

	server {
		listen       80;

		location / {
			root   html/websocket;
			index  index.html index.htm;
		}

		location /ws {
			websocket;
		}
	}	
}
```
