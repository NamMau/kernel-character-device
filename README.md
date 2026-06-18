# Linux character device

A small in-memory character driver for learning the Linux device model and
userspace/kernel data transfer.

The module:

- allocates its major and minor numbers dynamically;
- creates `/dev/mychardev` through the Linux device model;
- supports synchronized reads and writes;
- stores one message of at most 1023 bytes;
- replaces the previous message on every successful write;
- does not support seeking.

## Build and load

Install the kernel development package matching the running kernel, then run:

```sh
make
sudo insmod mychardev.ko
ls -l /dev/mychardev
sudo dmesg | tail
```

The device node should be created automatically by `udev`. No manual `mknod`
command is required.

## Test

The device permissions depend on the system's `udev` rules, so these examples
use `sudo`:

```sh
printf 'hello from userspace\n' | sudo tee /dev/mychardev >/dev/null
sudo cat /dev/mychardev
```

Each write replaces the stored message. A write of 1024 bytes or more fails
with `EMSGSIZE` instead of silently truncating data.

## Unload and clean

```sh
sudo rmmod mychardev
make clean
```

Do not unload the module while another process is using the device.
