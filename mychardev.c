// SPDX-License-Identifier: GPL-2.0

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/kfifo.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define DEVICE_NAME "mychardev"
#define CLASS_NAME "mychardev"
#define DEFAULT_DEVICE_COUNT 1
#define MAX_DEVICE_COUNT 64
#define DEFAULT_BUFFER_SIZE 1024
#define MAX_BUFFER_SIZE 1048576
#define DEFAULT_TIMER_INTERVAL_MS 1000
#define MIN_TIMER_INTERVAL_MS 1
#define MAX_TIMER_INTERVAL_MS 60000
#define MYCHARDEV_IOC_MAGIC 'm'
#define MYCHARDEV_IOC_CLEAR _IO(MYCHARDEV_IOC_MAGIC, 0)
#define MYCHARDEV_IOC_GET_INFO \
	_IOR(MYCHARDEV_IOC_MAGIC, 1, struct mychardev_info)
#define MYCHARDEV_IOC_RESET_TIMER _IO(MYCHARDEV_IOC_MAGIC, 2)
#define MYCHARDEV_IOC_GET_TIMER_INTERVAL \
	_IOR(MYCHARDEV_IOC_MAGIC, 3, __u32)
#define MYCHARDEV_IOC_SET_TIMER_INTERVAL \
	_IOW(MYCHARDEV_IOC_MAGIC, 4, __u32)

struct mychardev_info {
	__u32 buffer_size;
	__u32 bytes_used;
	__u32 bytes_free;
	__u32 open_count;
	__u32 mmap_count;
	__u32 timer_interval_ms;
	__u32 reserved;
	__u64 timer_ticks;
};

struct mychardev_shared {
	u64 timer_ticks;
};

struct mychardev {
	struct cdev cdev;
	struct device *device;
	struct mutex lock;
	struct timer_list timer;
	atomic64_t timer_ticks;
	atomic_t open_count;
	atomic_t mmap_count;
	unsigned int minor;
	unsigned int timer_interval_ms;
	bool cdev_added;
	wait_queue_head_t read_queue;
	wait_queue_head_t write_queue;
	void *shared_page;
	struct kfifo buffer;
};

static unsigned int device_count = DEFAULT_DEVICE_COUNT;
module_param(device_count, uint, 0444);
MODULE_PARM_DESC(device_count, "Number of character devices to create");

static unsigned int buffer_size = DEFAULT_BUFFER_SIZE;
module_param(buffer_size, uint, 0444);
MODULE_PARM_DESC(buffer_size, "FIFO buffer size in bytes");

static unsigned int timer_interval_ms = DEFAULT_TIMER_INTERVAL_MS;
module_param(timer_interval_ms, uint, 0444);
MODULE_PARM_DESC(timer_interval_ms, "Initial timer interval in milliseconds");

static dev_t device_number;
static struct class *device_class;
static struct mychardev *devices;

static void mychardev_vma_open(struct vm_area_struct *vma)
{
	struct mychardev *dev = vma->vm_private_data;

	__module_get(THIS_MODULE);
	atomic_inc(&dev->mmap_count);
}

static void mychardev_vma_close(struct vm_area_struct *vma)
{
	struct mychardev *dev = vma->vm_private_data;

	atomic_dec(&dev->mmap_count);
	module_put(THIS_MODULE);
}

static const struct vm_operations_struct mychardev_vm_ops = {
	.open = mychardev_vma_open,
	.close = mychardev_vma_close,
};

static void mychardev_timer(struct timer_list *timer)
{
	struct mychardev *dev = timer_container_of(dev, timer, timer);
	struct mychardev_shared *shared = dev->shared_page;
	s64 ticks;

	ticks = atomic64_inc_return(&dev->timer_ticks);
	WRITE_ONCE(shared->timer_ticks, ticks);

	mod_timer(&dev->timer,
		  jiffies +
		  msecs_to_jiffies(READ_ONCE(dev->timer_interval_ms)));
}

static int mychardev_open(struct inode *inode, struct file *file)
{
	struct mychardev *dev;
	int ret;

	dev = container_of(inode->i_cdev, struct mychardev, cdev);

	ret = nonseekable_open(inode, file);
	if (ret)
		return ret;

	file->private_data = dev;
	atomic_inc(&dev->open_count);

	return 0;
}

static int mychardev_release(struct inode *inode, struct file *file)
{
	struct mychardev *dev = file->private_data;

	if (dev)
		atomic_dec(&dev->open_count);

	file->private_data = NULL;
	return 0;
}

static ssize_t mychardev_read(struct file *file, char __user *buffer,
			      size_t count, loff_t *offset)
{
	struct mychardev *dev = file->private_data;
	unsigned int copied = 0;
	int ret;

	if (!count)
		return 0;

	for (;;) {
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;

		if (!kfifo_is_empty(&dev->buffer))
			break;

		mutex_unlock(&dev->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(dev->read_queue,
					       !kfifo_is_empty(&dev->buffer));
		if (ret)
			return ret;
	}

	ret = kfifo_to_user(&dev->buffer, buffer, count, &copied);
	mutex_unlock(&dev->lock);

	if (copied)
		wake_up_interruptible(&dev->write_queue);

	return ret ? ret : copied;
}

static ssize_t mychardev_write(struct file *file,
			       const char __user *buffer, size_t count,
			       loff_t *offset)
{
	struct mychardev *dev = file->private_data;
	unsigned int copied = 0;
	int ret;

	if (!count)
		return 0;

	for (;;) {
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;

		if (!kfifo_is_full(&dev->buffer))
			break;

		mutex_unlock(&dev->lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(dev->write_queue,
					       !kfifo_is_full(&dev->buffer));
		if (ret)
			return ret;
	}

	ret = kfifo_from_user(&dev->buffer, buffer, count, &copied);
	mutex_unlock(&dev->lock);

	if (copied)
		wake_up_interruptible(&dev->read_queue);

	return ret ? ret : copied;
}

static __poll_t mychardev_poll(struct file *file, poll_table *wait)
{
	struct mychardev *dev = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &dev->read_queue, wait);
	poll_wait(file, &dev->write_queue, wait);

	mutex_lock(&dev->lock);

	if (!kfifo_is_empty(&dev->buffer))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!kfifo_is_full(&dev->buffer))
		mask |= EPOLLOUT | EPOLLWRNORM;

	mutex_unlock(&dev->lock);
	return mask;
}

static long mychardev_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct mychardev *dev = file->private_data;
	struct mychardev_shared *shared = dev->shared_page;
	struct mychardev_info info;
	__u32 interval;

	switch (cmd) {
	case MYCHARDEV_IOC_CLEAR:
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;

		kfifo_reset(&dev->buffer);
		mutex_unlock(&dev->lock);
		wake_up_interruptible(&dev->write_queue);
		return 0;

	case MYCHARDEV_IOC_GET_INFO:
		if (mutex_lock_interruptible(&dev->lock))
			return -ERESTARTSYS;

		info.buffer_size = kfifo_size(&dev->buffer);
		info.bytes_used = kfifo_len(&dev->buffer);
		info.bytes_free = kfifo_avail(&dev->buffer);
		info.open_count = atomic_read(&dev->open_count);
		info.mmap_count = atomic_read(&dev->mmap_count);
		info.timer_interval_ms = READ_ONCE(dev->timer_interval_ms);
		info.reserved = 0;
		info.timer_ticks = atomic64_read(&dev->timer_ticks);
		mutex_unlock(&dev->lock);

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;

	case MYCHARDEV_IOC_RESET_TIMER:
		atomic64_set(&dev->timer_ticks, 0);
		WRITE_ONCE(shared->timer_ticks, 0);
		return 0;

	case MYCHARDEV_IOC_GET_TIMER_INTERVAL:
		interval = READ_ONCE(dev->timer_interval_ms);
		if (copy_to_user((void __user *)arg, &interval,
				 sizeof(interval)))
			return -EFAULT;
		return 0;

	case MYCHARDEV_IOC_SET_TIMER_INTERVAL:
		if (copy_from_user(&interval, (void __user *)arg,
				   sizeof(interval)))
			return -EFAULT;
		if (interval < MIN_TIMER_INTERVAL_MS ||
		    interval > MAX_TIMER_INTERVAL_MS)
			return -EINVAL;

		WRITE_ONCE(dev->timer_interval_ms, interval);
		mod_timer(&dev->timer,
			  jiffies + msecs_to_jiffies(interval));
		return 0;

	default:
		return -ENOTTY;
	}
}

static int mychardev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mychardev *dev = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pfn;
	int ret;

	if (vma->vm_pgoff || size != PAGE_SIZE)
		return -EINVAL;
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	vma->vm_private_data = dev;
	vma->vm_ops = &mychardev_vm_ops;
	pfn = page_to_pfn(virt_to_page(dev->shared_page));
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

	ret = remap_pfn_range(vma, vma->vm_start, pfn, PAGE_SIZE,
			      vma->vm_page_prot);
	if (ret) {
		vma->vm_private_data = NULL;
		vma->vm_ops = NULL;
		module_put(THIS_MODULE);
		return ret;
	}

	atomic_inc(&dev->mmap_count);
	return 0;
}

static const struct file_operations mychardev_fops = {
	.owner = THIS_MODULE,
	.open = mychardev_open,
	.release = mychardev_release,
	.read = mychardev_read,
	.write = mychardev_write,
	.poll = mychardev_poll,
	.unlocked_ioctl = mychardev_ioctl,
	.mmap = mychardev_mmap,
};

static int mychardev_setup_device(struct mychardev *dev, unsigned int minor)
{
	int ret;

	dev->minor = minor;
	mutex_init(&dev->lock);
	init_waitqueue_head(&dev->read_queue);
	init_waitqueue_head(&dev->write_queue);
	atomic64_set(&dev->timer_ticks, 0);
	atomic_set(&dev->open_count, 0);
	atomic_set(&dev->mmap_count, 0);
	dev->timer_interval_ms = timer_interval_ms;
	timer_setup(&dev->timer, mychardev_timer, 0);
	cdev_init(&dev->cdev, &mychardev_fops);

	ret = kfifo_alloc(&dev->buffer, buffer_size, GFP_KERNEL);
	if (ret)
		return ret;

	dev->shared_page = (void *)get_zeroed_page(GFP_KERNEL);
	if (!dev->shared_page) {
		ret = -ENOMEM;
		goto free_fifo;
	}

	ret = cdev_add(&dev->cdev, device_number + minor, 1);
	if (ret)
		goto free_shared_page;
	dev->cdev_added = true;

	if (device_count == 1)
		dev->device = device_create(device_class, NULL,
					    device_number + minor, NULL,
					    DEVICE_NAME);
	else
		dev->device = device_create(device_class, NULL,
					    device_number + minor, NULL,
					    "%s%u", DEVICE_NAME, minor);
	if (IS_ERR(dev->device)) {
		ret = PTR_ERR(dev->device);
		dev->device = NULL;
		goto delete_cdev;
	}

	mod_timer(&dev->timer,
		  jiffies +
		  msecs_to_jiffies(dev->timer_interval_ms));
	return 0;

delete_cdev:
	cdev_del(&dev->cdev);
	dev->cdev_added = false;
free_shared_page:
	free_page((unsigned long)dev->shared_page);
	dev->shared_page = NULL;
free_fifo:
	kfifo_free(&dev->buffer);
	return ret;
}

static void mychardev_destroy_device(struct mychardev *dev)
{
	timer_shutdown_sync(&dev->timer);

	if (dev->device) {
		device_destroy(device_class, device_number + dev->minor);
		dev->device = NULL;
	}

	if (dev->cdev_added) {
		cdev_del(&dev->cdev);
		dev->cdev_added = false;
	}

	if (dev->shared_page) {
		free_page((unsigned long)dev->shared_page);
		dev->shared_page = NULL;
	}

	kfifo_free(&dev->buffer);
}

static int __init mychardev_init(void)
{
	unsigned int i;
	int ret;

	if (!device_count || device_count > MAX_DEVICE_COUNT)
		return -EINVAL;
	if (!buffer_size || buffer_size > MAX_BUFFER_SIZE)
		return -EINVAL;
	if (timer_interval_ms < MIN_TIMER_INTERVAL_MS ||
	    timer_interval_ms > MAX_TIMER_INTERVAL_MS)
		return -EINVAL;

	ret = alloc_chrdev_region(&device_number, 0, device_count, DEVICE_NAME);
	if (ret)
		return ret;

	devices = kcalloc(device_count, sizeof(*devices), GFP_KERNEL);
	if (!devices) {
		ret = -ENOMEM;
		goto unregister_region;
	}

	device_class = class_create(CLASS_NAME);
	if (IS_ERR(device_class)) {
		ret = PTR_ERR(device_class);
		device_class = NULL;
		goto free_devices;
	}

	for (i = 0; i < device_count; i++) {
		ret = mychardev_setup_device(&devices[i], i);
		if (ret)
			goto destroy_devices;
	}

	pr_info("%s: registered major=%u count=%u\n", DEVICE_NAME,
		MAJOR(device_number), device_count);
	return 0;

destroy_devices:
	while (i--)
		mychardev_destroy_device(&devices[i]);
	class_destroy(device_class);
	device_class = NULL;
free_devices:
	kfree(devices);
	devices = NULL;
unregister_region:
	unregister_chrdev_region(device_number, device_count);
	return ret;
}

static void __exit mychardev_exit(void)
{
	unsigned int i;

	for (i = 0; i < device_count; i++)
		mychardev_destroy_device(&devices[i]);

	class_destroy(device_class);
	kfree(devices);
	unregister_chrdev_region(device_number, device_count);

	pr_info("%s: unregistered\n", DEVICE_NAME);
}

module_init(mychardev_init);
module_exit(mychardev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mau");
MODULE_DESCRIPTION("Timer-driven in-memory Linux character device");
MODULE_VERSION("7.0");
