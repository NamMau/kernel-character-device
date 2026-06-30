# Linux character device

A small in-memory character driver for learning the Linux device model and
userspace/kernel data transfer.

The module:

- allocates its major and minor numbers dynamically;
- creates one or more device nodes through the Linux device model;
- supports synchronized reads and writes;
- stores data in a configurable FIFO ring buffer;
- appends writes and consumes data as it is read;
- supports blocking and nonblocking I/O;
- reports read and write readiness to `poll`, `select`, and `epoll`;
- supports `ioctl` controls for status, FIFO reset, and timer settings;
- provides a versioned status page through `mmap`;
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

The device count, FIFO size, and initial timer interval can be set when loading
the module:

```sh
sudo insmod mychardev.ko device_count=4 buffer_size=4096 timer_interval_ms=500
```

`device_count` must be between 1 and 64. When `device_count=1`, the driver
creates `/dev/mychardev`. When more than one device is requested, the driver
creates numbered nodes such as `/dev/mychardev0` and `/dev/mychardev1`.

`buffer_size` is expressed in bytes and must be between 1 and 1048576.
The default is 1024. Use `MYCHARDEV_IOC_GET_INFO` to read the active FIFO
capacity reported by the driver.

The initial `timer_interval_ms` value must be between 1 and 60000. The default
is 1000. The timer interval can also be changed later with `ioctl()`.

## Test

The device permissions depend on the system's `udev` rules, so these examples
use `sudo`:

```sh
printf 'hello from userspace\n' | sudo tee /dev/mychardev >/dev/null
sudo cat /dev/mychardev
```

For multiple devices, use the numbered node you want to test:

```sh
printf 'hello dev0\n' | sudo tee /dev/mychardev0 >/dev/null
sudo cat /dev/mychardev0
```

Writes append as much data as the ring buffer can hold. Reads consume available
data in FIFO order. By default, reads wait while the buffer is empty and writes
wait while it is full. A signal interrupts either wait.

Applications that open the device with `O_NONBLOCK` receive `EAGAIN` instead
of sleeping when a read cannot return data or a write cannot accept data.

Read readiness is reported while the FIFO contains data. Write readiness is
reported while the FIFO has free space. Applications can monitor these states
with `poll()`, `select()`, or `epoll()`.

## Device controls

The driver accepts these `ioctl()` commands:

```c
#define MYCHARDEV_IOC_MAGIC 'm'
#define MYCHARDEV_IOC_CLEAR _IO(MYCHARDEV_IOC_MAGIC, 0)
#define MYCHARDEV_IOC_GET_INFO _IOR(MYCHARDEV_IOC_MAGIC, 1, struct mychardev_info)
#define MYCHARDEV_IOC_RESET_TIMER _IO(MYCHARDEV_IOC_MAGIC, 2)
#define MYCHARDEV_IOC_GET_TIMER_INTERVAL _IOR(MYCHARDEV_IOC_MAGIC, 3, uint32_t)
#define MYCHARDEV_IOC_SET_TIMER_INTERVAL _IOW(MYCHARDEV_IOC_MAGIC, 4, uint32_t)
```

`MYCHARDEV_IOC_CLEAR` empties the FIFO and wakes blocked writers.
`MYCHARDEV_IOC_RESET_TIMER` sets the timer tick counter back to zero.
The timer interval is expressed in milliseconds and must be between 1 and
60000.

`MYCHARDEV_IOC_GET_INFO` copies this fixed-size status structure to userspace:

```c
struct mychardev_info {
	uint32_t buffer_size;
	uint32_t bytes_used;
	uint32_t bytes_free;
	uint32_t open_count;
	uint32_t mmap_count;
	uint32_t timer_interval_ms;
	uint32_t reserved;
	uint64_t timer_ticks;
};
```

## Shared memory

Applications can map one page from offset zero using `MAP_SHARED`:

```c
void *page;

page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
if (page == MAP_FAILED)
	perror("mmap");
```

Use `sysconf(_SC_PAGESIZE)` instead of assuming a 4096-byte page in production
code. The mapping is a read-only status protocol from the driver's point of
view. Applications must not write to fields in the mapped page.

The mapped page starts with this versioned structure:

```c
struct mychardev_shared {
	uint32_t magic;
	uint16_t version;
	uint16_t struct_size;
	uint32_t sequence;
	uint32_t minor;
	uint32_t buffer_size;
	uint32_t bytes_used;
	uint32_t bytes_free;
	uint32_t open_count;
	uint32_t mmap_count;
	uint32_t timer_interval_ms;
	uint32_t flags;
	uint64_t timer_ticks;
	uint64_t last_update_ns;
	uint64_t reserved[4];
};
```

`magic` is `0x4d434844`, `version` is `1`, and `struct_size` is the size of
the structure above. `sequence` is odd while the driver is updating the shared
page and even when a snapshot is complete. Userspace should read `sequence`,
copy the fields, then read `sequence` again and retry if either value is odd or
the values differ.

`flags` uses bit 0 for readable FIFO state and bit 1 for writable FIFO state.
`last_update_ns` is a monotonic timestamp from the kernel.

The driver refreshes this status after open, close, read, write, FIFO clear,
timer reset, timer interval changes, mmap lifetime changes, and timer ticks.
Direct mapped-memory accesses do not enter the driver, so the mapped page is
for observing driver-owned state rather than exchanging application data.

## Unload and clean

```sh
sudo rmmod mychardev
make clean
```

Do not unload the module while another process is using the device.
