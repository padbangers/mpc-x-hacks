# Building the shim

Cross-compiled for the MPC X (ARM, armhf, glibc) with Zig:

    zig cc -target arm-linux-gnueabihf.2.31 -shared -fPIC -O2 \
        -o shim_remap6.so shim_remap6.c -ldl -lpthread

The prebuilt `files/shim_remap6.so` is the result of this command.
