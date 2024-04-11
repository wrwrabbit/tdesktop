#!/usr/bin/python3
import os
import re

KEY_NAME_PRIVATE = "./test-key-private.pem"
KEY_NAME_PUBLIC = "./test-key-public.pem"

SH_TEST_ENV = "./test-env.sh"

SRC_PACKER = "../SourceFiles/_other/packer.cpp"
SRC_CONFIG = "../SourceFiles/config.h"

if not os.path.exists(KEY_NAME_PRIVATE):
    print ("Generate new KEYS")
    os.system("openssl genrsa -out %s 1024" % (KEY_NAME_PRIVATE))
    os.system("openssl rsa -in %s -RSAPublicKey_out -out %s" % (KEY_NAME_PRIVATE, KEY_NAME_PUBLIC))
else:
    print ("Use existing KEYS")

## update cpp files
def update_source(fn):
    with open(KEY_NAME_PUBLIC, "r") as f:
        keydata = f.read()
    with open(fn, "r") as f:
        data = f.read()
    keydata = keydata.replace("\n", "\\n\\\n")[:-4]
    if keydata not in data:
        newdata = re.sub("-----BEGIN.*?---END RSA PUBLIC KEY-----", keydata.replace("\\", "\\\\"), data, flags = re.S + re.M)
        if newdata != data:
            print("Write new %s" % (fn))
            with open(fn, "w") as f:
                f.write(newdata)

update_source(SRC_PACKER)
update_source(SRC_CONFIG)

## set API_ID/HASH if not set
## set PTG_SAFETEST w/o signing
if not os.path.exists(SH_TEST_ENV):
    with open(SH_TEST_ENV, "w") as f:
        f.write("""#!/bin/sh
export PTG_TESTBUILD=1
export RSA_PRIVATE=`cat ./test-key-private.pem | base64`
""")
