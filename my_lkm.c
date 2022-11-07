#include <linux/module.h>     /* Needed by all modules */
#include <linux/kernel.h>  
#include <linux/fs.h>          /* Needed for KERN_INFO */
#include <linux/init.h>       /* Needed for the macros */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Navrotskij");
MODULE_DESCRIPTION("Simple Hello World module!");
MODULE_VERSION("0.1");

#define DEVICE_NAME "my_lkm"

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);

static int major_num;
static int device_open_count = 0;

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = device_read,
    .write = device_write,
    //.open = device_open,
    .release = device_release    
};

static int device_open(struct inode *inode, struct file *file){
    if(device_open_count) {
        return -EBUSY;
    }
    device_open_count++;
    printk(KERN_INFO "device open");
    try_module_get(THIS_MODULE);
    return 0;
}

static int device_release(struct inode *inode, struct file *file) {
    device_open_count--;
    module_put(THIS_MODULE);
    return 0;
}

static ssize_t device_write(struct file *flip, const char *buffer, size_t len, loff_t *offset) {
    printk(KERN_ALERT "write function\n");
    return 0;
}

static ssize_t device_read(struct file *flip, char *buffer, size_t len, loff_t *offset) {
    printk(KERN_INFO "read function\n");
    return 0;
}

static int __init hello_start(void){
    printk(KERN_INFO "Loading hello module...\n");
    major_num = register_chrdev(0, DEVICE_NAME, &fops);

    if(major_num < 0){
        printk(KERN_ALERT "Fail registration device: %d\n", major_num);
        return major_num;
    } else {
        printk(KERN_INFO "my_lkm module loaded with device major number %d\n", major_num);
        return 0;
    }
}

static void __exit hello_end(void)
{
    unregister_chrdev(major_num, DEVICE_NAME);
    printk(KERN_INFO "Goodbye Mr.\n");
}
  
module_init(hello_start);
module_exit(hello_end);