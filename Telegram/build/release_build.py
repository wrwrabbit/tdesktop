#!/usr/bin/python

# usage release_build.py <target>
import sys
import os
import platform
import base64
import json
import subprocess
import tempfile
import release_patch

_THIS_DIR = os.path.dirname(__file__)

TARGET_MAP = {
    "x64_x86": "win",
    "x64": "win64"
}

def prepare_update_signing(path):
    for secret, filename in (
        ("UPDATE_ED25519_PRIVATE", "release-local.pem"),
        ("UPDATE_ES256_PRIVATE", "release-es256.pem"),
    ):
        value = os.environ.get(secret)
        if not value:
            raise RuntimeError("Missing release secret: " + secret)
        key_path = os.path.join(path, filename)
        with open(key_path, "wb") as output:
            output.write(base64.b64decode(value, validate=True))
        os.chmod(key_path, 0o600)
    config = json.loads(os.environ["UPDATE_SIGNING_CONFIG"])
    if config.get("backend") != "openssl":
        raise RuntimeError("Release secrets require the OpenSSL signing backend.")
    config["local_key"] = os.path.join(path, "release-local.pem")
    config["es256_key"] = os.path.join(path, "release-es256.pem")
    config_path = os.path.join(path, "update-signing.json")
    with open(config_path, "w", encoding="utf-8") as output:
        json.dump(config, output)
    os.chmod(config_path, 0o600)
    subprocess.run([
        sys.executable, os.path.join(_THIS_DIR, "update_signing.py"),
        "--config", config_path, "--check",
    ], check=True)
    os.environ["TDESKTOP_UPDATE_SIGNING_CONFIG"] = config_path

def main():
    if len(sys.argv) != 2:
        print("Usage release_build.py <target>")
        sys.exit(1)
    target = sys.argv[1]
    target = TARGET_MAP.get(target, target)
    api_id = os.environ["API_ID"]
    api_hash = os.environ["API_HASH"]

    print("Set target = %s" % (target))
    file_target = os.path.join(_THIS_DIR, "target")
    with open(file_target, "w") as f:
        f.write(target)

    # Prepare dropbox path
    print("Stub output file locations")
    if platform.system() == "Windows":
        dropbox_path = os.path.join(_THIS_DIR, "../../../Dropbox/Telegram/symbols")
        if not os.path.exists(dropbox_path):
            os.makedirs(dropbox_path)
        # Prepare release path
        release_path = os.path.join(_THIS_DIR, "../../../Projects/backup/tdesktop")
        if not os.path.exists(release_path):
            os.makedirs(release_path)
    else:
        dropbox_path = os.path.join(_THIS_DIR, "../../out/Dropbox/Telegram/symbols")
        if not os.path.exists(dropbox_path):
            os.makedirs(dropbox_path)
        # Prepare release path
        release_path = os.path.join(_THIS_DIR, "../../out/backup/tdesktop")
        if not os.path.exists(release_path):
            os.makedirs(release_path)

    print("Generate DesktopPrivate")
    private_path = os.path.join(_THIS_DIR, "../../../DesktopPrivate")
    if not os.path.exists(private_path):
        os.makedirs(private_path)
    print("Generate custom_api_id.h")
    with open(os.path.join(private_path, "custom_api_id.h"), "w") as f:
        f.write("const long ApiId = %s\n" % (api_id))
        f.write("const char* ApiHash = \"%s\"\n" % (api_hash))

    # RSA Keys
    print("Generate alpha_private.h")
    with open(os.path.join(private_path, "alpha_private.h"), "w") as f:
        f.write("static const char *AlphaPrivateKey = \"\";\n")
    if "RSA_PRIVATE" in os.environ:
        print("Generate packer_private.h")
        rsa_key = os.environ["RSA_PRIVATE"] # base64
        rsa_key = base64.b64decode(rsa_key).decode('ascii')
        rsa_key = rsa_key.splitlines()
        RSA_STRING = "\\n\\\n".join(rsa_key)
        cpp_file = "const char *PrivateBetaKey = \"\\\n" +\
                   RSA_STRING +\
                   "\";\n\n" +\
                   "const char *PrivateKey = \"\\\n" +\
                   RSA_STRING +\
                   "\";\n"
        with open(os.path.join(private_path, "packer_private.h"), "w") as f:
            f.write(cpp_file)

    if platform.system() == "Windows":
        print("Generate Sign.bat")
        with open(os.path.join(private_path, "Sign.bat"), "w") as f:
            pass

    with tempfile.TemporaryDirectory(prefix="update-signing-", dir=private_path) as signing_path:
        prepare_update_signing(os.path.abspath(signing_path))
        # Apply patches
        errors = release_patch.main()
        if errors:
            print("Patches completed with errors")
            sys.exit(errors)

        if platform.system() == "Windows":
            cmd = "build.bat"
        elif platform.system() == "Linux":
            cmd = "bash ./build.sh -DDESKTOP_APP_DISABLE_CRASH_REPORTS=OFF "
        else:
            cmd = "bash ./build.sh"
        cmd += " -DDESKTOP_APP_SPECIAL_TARGET=" + target
        print("Call %s" % (cmd))
        result = subprocess.call(cmd, shell=True)
        print("Finished: %s" % (result))
        sys.exit(result)

if __name__ == "__main__":
    main()
