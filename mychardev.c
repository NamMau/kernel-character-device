#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "mychardev"
#define BUFFER_SIZE 1024

static int major_number;
static char device_buffer[BUFFER_SIZE];
static size_t buffer_len;

static int dev_open(struct inode *inodep, struct file *filep)
{
	pr_info("mychardev: opened\n");
	return 0;
}

static ssize_t dev_read(struct file *filep, char __user *buffer,
			size_t len, loff_t *offset)
{
	size_t bytes_to_read;

	if (*offset >= buffer_len)
		return 0;

	bytes_to_read = min(len, buffer_len - (size_t)*offset);

	if (copy_to_user(buffer, device_buffer + *offset, bytes_to_read))
		return -EFAULT;

	*offset += bytes_to_read;

	pr_info("mychardev: read %zu bytes\n", bytes_to_read);

	return bytes_to_read;
}

static ssize_t dev_write(struct file *filep, const char __user *buffer,
			 size_t len, loff_t *offset)
{
	size_t bytes_to_write;

	bytes_to_write = min(len, (size_t)(BUFFER_SIZE - 1));

	if (copy_from_user(device_buffer, buffer, bytes_to_write))
		return -EFAULT;

	device_buffer[bytes_to_write] = '\0';
	buffer_len = bytes_to_write;

	pr_info("mychardev: wrote %zu bytes\n", bytes_to_write);

	return bytes_to_write;
}

static int dev_release(struct inode *inodep, struct file *filep)
{
	pr_info("mychardev: released\n");
	return 0;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = dev_open,
	.read = dev_read,
	.write = dev_write,
	.release = dev_release,
};

static int __init char_init(void)
{
	major_number = register_chrdev(0, DEVICE_NAME, &fops);

	if (major_number < 0) {
		pr_err("mychardev: failed to register device\n");
		return major_number;
	}

	pr_info("mychardev: registered with major number %d\n", major_number);
	pr_info("mychardev: create device with: sudo mknod /dev/%s c %d 0\n",
		DEVICE_NAME, major_number);

	return 0;
}

static void __exit char_exit(void)
{
	unregister_chrdev(major_number, DEVICE_NAME);
	pr_info("mychardev: unregistered\n");
}

module_init(char_init);
module_exit(char_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mau");
MODULE_DESCRIPTION("Simple Linux Character Driver");
MODULE_VERSION("1.0");
