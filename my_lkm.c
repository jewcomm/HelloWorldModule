#include <linux/module.h>       /* Needed by all modules */
#include <linux/kernel.h>
#include <linux/fs.h>           /* Needed for KERN_INFO */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/slab.h>         /* Needed for allocators */
#include <linux/device.h>       /* Needed for class */
#include <linux/mutex.h>        /* Needed for mutex */
#include <linux/wait.h>         /* Needed for wait queue*/
#include <linux/list.h>         /* Needed for list */

#define DEBUG 0

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Navrotskij");
MODULE_DESCRIPTION("Simple Hello World module!");
MODULE_VERSION("0.1");

#define DEVICE_NAME "my_lkm"
#define BUFFER_SIZE 32
#define PIPE_SET_NEW_BUFFER_SIZE 0x30

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static struct mutex *my_pipe_mutex;

int dev_no;
struct class *class;

static int major_num;

struct my_pipe{
        char *pipe_buffer;

        int reader_counter;
        int writer_counter;
        int available_read;

        int condition; // -1 empty, 0 part filled, 1 - fill (ring)

        int buffer_size;

        bool mode_reader;
        bool mode_writer;

        struct mutex *rw_mutex;

        wait_queue_head_t reader_queue;

        uid_t user_id;

        struct list_head list;
};

struct my_pipe *pipes;

struct my_pipe *my_pipe_init(int buffer_size) {
        struct my_pipe *new_pipe;

        new_pipe = (struct my_pipe *)kmalloc(sizeof(struct my_pipe), GFP_USER);
        if (new_pipe == NULL) { 
                pr_alert("ERROR create pipe struct\n");
                return NULL;
        }

        new_pipe->buffer_size = buffer_size;

        new_pipe->pipe_buffer = (char *)kmalloc(buffer_size, GFP_USER);
        if (new_pipe->pipe_buffer == NULL) {
                pr_alert("ERROR create pipe struct\n");
                kfree(new_pipe);
                return NULL;
        }

        new_pipe->rw_mutex = (struct mutex *)kmalloc(sizeof(struct mutex), GFP_USER);
        if (new_pipe->rw_mutex == NULL) {
                pr_alert("ERROR create pipe struct\n");
                kfree(new_pipe);
                kfree(new_pipe->pipe_buffer);
                return NULL;
        }

        mutex_init(new_pipe->rw_mutex);

        new_pipe->mode_reader = false;
        new_pipe->mode_writer = false;

        new_pipe->reader_counter = 0;
        new_pipe->writer_counter = 0;
        new_pipe->available_read = 0;
        new_pipe->condition = -1;

        new_pipe->user_id = current_uid().val;
        INIT_LIST_HEAD(&new_pipe->list);
        init_waitqueue_head(&new_pipe->reader_queue);
        return new_pipe;
}

struct my_pipe *find_by_user_id(struct my_pipe *head, uid_t user_id)
{
        struct my_pipe *retval;

        if (&head->list && head->user_id == user_id){
                return head;
        }

        list_for_each_entry(retval, &head->list, list){
                if (retval->user_id == user_id){
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
        .release = device_release,
        .unlocked_ioctl = device_ioctl
};

static char *chr_devnode(struct device *dev, umode_t *mode) {
        if (!mode) return NULL;

        if (dev->devt == MKDEV(major_num, 0) || dev->devt == MKDEV(major_num, 2))
                *mode = 0666;
        
        return NULL;
}

static int device_open(struct inode *inode, struct file *file) {
        struct my_pipe *temp;
        int mode;

#if DEBUG
        pr_cont("Device Open\n");
#endif

        mutex_lock(my_pipe_mutex);

        if (!pipes) {
                pipes = my_pipe_init(BUFFER_SIZE);
                if (!pipes) {
                        mutex_unlock(my_pipe_mutex);
                        return -1;
                }
                temp = pipes;
        } else {
                struct my_pipe *t_pipe = find_by_user_id(pipes, current_uid().val);

                if (!t_pipe) {
                        if (!(t_pipe = my_pipe_init(BUFFER_SIZE))) {
                                mutex_unlock(my_pipe_mutex);
                                return -1;
                        }
                        list_add(&t_pipe->list, &pipes->list);
                }

                temp = t_pipe;
        }

        mutex_lock(temp->rw_mutex);
        // define operations
        mode = (file->f_mode) & (FMODE_READ | FMODE_WRITE);
        // set type operations
        if (mode == FMODE_READ) {
                if (temp->mode_reader) {
                        mutex_unlock(temp->rw_mutex);
                        mutex_unlock(my_pipe_mutex);
                        return -EBUSY;
                }
                temp->mode_reader = true;
        } else if (mode == FMODE_WRITE) {
                if (temp->mode_writer) {
                        mutex_unlock(temp->rw_mutex);
                        mutex_unlock(my_pipe_mutex);
                        return -EBUSY;
                }
                temp->mode_writer = true;
        }
#if DEBUG
        pr_cont("device open\n");
#endif
        try_module_get(THIS_MODULE);
        file->private_data = temp;
        mutex_unlock(my_pipe_mutex);
        mutex_unlock(temp->rw_mutex);
        return 0;
}

static int device_release(struct inode *inode, struct file *file) {
        struct my_pipe *temp = file->private_data;
        int mode = (file->f_mode) & (FMODE_READ | FMODE_WRITE);
        mutex_lock(temp->rw_mutex);
        mutex_lock(my_pipe_mutex);
        // unset type operations
        if (mode == FMODE_READ) {
#if DEBUG
                pr_cont("Is closed READ\n");
#endif
                temp->mode_reader = false;
        } else if (mode == FMODE_WRITE) {
#if DEBUG
                pr_cont("Is closed WRITE\n");
#endif
                temp->mode_writer = false;
        }
        module_put(THIS_MODULE);
#if DEBUG
        pr_cont("device reelase\n");
#endif
        mutex_unlock(temp->rw_mutex);
        mutex_unlock(my_pipe_mutex);
        return 0;
}

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
        struct my_pipe *temp = file->private_data;

        switch (cmd) {
                case PIPE_SET_NEW_BUFFER_SIZE:
#if DEBUG
                pr_cont("\n----ICOTL----\n");
                pr_cont("Received value: %li\n", arg);
#endif
                temp->buffer_size = arg;
                temp->pipe_buffer = krealloc(temp->pipe_buffer, arg, GFP_USER);
                if ((temp->pipe_buffer) == NULL) {
                        pr_alert("ERROR!!!! KREALLOC RETURNED NULL\n");
                }
                break;

        default:
                pr_alert("Error code 0x%x int ioctl\n", cmd);
                return -1;
        }

        return 0;
}

static ssize_t device_write(struct file *file, const char __user *buffer, size_t len, loff_t *offset) {
        struct my_pipe *temp = file->private_data;
        size_t not_written;
        size_t rem;

        mutex_lock(temp->rw_mutex);
#if DEBUG
        pr_cont("\n\n-----DEVICE WRITE BEGIN-----\n");
        pr_cont("Reader counter: %i\n", temp->reader_counter);
        pr_cont("Writer counter: %i\n", temp->writer_counter);
        pr_cont("Avaible: %i", temp->available_read);
        pr_cont("Buffer condition: %i\n", temp->condition);
#endif
        // hmm... Is it worth considering?
        if (len > (temp->buffer_size)) len = (temp->buffer_size);

        if ((temp->writer_counter) + len >= (temp->buffer_size)) {
                // ring writing
#if DEBUG
                pr_cont("RING WRITING\n");
#endif
                rem = (temp->writer_counter) + len - (temp->buffer_size);
                not_written = copy_from_user((temp->pipe_buffer) + temp->writer_counter, buffer, len - rem);
                if (not_written) {
                        pr_alert("ERROR! Fail copy_from_user.\n");
                        mutex_unlock(temp->rw_mutex);
                        return -1;
                }

                // if can't write some part,
                // it's correct idea?
                not_written = copy_from_user(temp->pipe_buffer, buffer + (len - rem), rem);
                if (not_written) {
                        pr_alert("ERROR! Fail copy_from_user.\n");
                        mutex_unlock(temp->rw_mutex);
                        return -1;
                }

                temp->writer_counter = rem;
                temp->available_read += rem;

                // rewrite already writer sumbols ;(
                if (temp->writer_counter >= temp->reader_counter) {
                        temp->reader_counter = temp->writer_counter;
                        temp->available_read = (temp->buffer_size);
                }
                temp->condition = 1;
        } else {
                not_written = copy_from_user((temp->pipe_buffer) + temp->writer_counter, buffer, len);
#if DEBUG
                pr_cont("LINE WRITING\n");
#endif
                if (not_written) {
                        pr_alert("ERROR! Fail copy_from_user.\n");
                        mutex_unlock(temp->rw_mutex);
                        return -1;
                }
                temp->writer_counter += len;
                temp->available_read += len;
                // rewrite already writer symbols ;(
                if (temp->writer_counter >= temp->reader_counter && temp->condition == 1) {
                        temp->reader_counter = temp->writer_counter;
                        temp->available_read = (temp->buffer_size);
                        temp->condition = 1;
                } else if (temp->available_read <= temp->reader_counter) {
                        temp->condition = 0;
                }
        }

        wake_up_interruptible(&(temp->reader_queue));
        mutex_unlock(temp->rw_mutex);
#if DEBUG
        pr_cont("Reader counter: %i\n", temp->reader_counter);
        pr_cont("Writer counter: %i\n", temp->writer_counter);
        pr_cont("Avaible: %i", temp->available_read);
        pr_cont("Buffer condition: %i\n", temp->condition);
        pr_cont("-----DEVICE WRITE END-----\n");
#endif
        return len;
}

static ssize_t device_read(struct file *file, char __user *buffer, size_t len, loff_t *offset)
{
        int not_readed;

        struct my_pipe *temp = file->private_data;

#if DEBUG
        pr_cont("\n\n-----DEVICE READ BEGIN-----\n");
#endif
        while (true) {
                mutex_lock(temp->rw_mutex);

#if DEBUG
                pr_cont("Reader counter: %i\n", temp->reader_counter);
                pr_cont("Writer counter: %i\n", temp->writer_counter);
                pr_cont("Avaible: %i\n", temp->available_read);
                pr_cont("Buffer condition: %i\n", temp->condition);
#endif
                if ((temp->reader_counter > temp->writer_counter) || (temp->reader_counter == temp->writer_counter && temp->condition == 1)) {
                        // if buffer overflowed and works as ring buffer
                        temp->available_read = temp->writer_counter + (temp->buffer_size) - temp->reader_counter;
#if DEBUG
                        pr_cont("RING READING\n: %i\n", temp->available_read);
#endif
                        if (temp->available_read >= len) {
                                size_t rem = temp->reader_counter + len;
                                if (rem > (temp->buffer_size)) {
                                        // if read from zero
                                        rem -= (temp->buffer_size);
#if DEBUG
                                        pr_cont("Rem: %li\n", rem);
#endif
                                        not_readed = copy_to_user(buffer, (temp->pipe_buffer) + temp->reader_counter, len - rem);
                                        if (not_readed) {
                                                pr_alert("ERROR! Fail copy_to_user.\n");
                                                mutex_unlock(temp->rw_mutex);
                                                return -1;
                                        }

                                        not_readed = copy_to_user(buffer + (len - rem), temp->pipe_buffer, rem);
#if DEBUG
                                        pr_cont("Buffer: %s\n", temp->pipe_buffer);
#endif
                                        if (not_readed) {
                                                pr_alert("ERROR! Fail copy_to_user.\n");
                                                mutex_unlock(temp->rw_mutex);
                                                return -1;
                                        }

                                        temp->reader_counter = rem;
                                        temp->condition = 0;
                                } else {
                                        not_readed = copy_to_user(buffer, (temp->rw_mutex) + temp->reader_counter, len);
#if DEBUG
                                        pr_cont("buffer: %s\n", buffer);
                                        pr_cont("Readed: %li\n", len - not_readed);
#endif

                                        if (not_readed) {
                                                pr_alert("ERROR! Fail copy_to_user.\n");
                                                mutex_unlock(temp->rw_mutex);
                                                return -1;
                                        }

                                        temp->reader_counter += len;
                                        temp->condition = 1;
                                }
                                temp->available_read -= len;
#if DEBUG
                                pr_cont("Reader counter: %i\n", temp->reader_counter);
                                pr_cont("Writer counter: %i\n", temp->writer_counter);
                                pr_cont("Avaible: %i", temp->available_read);
                                pr_cont("Buffer condition: %i\n", temp->condition);
                                pr_cont("-----DEVICE READ END-----\n");
#endif
                                mutex_unlock(temp->rw_mutex);
                                return len;
                        } 
                } else if (temp->reader_counter < temp->writer_counter) {
                        temp->available_read = temp->writer_counter - temp->reader_counter;

#if DEBUG
                        pr_alert("[LINE]Available read: %i\n", temp->available_read);
#endif
                        // adding wait_queue
                        if (temp->available_read >= len) {
                                not_readed = copy_to_user(buffer, temp->pipe_buffer + (temp->reader_counter), len);
#if DEBUG
                                pr_cont("buffer: %s\n", buffer);
                                pr_cont("Readed: %li\n", len - not_readed);
#endif

                                if (not_readed) {
                                        pr_alert("ERROR! Fail copy_to_user.\n");
                                        mutex_unlock(temp->rw_mutex);
                                        return -1;
                                }

                                temp->reader_counter += len;
                                temp->available_read -= len;

                                mutex_unlock(temp->rw_mutex);
                                temp->condition = 0;

#if DEBUG
                                pr_cont("Reader counter: %i\n", temp->reader_counter);
                                pr_cont("Writer counter: %i\n", temp->writer_counter);
                                pr_cont("Avaible: %i", temp->available_read);
                                pr_cont("Buffer condition: %i\n", temp->condition);
                                pr_cont("-----DEVICE READ END-----\n");
#endif

                                return len;
                        }
                }
                mutex_unlock(temp->rw_mutex);
                wait_event_interruptible(temp->reader_queue, temp->available_read >= len);
                
#if DEBUG
                pr_cont("Reader counter: %i\n", temp->reader_counter);
                pr_cont("Writer counter: %i\n", temp->writer_counter);
                pr_cont("Avaible: %i", temp->available_read);
                pr_cont("Buffer condition: %i\n", temp->condition);
                pr_cont("-----DEVICE READ SLEEP-----\n");
                pr_cont("-----DEVICE READ WAKEUP-----\n");
#endif
        }
}

static int __init hello_start(void) {
        struct device *dev;

        major_num = register_chrdev(0, DEVICE_NAME, &fops);

        if (major_num < 0) {
                pr_alert("Fail registration device: %d\n", major_num);
                return major_num;
        }

        dev_no = MKDEV(major_num, 0);

        /* Needed for autocreating device */
        class = class_create(THIS_MODULE, "x");
        if (IS_ERR(class)) {
                pr_alert("can't create class\n");
                unregister_chrdev_region(dev_no, 1);
                return -1;
        }
        class->devnode = chr_devnode;

        if (IS_ERR(dev = device_create(class, NULL, dev_no, NULL, DEVICE_NAME))) {
                pr_alert("my_lkm.ko cant create device /dev/my_lkm\n");
                class_destroy(class);
                unregister_chrdev_region(dev_no, 1);
                return -1;
        }

        my_pipe_mutex = (struct mutex *)kzalloc(sizeof(struct mutex), GFP_KERNEL);
        mutex_init(my_pipe_mutex);

        pr_cont("Module %s loaded with device major number %d\n", DEVICE_NAME, major_num);

        return 0;
}

static void __exit hello_end(void) {
        device_destroy(class, dev_no);
        class_destroy(class);
        unregister_chrdev(major_num, DEVICE_NAME);
        pr_cont("Module %s, unload\n", DEVICE_NAME);
}

module_init(hello_start);
module_exit(hello_end);