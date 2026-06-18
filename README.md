# Linux character device

A small in-memory character driver for learning the Linux device model and
userspace/kernel data transfer.

The module:

- allocates its major and minor numbers dynamically;
- creates `/dev/mychardev` through the Linux device model;
- supports synchronized reads and writes;
- stores up to 1024 bytes in a FIFO ring buffer;
- appends writes and consumes data as it is read;
- supports blocking and nonblocking I/O;
- reports read and write readiness to `poll`, `select`, and `epoll`;
- provides one shared page through `mmap`;
- updates a timer tick counter once per second;
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
data in FIFO order. By default, reads wait while the buffer is empty and writes
wait while it is full. A signal interrupts either wait.

Applications that open the device with `O_NONBLOCK` receive `EAGAIN` instead
of sleeping when a read cannot return data or a write cannot accept data.

Read readiness is reported while the FIFO contains data. Write readiness is
reported while the FIFO has free space. Applications can monitor these states
with `poll()`, `select()`, or `epoll()`.

## Shared memory

Applications can map one page from offset zero using `MAP_SHARED`:

```c
void *page;

page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
if (page == MAP_FAILED)
	perror("mmap");
```

Use `sysconf(_SC_PAGESIZE)` instead of assuming a 4096-byte page in production
code. The mapping is separate from the FIFO used by `read()` and `write()`.
Processes sharing the page must provide their own synchronization and data
format because direct mapped-memory accesses do not enter the driver.

The first 64 bits of the mapped page are reserved for a timer tick counter.
The driver increments this counter once per second:

```c
volatile uint64_t *timer_ticks = page;

printf("timer ticks: %" PRIu64 "\n", *timer_ticks);
```

Include `<inttypes.h>` and `<stdint.h>` for this example. Applications must
treat the counter as read-only. The remaining bytes in the page are available
for application-defined shared data.

## Unload and clean

```sh
sudo rmmod mychardev
make clean
```

Do not unload the module while another process is using the device.
