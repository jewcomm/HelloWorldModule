#include <linux/module.h>     /* Needed by all modules */
#include <linux/kernel.h>  
#include <linux/fs.h>          /* Needed for KERN_INFO */
#include <linux/init.h>       /* Needed for the macros */
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/wait.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Navrotskij");
MODULE_DESCRIPTION("Simple Hello World module!");
MODULE_VERSION("0.1");

#define DEVICE_NAME "my_lkm"

#define BUFFER_SIZE 32

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);

static int reader_counter = 0;
static int writer_counter = 0;

static char *msg_ptr;

static struct mutex *pipeMutex;

static bool isReader = false;
static bool isWriter = false;

static wait_queue_head_t reader_queue;

int devNo;
struct class *pClass;

static int major_num;

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = device_read,
    .write = device_write,
    .open = device_open,
    .release = device_release    
};

static char *chr_devnode(struct device *dev, umode_t *mode){
    if(!mode) return NULL;
    if(dev->devt == MKDEV(major_num, 0) || dev->devt == MKDEV(major_num, 2)) *mode = 0666;
    return NULL;
}

static int device_open(struct inode *inode, struct file *file){
    // define operations
    int mode = (file->f_mode) & (FMODE_READ | FMODE_WRITE); 
    // set type operations
    if(mode == FMODE_READ) {
        if(isReader) {
            return -EBUSY;
        }
        //pr_cont("Is opened READ\n");
        isReader = true;
    } else if (mode == FMODE_WRITE) {
        if(isWriter) {
            return -EBUSY;
        }
        //pr_cont("Is opened WRITE\n");
        isWriter = true;
    }
    //pr_cont("mode: %i", mode);
    pr_cont("device open\n");
    try_module_get(THIS_MODULE);
    return 0;
}

static int device_release(struct inode *inode, struct file *file) {
    int mode = (file->f_mode) & (FMODE_READ | FMODE_WRITE); 
    //unset type operations
    if(mode == FMODE_READ) {
        pr_cont("Is closed READ\n");
        isReader = false;
    } else if (mode == FMODE_WRITE) {
        pr_cont("Is closed WRITE\n");
        isWriter = false;
    }
    module_put(THIS_MODULE);
    pr_cont("device reelase\n");
    return 0;
}

static ssize_t device_write(struct file *flip, const char __user *buffer, size_t len, loff_t *offset) {
    size_t notWritten; 

    mutex_lock(pipeMutex);
    pr_cont("write function\n");
    // hmm... Is it worth considering?
    if(len > BUFFER_SIZE) len = BUFFER_SIZE;

    if(writer_counter + len > BUFFER_SIZE) {
        size_t rem = writer_counter + len - BUFFER_SIZE;
        notWritten = copy_from_user(msg_ptr + writer_counter, buffer, len - rem);
        if(notWritten) { 
            pr_alert("ERROR! Fail copy_from_user.\n"); 
            mutex_unlock(pipeMutex);
            return -1;
        }
        
        // if can't write some part, 
        // it's correct idea?
        notWritten = copy_from_user(msg_ptr, buffer + rem, rem);
        if(notWritten) { 
            pr_alert("ERROR! Fail copy_from_user.\n"); 
            mutex_unlock(pipeMutex);
            return -1;
        }

        writer_counter = rem;

        mutex_unlock(pipeMutex);
        return len;
    } else {
        notWritten = copy_from_user(msg_ptr + writer_counter, buffer, len);
        pr_cont("Len: %li\n", len);
        pr_cont("Writed: %li\n", len - notWritten);
        
        if(notWritten) { 
            pr_alert("ERROR! Fail copy_from_user.\n"); 
            mutex_unlock(pipeMutex);
            return -1;
        }
        writer_counter += len - notWritten;
        mutex_unlock(pipeMutex);
        return len - notWritten;
    }
}

static ssize_t device_read(struct file *flip, char __user *buffer, size_t len, loff_t *offset) {
    pr_cont("read function: want read %li\n", len);
    mutex_lock(pipeMutex);
    int bytes_read;

    // temporary stub
    if(writer_counter < len) {
        mutex_unlock(pipeMutex);
        return -1;
    }
    
    bytes_read = copy_to_user(buffer, msg_ptr + reader_counter, len);
    pr_cont("buffer: %s\n", buffer);
    pr_cont("Readed: %li\n", len - bytes_read);

    // need add check if bytes_read != 0

    bytes_read = len - bytes_read;

    writer_counter -= bytes_read;
    reader_counter += bytes_read;
    mutex_unlock(pipeMutex);
    return bytes_read;
}

static int __init hello_start(void){
    struct device *pDev;

    // allocated memory for buffer
    msg_ptr = (char *)kmalloc(BUFFER_SIZE * sizeof(char), GFP_USER);

    if(msg_ptr == NULL) {
        pr_alert("ERROR! Fail allocated memory.\n");
        return -1;
    }

    major_num = register_chrdev(0, DEVICE_NAME, &fops);
    
    if(major_num < 0){
        pr_alert("Fail registration device: %d\n", major_num);
        kfree(msg_ptr);
        return major_num;
    }

    devNo = MKDEV(major_num, 0);

    pClass = class_create(THIS_MODULE, "x");
    if(IS_ERR(pClass)) {
        pr_alert("can't create class\n");
        unregister_chrdev_region(devNo, 1);
        kfree(msg_ptr);
        return -1;
    }
    pClass->devnode = chr_devnode;

    init_waitqueue_head(&reader_queue);

    if (IS_ERR(pDev = device_create(pClass, NULL, devNo, NULL, DEVICE_NAME))){
        pr_alert("my_lkm.ko cant create device /dev/my_lkm\n");
        class_destroy(pClass);
        unregister_chrdev_region(devNo, 1);
        kfree(msg_ptr);
        return -1;
    }

    pipeMutex = (struct mutex *)kzalloc(sizeof(struct mutex), GFP_KERNEL);
    mutex_init(pipeMutex);

    pr_cont("my_lkm module loaded with device major number %d\n", major_num);

    return 0;
}

static void __exit hello_end(void)
{
    kfree(msg_ptr);
    device_destroy(pClass, devNo);
    class_destroy(pClass);
    unregister_chrdev(major_num, DEVICE_NAME);
    pr_info("Goodbye Mr.\n");
}
  
module_init(hello_start);
module_exit(hello_end);