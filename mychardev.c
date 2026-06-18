// SPDX-License-Identifier: GPL-2.0

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "mychardev"
#define CLASS_NAME "mychardev"
#define BUFFER_SIZE 1024

struct mychardev {
	struct cdev cdev;
	struct mutex lock;
	DECLARE_KFIFO(buffer, unsigned char, BUFFER_SIZE);
};

static dev_t device_number;
static struct class *device_class;
static struct device *device;
static struct mychardev my_device;

static int mychardev_open(struct inode *inode, struct file *file)
{
	struct mychardev *dev;

	dev = container_of(inode->i_cdev, struct mychardev, cdev);
	file->private_data = dev;

	return nonseekable_open(inode, file);
}

static ssize_t mychardev_read(struct file *file, char __user *buffer,
			      size_t count, loff_t *offset)
{
	struct mychardev *dev = file->private_data;
	unsigned int copied;
	int ret;

	if (!count)
		return 0;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	ret = kfifo_to_user(&dev->buffer, buffer, count, &copied);

	mutex_unlock(&dev->lock);
	return ret ? ret : copied;
}

static ssize_t mychardev_write(struct file *file,
			       const char __user *buffer, size_t count,
			       loff_t *offset)
{
	struct mychardev *dev = file->private_data;
	unsigned int copied;
	int ret;

	if (!count)
		return 0;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	if (kfifo_is_full(&dev->buffer)) {
		ret = -ENOSPC;
		goto unlock;
	}

	ret = kfifo_from_user(&dev->buffer, buffer, count, &copied);

unlock:
	mutex_unlock(&dev->lock);
	return ret ? ret : copied;
}

static const struct file_operations mychardev_fops = {
	.owner = THIS_MODULE,
	.open = mychardev_open,
	.read = mychardev_read,
	.write = mychardev_write,
};

static int __init mychardev_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME);
	if (ret)
		return ret;

	mutex_init(&my_device.lock);
	INIT_KFIFO(my_device.buffer);
	cdev_init(&my_device.cdev, &mychardev_fops);

	ret = cdev_add(&my_device.cdev, device_number, 1);
	if (ret)
		goto unregister_region;

	device_class = class_create(CLASS_NAME);
	if (IS_ERR(device_class)) {
		ret = PTR_ERR(device_class);
		goto delete_cdev;
	}

	device = device_create(device_class, NULL, device_number, NULL,
			       DEVICE_NAME);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto destroy_class;
	}

	pr_info("%s: registered major=%u minor=%u\n", DEVICE_NAME,
		MAJOR(device_number), MINOR(device_number));
	return 0;

destroy_class:
	class_destroy(device_class);
delete_cdev:
	cdev_del(&my_device.cdev);
unregister_region:
	unregister_chrdev_region(device_number, 1);
	return ret;
}

static void __exit mychardev_exit(void)
{
	device_destroy(device_class, device_number);
	class_destroy(device_class);
	cdev_del(&my_device.cdev);
	unregister_chrdev_region(device_number, 1);

	pr_info("%s: unregistered\n", DEVICE_NAME);
}

module_init(mychardev_init);
module_exit(mychardev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mau");
MODULE_DESCRIPTION("Synchronized in-memory Linux character device");
MODULE_VERSION("3.0");
