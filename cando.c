/*
 * Cando USB touchscreen
 *
 * Copyright (C) 2017 Gary Hetzel
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>         /* kmalloc() */
#include <linux/usb.h>          /* USB stuff */
#include <linux/mutex.h>        /* mutexes */
#include <linux/ioctl.h>

#define CANDO_USB_VENDOR_ID  0x2087
#define CANDO_USB_PRODUCT_ID 0x0a01

struct usb_cando {
    struct usb_device              *udev;
    struct usb_interface           *interface;
    unsigned char                  minor;
    char                           serial_number[8];
    int                            open_count;       /* Open count for this port */
    struct semaphore               sem;              /* Locks this structure */
    spinlock_t                     cmd_spinlock;     /* locks dev->command */
    char                           *int_in_buffer;
    struct usb_endpoint_descriptor *int_in_endpoint;
    struct urb                     *int_in_urb;
    int                            int_in_running;
    // char                           *ctrl_buffer;
    // struct urb                     *ctrl_urb;
    // struct usb_ctrlrequest         *ctrl_dr;
};

static DEFINE_MUTEX(disconnect_mutex);
static struct usb_driver cando_driver;

// -----------------------------------------------------------------------------
static inline void cando_debug_data(const char *function, int size,
        const unsigned char *data)
{
    int i;
    printk(KERN_INFO "[debug] %s: length = %d, data = \n", function, size);

    for (i = 0; i < size; ++i) {
        printk(KERN_INFO "%.2x \n", data[i]);
    }

    printk("\n");
}

static void cando_abort_transfers(struct usb_cando *dev)
{
    if (!dev) {
        printk(KERN_CRIT "dev is NULL\n");
        return;
    }

    if (!dev->udev) {
        printk(KERN_CRIT "udev is NULL\n");
        return;
    }

    if (dev->udev->state == USB_STATE_NOTATTACHED) {
        printk(KERN_CRIT "udev not attached\n");
        return;
    }

    /* Shutdown transfer */
    if (dev->int_in_running) {
        dev->int_in_running = 0;
        mb();

        if (dev->int_in_urb) {
            usb_kill_urb(dev->int_in_urb);
        }
    }
}

static inline void cando_delete(struct usb_cando *dev)
{
    cando_abort_transfers(dev);

    /* Free data structures. */
    if (dev->int_in_urb) {
        usb_free_urb(dev->int_in_urb);
    }

    kfree(dev->int_in_buffer);
    kfree(dev);
}

// -----------------------------------------------------------------------------
static struct usb_device_id cando_table [] = {
    { USB_DEVICE(CANDO_USB_VENDOR_ID, CANDO_USB_PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(usb,cando_table);

// -----------------------------------------------------------------------------
static void cando_int_in_callback(struct urb *urb)
{
    struct usb_cando *dev = urb->context;
    int retval;

    printk(KERN_INFO "cando: INT\n");

    cando_debug_data(__FUNCTION__, urb->actual_length, urb->transfer_buffer);

    if (urb->status) {
        if (
            urb->status == -ENOENT     ||
            urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN
        ) {
            return;
        } else {
            printk(KERN_CRIT "non-zero urb status (%d)", urb->status);
            goto resubmit; /* Maybe we can recover. */
        }
    }

    if (urb->actual_length > 0) {
        spin_lock(&dev->cmd_spinlock);

        // is this the data we want?
        // dev->int_in_buffer

        spin_unlock(&dev->cmd_spinlock);
    }

resubmit:
    /* Resubmit if we're still running. */
    if (dev->int_in_running && dev->udev) {
        retval = usb_submit_urb(dev->int_in_urb, GFP_ATOMIC);
        if (retval) {
            printk(KERN_CRIT "resubmitting urb failed (%d)", retval);
            dev->int_in_running = 0;
        }
    }
}

// static struct file_operations cando_fops = {
//     .owner   = THIS_MODULE,
//     .open    = cando_open,
//     .release = cando_release,
// };

// static struct usb_class_driver cando_class = {
//     .name       = "cando%d",
//     .fops       = &cando_fops,
//     .minor_base = CANDO_MINOR_BASE,
// };

// -----------------------------------------------------------------------------
static int cando_probe(struct usb_interface *interface,
    const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(interface);
    struct usb_cando *dev = NULL;
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    int i, int_end_size;
    int retval = -ENODEV;

    printk(KERN_INFO "Starting Cando probe vendor=%04x product=%04x\n",
        id->idVendor,
        id->idProduct);

    if (!udev) {
        printk(KERN_CRIT "udev is NULL\n");
        goto exit;
    }

    dev = kzalloc(sizeof(struct usb_cando), GFP_KERNEL);

    if (!dev) {
        printk(KERN_CRIT "cannot allocate memory for struct usb_cando\n");
        retval = -ENOMEM;
        goto exit;
    }

    sema_init(&dev->sem, 1);
    spin_lock_init(&dev->cmd_spinlock);

    dev->udev = udev;
    dev->interface = interface;
    iface_desc = interface->cur_altsetting;

    /* Set up interrupt endpoint information. */
    for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
        endpoint = &iface_desc->endpoint[i].desc;

        if (
            ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN) &&
            ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT)
        ) {
            dev->int_in_endpoint = endpoint;
        }

    }

    if (!dev->int_in_endpoint) {
        printk(KERN_CRIT "could not find interrupt in endpoint\n");
        goto error;
    }

    int_end_size = le16_to_cpu(dev->int_in_endpoint->wMaxPacketSize);

    dev->int_in_buffer = kmalloc(int_end_size, GFP_KERNEL);

    if (!dev->int_in_buffer) {
        printk(KERN_CRIT "could not allocate int_in_buffer\n");
        retval = -ENOMEM;
        goto error;
    }

    dev->int_in_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!dev->int_in_urb) {
        printk(KERN_CRIT "could not allocate int_in_urb\n");
        retval = -ENOMEM;
        goto error;
    }

    // Retrieve a serial.
    if (!usb_string(udev, udev->descriptor.iSerialNumber,
        dev->serial_number, sizeof(dev->serial_number))) {
        printk(KERN_CRIT "could not retrieve serial number\n");
        goto error;
    }

    // Save our data pointer in this interface device
    usb_set_intfdata(interface, dev);

    /* We can register the device now, as it is ready. */
    // retval = usb_register_dev(interface, &cando_class);
    printk(KERN_INFO "[%d] cando int setup: endpoint=%x\n",
        udev->descriptor.iSerialNumber,
        dev->int_in_endpoint->bEndpointAddress);

    // Initialize interrupt URB.
    usb_fill_int_urb(
        dev->int_in_urb,
        dev->udev,
        usb_rcvintpipe(
            dev->udev,
            dev->int_in_endpoint->bEndpointAddress
        ),
        dev->int_in_buffer,
        le16_to_cpu(dev->int_in_endpoint->wMaxPacketSize),
        cando_int_in_callback,
        dev,
        dev->int_in_endpoint->bInterval
    );

    printk(KERN_INFO "Cando Touchscreen driver loaded successfully\n");

exit:
    return retval;

error:
    cando_delete(dev);
    return retval;
}

// -----------------------------------------------------------------------------
static void cando_disconnect(struct usb_interface *interface)
{
    /* called when unplugging a USB device. */
}

static struct usb_driver cando_driver = {
    .name       = "cando",
    .id_table   = cando_table,
    .probe      = cando_probe,
    .disconnect = cando_disconnect,
};

// -----------------------------------------------------------------------------
static int __init usb_cando_init(void)
{
    int result;

    printk(KERN_INFO "Register Cando touchscreen driver\n");
    result = usb_register(&cando_driver);

    if (result) {
        printk(KERN_CRIT "registering Cando touchscreen driver failed\n");
    } else {
        printk(KERN_INFO "Cando touchscreen driver registered successfully\n");
    }

    return result;
}

// -----------------------------------------------------------------------------
static void __exit usb_cando_exit(void)
{
    usb_deregister(&cando_driver);
    printk(KERN_INFO "Cando touchscreen module deregistered\n");
}

module_init(usb_cando_init);
module_exit(usb_cando_exit);

MODULE_AUTHOR("Gary Hetzel <garyhetzel@gmail.com>");
MODULE_DESCRIPTION("Cando USB Touchscreen driver");
MODULE_LICENSE("GPL");
