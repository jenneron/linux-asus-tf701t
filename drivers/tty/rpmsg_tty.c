// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 STMicroelectronics - All Rights Reserved
 *
 * The rpmsg tty driver implements serial communication on the RPMsg bus to makes
 * possible for user-space programs to send and receive rpmsg messages as a standard
 * tty protocol.
 *
 * The remote processor can instantiate a new tty by requesting a "rpmsg-tty" RPMsg service.
 * The "rpmsg-tty" service is directly used for data exchange. No flow control is implemented yet.
 */

#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define MAX_TTY_RPMSG	32

static DEFINE_IDR(tty_idr);	/* tty instance id */
static DEFINE_MUTEX(idr_lock);	/* protects tty_idr */

static struct tty_driver *rpmsg_tty_driver;

struct rpmsg_tty_port {
	struct tty_port		port;	 /* TTY port data */
	int			id;	 /* TTY rpmsg index */
	struct rpmsg_device	*rpdev;	 /* rpmsg device */
};

static int rpmsg_tty_cb(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src)
{
	struct rpmsg_tty_port *cport = dev_get_drvdata(&rpdev->dev);
	int copied;

	if (!len)
		return -EINVAL;
	copied = tty_insert_flip_string(&cport->port, data, len);
	if (copied != len)
		dev_err_ratelimited(&rpdev->dev, "Trunc buffer: available space is %d\n", copied);
	tty_flip_buffer_push(&cport->port);

	return 0;
}

static int rpmsg_tty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct rpmsg_tty_port *cport = idr_find(&tty_idr, tty->index);

	tty->driver_data = cport;

	return tty_port_install(&cport->port, driver, tty);
}

static int rpmsg_tty_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(tty->port, tty, filp);
}

static void rpmsg_tty_close(struct tty_struct *tty, struct file *filp)
{
	return tty_port_close(tty->port, tty, filp);
}

static int rpmsg_tty_write(struct tty_struct *tty, const u8 *buf, int len)
{
	struct rpmsg_tty_port *cport = tty->driver_data;
	struct rpmsg_device *rpdev;
	int msg_max_size, msg_size;
	int ret;

	rpdev = cport->rpdev;

	msg_max_size = rpmsg_get_mtu(rpdev->ept);
	if (msg_max_size < 0)
		return msg_max_size;

	msg_size = min(len, msg_max_size);

	/*
	 * Use rpmsg_trysend instead of rpmsg_send to send the message so the caller is not
	 * hung until a rpmsg buffer is available. In such case rpmsg_trysend returns -ENOMEM.
	 */
	ret = rpmsg_trysend(rpdev->ept, (void *)buf, msg_size);
	if (ret) {
		dev_dbg_ratelimited(&rpdev->dev, "rpmsg_send failed: %d\n", ret);
		return ret;
	}

	return msg_size;
}

static unsigned int rpmsg_tty_write_room(struct tty_struct *tty)
{
	struct rpmsg_tty_port *cport = tty->driver_data;
	int size;

	size = rpmsg_get_mtu(cport->rpdev->ept);
	if (size < 0)
		return 0;

	return size;
}

static const struct tty_operations rpmsg_tty_ops = {
	.install	= rpmsg_tty_install,
	.open		= rpmsg_tty_open,
	.close		= rpmsg_tty_close,
	.write		= rpmsg_tty_write,
	.write_room	= rpmsg_tty_write_room,
};

static struct rpmsg_tty_port *rpmsg_tty_alloc_cport(void)
{
	struct rpmsg_tty_port *cport;
	int err;

	cport = kzalloc(sizeof(*cport), GFP_KERNEL);
	if (!cport)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&idr_lock);
	cport->id = idr_alloc(&tty_idr, cport, 0, MAX_TTY_RPMSG, GFP_KERNEL);
	mutex_unlock(&idr_lock);

	if (cport->id < 0) {
		err = cport->id;
		kfree(cport);
		return ERR_PTR(err);
	}

	return cport;
}

static void rpmsg_tty_release_cport(struct rpmsg_tty_port *cport)
{
	mutex_lock(&idr_lock);
	idr_remove(&tty_idr, cport->id);
	mutex_unlock(&idr_lock);

	kfree(cport);
}

static const struct tty_port_operations rpmsg_tty_port_ops = { };

static int rpmsg_tty_probe(struct rpmsg_device *rpdev)
{
	struct rpmsg_tty_port *cport;
	struct device *dev = &rpdev->dev;
	struct device *tty_dev;
	int ret;

	cport = rpmsg_tty_alloc_cport();
	if (IS_ERR(cport)) {
		dev_err(dev, "Failed to alloc tty port\n");
		return PTR_ERR(cport);
	}

	tty_port_init(&cport->port);
	cport->port.ops = &rpmsg_tty_port_ops;

	tty_dev = tty_port_register_device(&cport->port, rpmsg_tty_driver,
					   cport->id, dev);
	if (IS_ERR(tty_dev)) {
		dev_err(dev, "Failed to register tty port\n");
		ret = PTR_ERR(tty_dev);
		goto  err_destroy;
	}

	cport->rpdev = rpdev;

	dev_set_drvdata(dev, cport);

	dev_dbg(dev, "New channel: 0x%x -> 0x%x : ttyRPMSG%d\n",
		rpdev->src, rpdev->dst, cport->id);

	return 0;

err_destroy:
	tty_port_destroy(&cport->port);
	rpmsg_tty_release_cport(cport);

	return ret;
}

static void rpmsg_tty_remove(struct rpmsg_device *rpdev)
{
	struct rpmsg_tty_port *cport = dev_get_drvdata(&rpdev->dev);

	dev_dbg(&rpdev->dev, "Removing rpmsg tty device %d\n", cport->id);

	/* User hang up to release the tty */
	if (tty_port_initialized(&cport->port))
		tty_port_tty_hangup(&cport->port, false);

	tty_unregister_device(rpmsg_tty_driver, cport->id);

	tty_port_destroy(&cport->port);
	rpmsg_tty_release_cport(cport);
}

static struct rpmsg_device_id rpmsg_driver_tty_id_table[] = {
	{ .name	= "rpmsg-tty" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_tty_id_table);

static struct rpmsg_driver rpmsg_tty_rpmsg_drv = {
	.drv.name	= KBUILD_MODNAME,
	.id_table	= rpmsg_driver_tty_id_table,
	.probe		= rpmsg_tty_probe,
	.callback	= rpmsg_tty_cb,
	.remove		= rpmsg_tty_remove,
};

static int __init rpmsg_tty_init(void)
{
	int err;

	rpmsg_tty_driver = tty_alloc_driver(MAX_TTY_RPMSG, TTY_DRIVER_REAL_RAW |
					    TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(rpmsg_tty_driver))
		return PTR_ERR(rpmsg_tty_driver);

	rpmsg_tty_driver->driver_name = "rpmsg_tty";
	rpmsg_tty_driver->name = "ttyRPMSG";
	rpmsg_tty_driver->major = 0;
	rpmsg_tty_driver->type = TTY_DRIVER_TYPE_CONSOLE;

	/* Disable unused mode by default */
	rpmsg_tty_driver->init_termios = tty_std_termios;
	rpmsg_tty_driver->init_termios.c_lflag &= ~(ECHO | ICANON);
	rpmsg_tty_driver->init_termios.c_oflag &= ~(OPOST | ONLCR);

	tty_set_operations(rpmsg_tty_driver, &rpmsg_tty_ops);

	err = tty_register_driver(rpmsg_tty_driver);
	if (err < 0) {
		pr_err("Couldn't install rpmsg tty driver: err %d\n", err);
		goto error_put;
	}

	err = register_rpmsg_driver(&rpmsg_tty_rpmsg_drv);
	if (err < 0) {
		pr_err("Couldn't register rpmsg tty driver: err %d\n", err);
		goto error_unregister;
	}

	return 0;

error_unregister:
	tty_unregister_driver(rpmsg_tty_driver);

error_put:
	tty_driver_kref_put(rpmsg_tty_driver);

	return err;
}

static void __exit rpmsg_tty_exit(void)
{
	unregister_rpmsg_driver(&rpmsg_tty_rpmsg_drv);
	tty_unregister_driver(rpmsg_tty_driver);
	tty_driver_kref_put(rpmsg_tty_driver);
	idr_destroy(&tty_idr);
}

module_init(rpmsg_tty_init);
module_exit(rpmsg_tty_exit);

MODULE_AUTHOR("Arnaud Pouliquen <arnaud.pouliquen@foss.st.com>");
MODULE_DESCRIPTION("remote processor messaging tty driver");
MODULE_LICENSE("GPL v2");
