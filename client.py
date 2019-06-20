#!/usr/bin/env python3

import redis
import os

conn = redis.StrictRedis(host=os.environ.get("REDIS_HOST"), port=int(os.environ.get("REDIS_PORT", 6401)), ssl=True, ssl_cert_reqs=None, ssl_ca_certs="/etc/redis/ssl/ca.crt", ssl_keyfile="/etc/redis/ssl/server.key", ssl_certfile="/etc/redis/ssl/server.crt")
print("Pong returned: ", conn.ping())
