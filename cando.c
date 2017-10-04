/*
 * Cando USB touchscreen
 *
 * Copyright (C) 2017 Gary Hetzel
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/input/mt.h>

MODULE_AUTHOR("Gary Hetzel <garyhetzel@gmail.com>");
MODULE_DESCRIPTION("Cando USB Touchscreen driver");
MODULE_LICENSE("GPL");

#define CANDO_USB_VENDOR_ID  0x2087
#define CANDO_USB_PRODUCT_ID 0x0a01
#define CANDO_ABS_X_MAX      4095
#define CANDO_ABS_Y_MAX      4095


struct coords {
    int x;
    int y;
    int state;
};

struct cando_device {
    struct hid_device              *hdev;
    const struct hid_device_id     *id;
    struct input_dev               *input;
    struct coords                  *last1;
    struct coords                  *last2;
};

// -----------------------------------------------------------------------------
static void cando_send_touch_event(struct input_dev *input, int contact_num,
    int x, int y, int state, struct coords *last)
{
    if (state) {
        // send multitouch events
        input_event(input, EV_ABS, ABS_MT_SLOT, contact_num);
        input_report_abs(input, ABS_MT_TRACKING_ID, contact_num);
        input_report_abs(input, ABS_MT_POSITION_X, x);
        input_report_abs(input, ABS_MT_POSITION_Y, y);

        // send touch event
        input_report_key(input, BTN_TOUCH, 1);

        // send traditional single-touch (ST) events
        input_report_abs(input, ABS_X, x);
        input_report_abs(input, ABS_Y, y);

        input_event(input, EV_SYN, SYN_REPORT, 0);
        input_sync(input);

        last->x = x;
        last->y = y;
        last->state = 1;
    } else {
        input_event(input, EV_ABS, ABS_MT_SLOT, contact_num);
        input_report_abs(input, ABS_MT_TRACKING_ID, -1);
        input_report_key(input, BTN_TOUCH, 0);
        input_event(input, EV_SYN, SYN_REPORT, 0);
        input_sync(input);

        last->state = 0;
    }
}

// -----------------------------------------------------------------------------
static void cando_report(struct hid_device *hdev, struct hid_report *report)
{
    struct cando_device *cdev = hid_get_drvdata(hdev);
    int i;
    unsigned count;
    struct hid_field *field;
    int32_t value;
    int32_t active1, active2, x1, y1, x2, y2, active_contacts;

    if(!cdev || !cdev->input) {
        return;
    }

    // iterate through all fields in this report
    for (i = 0; i < report->maxfield; i++) {
        field = report->field[i];
        count = field->report_count;

        if (!(HID_MAIN_ITEM_VARIABLE & field->flags))
            continue;

        value = (int32_t)(*field->value);

        switch(i) {
        case 0: // Contact 1 touch detect
            if (value) {
                active1 = 1;
            } else {
                active1 = 0;
            }
            break;

        case 3: // Contact 1 X-coordinate
            x1 = value;
            break;

        case 4: // Contact 1 Y-coordinate
            y1 = value;
            break;

        case 5: // Contact 2 touch detect
            if (value) {
                active2 = 1;
            } else {
                active2 = 0;
            }
            break;

        case 8: // Contact 2 X-coordinate
            x2 = value;
            break;

        case 9: // Contact 2 Y-coordinate
            y2 = value;
            break;

        case 10: // Total active contact count
            active_contacts = value;
            break;
        }
    }

    cando_send_touch_event(cdev->input, 0, x1, y1, active1, cdev->last1);
    cando_send_touch_event(cdev->input, 1, x2, y2, active2, cdev->last2);
}

// -----------------------------------------------------------------------------
static int cando_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    struct cando_device *cdev = NULL;
    struct input_dev *input;
    int ret = -ENODEV;

    printk(KERN_INFO "Starting Cando probe bus=%02x group=%02x vendor=%04x product=%04x\n",
        id->bus,
        id->group,
        id->vendor,
        id->product
    );

    // allocate devices for input subsystem
    // ----------------------------------------
    input = input_allocate_device();

    if (!input) {
        ret = -ENOMEM;
        printk(KERN_CRIT "Failed to allocate input: %d\n", ret);
        return ret;
    }

    input->name        = "Cando Multitouch Driver";
    input->id.bustype  = BUS_USB;
    input->dev.parent  = &hdev->dev;

    input_set_capability(input, EV_KEY, BTN_TOUCH);
    input_set_capability(input, EV_KEY, BTN_TOUCH);

    input_set_abs_params(input, ABS_X, 0, CANDO_ABS_X_MAX, 0, 0);
    input_set_abs_params(input, ABS_Y, 0, CANDO_ABS_Y_MAX, 0, 0);
    input_mt_init_slots(input, 2, 0);
    input_set_abs_params(input, ABS_MT_SLOT, 0, 1, 0, 0);
    input_set_abs_params(input, ABS_MT_TRACKING_ID, 0, 65535, 0, 0);
    input_set_abs_params(input, ABS_MT_POSITION_X, 0, CANDO_ABS_X_MAX, 0, 0);
    input_set_abs_params(input, ABS_MT_POSITION_Y, 0, CANDO_ABS_Y_MAX, 0, 0);

    // allocate driver struct
    // ----------------------------------------
    cdev = devm_kzalloc(&hdev->dev, sizeof(struct cando_device), GFP_KERNEL);

    if (!cdev) {
        dev_err(&hdev->dev, "cannot allocate memory for struct cando_device\n");
        return -ENOMEM;
    }

    // populate driver-specific struct
    // ----------------------------------------
    cdev->hdev  = hdev;
    cdev->id    = id;
    cdev->input = input;
    cdev->last1 = &(struct coords){ 0, 0, 0 };
    cdev->last2 = &(struct coords){ 0, 0, 0 };

    // final setup
    // ----------------------------------------
    hid_set_drvdata(hdev, cdev);
    input_set_drvdata(input, cdev);

    ret = hid_parse(hdev);

    if (ret != 0) {
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);

    if (ret) {
        return ret;
    }

    printk(KERN_INFO "Cando Touchscreen driver loaded successfully\n");
    return 0;
}

// -----------------------------------------------------------------------------
static const struct hid_device_id cando_table[] = {
    { HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH, CANDO_USB_VENDOR_ID,
        CANDO_USB_PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(hid, cando_table);

static const struct hid_usage_id cando_grabbed_usages[] = {
    { HID_ANY_ID, HID_ANY_ID, HID_ANY_ID },
    { HID_ANY_ID - 1, HID_ANY_ID - 1, HID_ANY_ID - 1}
};

static struct hid_driver cando_driver = {
    .name             = "cando",
    .id_table         = cando_table,
    .probe            = cando_probe,
    // .remove           =
    // .event            = cando_event,
    .report           = cando_report,
};
module_hid_driver(cando_driver);
