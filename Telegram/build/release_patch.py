#!/usr/bin/python

# usage release_build.py <target>
import sys
import os

_THIS_DIR = os.path.dirname(__file__)

def main():
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
        # by default error code == 256
        # and it became 0 in some shells
        return 1 
    return 0

if __name__ == "__main__":
    sys.exit(main())
