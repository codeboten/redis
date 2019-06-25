#!/usr/bin/env python3

import redis
import os

cacert = os.environ.get("REDIS_SSL_DIR", "/etc/redis/ssl") + "/ca.crt"
sslkeyfile = os.environ.get("REDIS_SSL_DIR", "/etc/redis/ssl") + "/server.key"
sslcert = os.environ.get("REDIS_SSL_DIR", "/etc/redis/ssl") + "/server.crt"

conn = redis.StrictRedis(host=os.environ.get("REDIS_HOST"), port=int(os.environ.get("REDIS_PORT", 6401)), ssl=True, ssl_cert_reqs=None, ssl_ca_certs=cacert, ssl_keyfile=sslkeyfile, ssl_certfile=sslcert)
print("Pong returned: ", conn.ping())
