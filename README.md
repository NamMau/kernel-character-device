# Linux character device

A small in-memory character driver for learning the Linux device model and
userspace/kernel data transfer.

The module:

- allocates its major and minor numbers dynamically;
- creates `/dev/mychardev` through the Linux device model;
- supports synchronized reads and writes;
- stores up to 1024 bytes in a FIFO ring buffer;
- appends writes and consumes data as it is read;
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

Writes append as much data as the ring buffer can hold. Reads consume available
data in FIFO order. An empty read returns immediately with no data, and a write
to a full buffer fails with `ENOSPC`. Blocking behavior will be added in a
later phase.

## Unload and clean

```sh
sudo rmmod mychardev
make clean
```

Do not unload the module while another process is using the device.
