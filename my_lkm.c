#include <linux/module.h>     /* Needed by all modules */
#include <linux/kernel.h>  
#include <linux/fs.h>          /* Needed for KERN_INFO */
#include <linux/init.h>       /* Needed for the macros */
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/list.h>


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

static struct mutex *myPipeMutex;

int devNo;
struct class *pClass;

static int major_num;

struct myPipe
{
    char *msgPtr;

    int readerCounter;
    int writerCounter;
    int availableRead;

    int buffCond; // -1 empty, 0 part filled, 1 - fill (ring)

    bool isReader;
    bool isWriter;

    struct mutex* rwMutex;

    wait_queue_head_t reader_queue;

    uid_t userId;

    struct list_head list;    
};

struct myPipe* pipes;

struct myPipe* myPipeInit(void){
    struct myPipe* newPipe;

    newPipe = (struct myPipe*)kmalloc(sizeof(struct myPipe), GFP_USER);
    if(newPipe == NULL) return NULL;

    newPipe->msgPtr = (char*)kmalloc(BUFFER_SIZE, GFP_USER);
    if(newPipe->msgPtr == NULL) { 
        kfree(newPipe);
        return NULL; 
    }

    newPipe->rwMutex = (struct mutex*)kmalloc(sizeof(struct mutex), GFP_KERNEL);
    if(newPipe->rwMutex == NULL){
        kfree(newPipe);
        kfree(newPipe->msgPtr);
        return NULL;
    }

    mutex_init(newPipe->rwMutex);

    newPipe->isReader = false;
    newPipe->isWriter = false;

    newPipe->readerCounter = 0;
    newPipe->writerCounter = 0;
    newPipe->availableRead = 0;
    newPipe->buffCond = -1;

    newPipe->userId = current_uid().val;
    INIT_LIST_HEAD(&newPipe->list);
    init_waitqueue_head(&newPipe->reader_queue);
    return newPipe;
}

struct myPipe* findByUserId(struct myPipe* head, uid_t userId) {
    struct myPipe* retval;

    if(&head->list && head->userId == userId) {
        return head;
    }

    // macros start 
    list_for_each_entry(retval, &head->list, list) {
        if(retval->userId == userId) {
            return retval;
        }
    }

    return NULL;
}

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
    struct myPipe* temp;
    int mode;

    mutex_lock(myPipeMutex);

    if(!pipes){
        pipes = myPipeInit();
        if(!pipes) {
            mutex_unlock(myPipeMutex);
            return -1;
        }
        temp = pipes;
    } else {
        struct myPipe* t_pipe = findByUserId(pipes, current_uid().val);

        if(!t_pipe){
            if(!(t_pipe = myPipeInit())){
                mutex_unlock(myPipeMutex);
                return -1;
            }
            list_add(&t_pipe->list, &pipes->list);
        }

        temp = t_pipe;
    }

    mutex_lock(temp->rwMutex);
    // define operations
    mode = (file->f_mode) & (FMODE_READ | FMODE_WRITE); 
    // set type operations
    if(mode == FMODE_READ) {
        if(temp->isReader) {
            mutex_unlock(temp->rwMutex);
            mutex_unlock(myPipeMutex);
            return -EBUSY;
        }
        //pr_cont("Is opened READ\n");
        temp->isReader = true;
    } else if (mode == FMODE_WRITE) {
        if(temp->isWriter) {
            mutex_unlock(temp->rwMutex);
            mutex_unlock(myPipeMutex);
            return -EBUSY;
        }
        //pr_cont("Is opened WRITE\n");
        temp->isWriter = true;
    }
    //pr_cont("mode: %i", mode);
    pr_cont("device open\n");
    try_module_get(THIS_MODULE);
    file->private_data = temp;
    mutex_unlock(myPipeMutex);
    mutex_unlock(temp->rwMutex);
    return 0;
}

static int device_release(struct inode *inode, struct file *file) {
    struct myPipe* temp = file->private_data;
    int mode = (file->f_mode) & (FMODE_READ | FMODE_WRITE); 
    mutex_lock(temp->rwMutex);
    mutex_lock(myPipeMutex);
    //unset type operations
    if(mode == FMODE_READ) {
        pr_cont("Is closed READ\n");
        temp->isReader = false;
    } else if (mode == FMODE_WRITE) {
        pr_cont("Is closed WRITE\n");
        temp->isWriter = false;
    }
    module_put(THIS_MODULE);
    pr_cont("device reelase\n");
    mutex_unlock(temp->rwMutex);
    mutex_unlock(myPipeMutex);
    return 0;
}

static ssize_t device_write(struct file *file, const char __user *buffer, size_t len, loff_t *offset) {
    struct myPipe* temp = file->private_data;   
    size_t notWritten; 
    size_t rem;

    mutex_lock(temp->rwMutex); 

    pr_cont("\n\n-----DEVICE WRITE BEGIN-----\n");
    pr_cont("Reader counter: %i\n", temp->readerCounter);
    pr_cont("Writer counter: %i\n", temp->writerCounter);
    pr_cont("Avaible: %i", temp->availableRead);
    pr_cont("Buffer condition: %i\n", temp->buffCond);

    // hmm... Is it worth considering?
    if(len > BUFFER_SIZE) len = BUFFER_SIZE;

    if((temp->writerCounter) + len >= BUFFER_SIZE) {
        // ring writing
        pr_cont("RING WRITING\n");
        rem = (temp->writerCounter) + len - BUFFER_SIZE;
        notWritten = copy_from_user((temp->msgPtr) + temp->writerCounter, buffer, len - rem);
        if(notWritten) { 
            pr_alert("ERROR! Fail copy_from_user.\n"); 
            mutex_unlock(temp->rwMutex);
            return -1;
        }
        
        // if can't write some part, 
        // it's correct idea?
        notWritten = copy_from_user(temp->msgPtr, buffer + (len - rem), rem);
        if(notWritten) { 
            pr_alert("ERROR! Fail copy_from_user.\n"); 
            mutex_unlock(temp->rwMutex);
            return -1;
        }

        temp->writerCounter = rem;
        temp->availableRead += rem;

        // rewrite already writer sumbols ;(
        if(temp->writerCounter >= temp->readerCounter) { 
            temp->readerCounter = temp->writerCounter; 
            temp->availableRead = BUFFER_SIZE;
        }
        temp->buffCond = 1;
    } else {
        notWritten = copy_from_user((temp->msgPtr) + temp->writerCounter, buffer, len);
        pr_cont("LINE WRITING\n");
        if(notWritten) { 
            pr_alert("ERROR! Fail copy_from_user.\n"); 
            mutex_unlock(temp->rwMutex);
            return -1;
        }
        temp->writerCounter += len;
        temp->availableRead += len;
        // rewrite already writer sumbols ;(
        if(temp->writerCounter >= temp->readerCounter && temp->buffCond == 1) { 
            temp->readerCounter = temp->writerCounter; 
            temp->availableRead = BUFFER_SIZE;
            temp->buffCond = 1;
        } else if(temp->availableRead <= temp->readerCounter) {
            temp->buffCond = 0;
        }
    }

    wake_up_interruptible(&(temp->reader_queue));
    mutex_unlock(temp->rwMutex);
    pr_cont("Reader counter: %i\n", temp->readerCounter);
    pr_cont("Writer counter: %i\n", temp->writerCounter);
    pr_cont("Avaible: %i", temp->availableRead);
    pr_cont("Buffer condition: %i\n", temp->buffCond);
    pr_cont("-----DEVICE WRITE END-----\n");
    return len;
}

static ssize_t device_read(struct file *file, char __user *buffer, size_t len, loff_t *offset) {
    int notReaded;

    struct myPipe* temp = file->private_data;

    pr_cont("\n\n-----DEVICE READ BEGIN-----\n");   
    while (true) {
        mutex_lock(temp->rwMutex);

        pr_cont("Reader counter: %i\n", temp->readerCounter);
        pr_cont("Writer counter: %i\n", temp->writerCounter);
        pr_cont("Avaible: %i\n", temp->availableRead);
        pr_cont("Buffer condition: %i\n", temp->buffCond);

        if((temp->readerCounter > temp->writerCounter) || (temp->readerCounter == temp->writerCounter && temp->buffCond == 1)) {
            // if buffer overflowed and works as ring buffer
            temp->availableRead = temp->writerCounter + BUFFER_SIZE - temp->readerCounter;
            
            pr_alert("RING READING\n: %i\n", temp->availableRead);

            if(temp->availableRead >= len) {
                size_t rem = temp->readerCounter + len;
                if(rem > BUFFER_SIZE) {
                    // if read from zero
                    rem -= BUFFER_SIZE;
                    pr_alert("Rem: %li\n", rem);
                    

                    notReaded = copy_to_user(buffer, (temp->msgPtr) + temp->readerCounter, len - rem);
                    if(notReaded) {
                        pr_alert("ERROR! Fail copy_to_user.\n"); 
                        mutex_unlock(temp->rwMutex);
                        return -1;
                    }

                    notReaded = copy_to_user(buffer + (len - rem), temp->msgPtr, rem);
                    pr_alert("Buffer: %s\n", temp->msgPtr);
                    if(notReaded) {
                        pr_alert("ERROR! Fail copy_to_user.\n"); 
                        mutex_unlock(temp->rwMutex);
                        return -1;
                    }

                    temp->readerCounter = rem;
                    temp->buffCond = 0;
                } else {
                    notReaded = copy_to_user(buffer, (temp->rwMutex) + temp->readerCounter, len);
                    pr_cont("buffer: %s\n", buffer);
                    pr_cont("Readed: %li\n", len - notReaded);

                    if(notReaded) {
                        pr_alert("ERROR! Fail copy_to_user.\n"); 
                        mutex_unlock(temp->rwMutex);
                        return -1;
                    }

                    temp->readerCounter += len;
                    temp->buffCond = 1;
                }
                temp->availableRead -= len;
                pr_cont("Reader counter: %i\n", temp->readerCounter);
                pr_cont("Writer counter: %i\n", temp->writerCounter);
                pr_cont("Avaible: %i", temp->availableRead);
                pr_cont("Buffer condition: %i\n", temp->buffCond);
                pr_cont("-----DEVICE READ END-----\n");
                mutex_unlock(temp->rwMutex);
                return len;
            }
        } else if (temp->readerCounter < temp->writerCounter) {
            temp->availableRead = temp->writerCounter - temp->readerCounter;
            pr_alert("[LINE]Available read: %i\n", temp->availableRead);

            // temporary stub
            // adding wait_queue
            if(temp->availableRead >= len) {
                notReaded = copy_to_user(buffer, temp->msgPtr + (temp->readerCounter), len);
                pr_cont("buffer: %s\n", buffer);
                pr_cont("Readed: %li\n", len - notReaded);

                if(notReaded) {
                    pr_alert("ERROR! Fail copy_to_user.\n"); 
                    mutex_unlock(temp->rwMutex);
                    return -1;
                }

                temp->readerCounter += len;
                temp->availableRead -= len;

                mutex_unlock(temp->rwMutex);
                temp->buffCond = 0;
                pr_cont("Reader counter: %i\n", temp->readerCounter);
                pr_cont("Writer counter: %i\n", temp->writerCounter);
                pr_cont("Avaible: %i", temp->availableRead);
                pr_cont("Buffer condition: %i\n", temp->buffCond);
                pr_cont("-----DEVICE READ END-----\n");
                return len;
            }
        } 
        mutex_unlock(temp->rwMutex);    
        pr_cont("Reader counter: %i\n", temp->readerCounter);
        pr_cont("Writer counter: %i\n", temp->writerCounter);
        pr_cont("Avaible: %i", temp->availableRead);
        pr_cont("Buffer condition: %i\n", temp->buffCond);
        pr_cont("-----DEVICE READ SLEEP-----\n");
        wait_event_interruptible(temp->reader_queue, temp->availableRead >= len);
        pr_cont("-----DEVICE READ WAKEUP-----\n");
    }
}

static int __init hello_start(void){
    struct device *pDev;

    major_num = register_chrdev(0, DEVICE_NAME, &fops);
    
    if(major_num < 0){
        pr_alert("Fail registration device: %d\n", major_num);
        return major_num;
    }

    devNo = MKDEV(major_num, 0);

    pClass = class_create(THIS_MODULE, "x");
    if(IS_ERR(pClass)) {
        pr_alert("can't create class\n");
        unregister_chrdev_region(devNo, 1);
        return -1;
    }
    pClass->devnode = chr_devnode;

    if (IS_ERR(pDev = device_create(pClass, NULL, devNo, NULL, DEVICE_NAME))){
        pr_alert("my_lkm.ko cant create device /dev/my_lkm\n");
        class_destroy(pClass);
        unregister_chrdev_region(devNo, 1);
        return -1;
    }

    myPipeMutex = (struct mutex *)kzalloc(sizeof(struct mutex), GFP_KERNEL);
    mutex_init(myPipeMutex);

    pr_cont("my_lkm module loaded with device major number %d\n", major_num);

    return 0;
}

static void __exit hello_end(void)
{
    device_destroy(pClass, devNo);
    class_destroy(pClass);
    unregister_chrdev(major_num, DEVICE_NAME);
    pr_info("Goodbye Mr.\n");
}

module_init(hello_start);
module_exit(hello_end);