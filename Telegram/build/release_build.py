#!/usr/bin/python

# usage release_build.py <target>
import sys
import os
import platform
import base64

_THIS_DIR = os.path.dirname(__file__)

TARGET_MAP = {
    "Win32": "win",
    "x64": "win64"
}

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

    # Apply patches
    patches = [
        ["../../cmake", "validate_d3d_compiler.py", "validate_d3d_compiler.patch"],
        ["../../cmake", "options_linux.cmake", "options_linux.patch"],
        ["../../cmake", "options_mac.cmake", "options_mac.patch"],
        ["../../cmake", "options_win.cmake", "options_win.patch"],
        ["../../cmake", "init_target.cmake", "init_target.patch"],
    ]
    print("Apply patches")
    errors = 0
    for fn_path, fn, patch_fn in patches:
        cmd = "cd %s && git checkout ./%s" % (os.path.join(_THIS_DIR, fn_path), fn)
        print("Call %s" % (cmd))
        os.system(cmd)
        cmd = "cd %s && git apply %s" % (os.path.join(_THIS_DIR, fn_path),
                                         os.path.join(_THIS_DIR, "patches/%s" % (patch_fn)))
        print("Call %s" % (cmd))
        errors += os.system(cmd)
    print("Patches done")
    if errors:
        print("Patches completed with errors")
        sys.exit(errors)

    if platform.system() == "Windows":
        cmd = "build.bat"
    elif platform.system() == "Linux":
        cmd = "bash ./build.sh -DDESKTOP_APP_DISABLE_CRASH_REPORTS=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON "
    else:
        cmd = "bash ./build.sh"
    cmd += " -DDESKTOP_APP_SPECIAL_TARGET=" + target
    print("Call %s" % (cmd))
    result = os.system(cmd)
    print("Finished: %s" % (result))
    sys.exit(result)

if __name__ == "__main__":
    main()
