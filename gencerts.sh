#!/bin/bash

cat > ssl/ca_config.conf << EndOfMessage
[ req ]
default_bits       = 4096
default_md         = sha512
default_keyfile    = domain.com.key
prompt             = no
encrypt_key        = no
distinguished_name = req_distinguished_name
# distinguished_name
[ req_distinguished_name ]
countryName            = "XX"                     # C=
localityName           = "XXXXX"                 # L=
organizationName       = "My Company"             # O=
organizationalUnitName = "Department"            # OU=
commonName             = "*"           # CN=
emailAddress           = "me@domain.com"          # email
EndOfMessage

openssl genrsa -out ssl/ca.key 2048
openssl req -x509 -new -nodes -key ssl/ca.key -sha256 -days 1024 -out ssl/ca.crt -config ssl/ca_config.conf
# Create Server Certificates
openssl genrsa -out ssl/server.key 2048
openssl req -new -key ssl/server.key -out ssl/server.csr -config ssl/ca_config.conf
openssl x509 -req -days 360 -in ssl/server.csr -CA ssl/ca.crt -CAkey ssl/ca.key -CAcreateserial -out ssl/server.crt
# Generate DH params file
openssl dhparam -out ssl/dh_params.dh 2048
