/*
  USB Driver layer for GSM modems

  Copyright (C) 2005  Matthias Urlichs <smurf@smurf.noris.de>

  This driver is free software; you can redistribute it and/or modify
  it under the terms of Version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  Portions copied from the Keyspan driver by Hugh Blemings <hugh@blemings.org>

  History: see the git log.

  Work sponsored by: Sigos GmbH, Germany <info@sigos.de>

  This driver exists because the "normal" serial driver doesn't work too well
  with GSM modems. Issues:
  - data loss -- one single Receive URB is not nearly enough
  - controlling the baud rate doesn't make sense
*/

#define DRIVER_VERSION "v0.7.2"
#define DRIVER_AUTHOR "Matthias Urlichs <smurf@smurf.noris.de>"
#define DRIVER_DESC "USB Driver for GSM modems"

#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/usb/serial.h>
#include <linux/serial.h>
#include "usb-wwan.h"
#include <linux/mdm_hsic_pm.h>

static int debug;
#if defined(CONFIG_MACH_JA_KOR_LGT)
static void usb_wwan_mdm_checksync(struct work_struct *work)
{

	struct usb_wwan_intf_private *intfdata =
			container_of(work, struct usb_wwan_intf_private,
					sync_check_work.work);
	
	if (atomic_read(&intfdata->sync_read_cnt)) {
		if(intfdata->temp_tty) {
			pr_info("mdm: efs, %s(), Call tty_flip_buffer_push again\n", __func__);
			tty_flip_buffer_push(intfdata->temp_tty);
		} else {
			pr_info("mdm: efs, %s(), !intfdata->temp_tty\n", __func__);
		}
	}
}
#endif

void usb_wwan_dtr_rts(struct usb_serial_port *port, int on)
{
	struct usb_wwan_port_private *portdata;

	struct usb_wwan_intf_private *intfdata;

	dbg("%s", __func__);

	intfdata = port->serial->private;

	if (!intfdata->send_setup)
		return;

	portdata = usb_get_serial_port_data(port);
	/* FIXME: locking */
	portdata->rts_state = on;
	portdata->dtr_state = on;

	intfdata->send_setup(port);
}
EXPORT_SYMBOL(usb_wwan_dtr_rts);

void usb_wwan_set_termios(struct tty_struct *tty,
			  struct usb_serial_port *port,
			  struct ktermios *old_termios)
{
	struct usb_wwan_intf_private *intfdata = port->serial->private;

	dbg("%s", __func__);

	/* Doesn't support option setting */
	tty_termios_copy_hw(tty->termios, old_termios);

	if (intfdata->send_setup)
		intfdata->send_setup(port);
}
EXPORT_SYMBOL(usb_wwan_set_termios);

int usb_wwan_tiocmget(struct tty_struct *tty)
{
	struct usb_serial_port *port = tty->driver_data;
	unsigned int value;
	struct usb_wwan_port_private *portdata;

	portdata = usb_get_serial_port_data(port);

	value = ((portdata->rts_state) ? TIOCM_RTS : 0) |
	    ((portdata->dtr_state) ? TIOCM_DTR : 0) |
	    ((portdata->cts_state) ? TIOCM_CTS : 0) |
	    ((portdata->dsr_state) ? TIOCM_DSR : 0) |
	    ((portdata->dcd_state) ? TIOCM_CAR : 0) |
	    ((portdata->ri_state) ? TIOCM_RNG : 0);

	return value;
}
EXPORT_SYMBOL(usb_wwan_tiocmget);

int usb_wwan_tiocmset(struct tty_struct *tty,
		      unsigned int set, unsigned int clear)
{
	struct usb_serial_port *port = tty->driver_data;
	struct usb_wwan_port_private *portdata;
	struct usb_wwan_intf_private *intfdata;

	portdata = usb_get_serial_port_data(port);
	intfdata = port->serial->private;

	if (!intfdata->send_setup)
		return -EINVAL;

	/* FIXME: what locks portdata fields ? */
	if (set & TIOCM_RTS)
		portdata->rts_state = 1;
	if (set & TIOCM_DTR)
		portdata->dtr_state = 1;

	if (clear & TIOCM_RTS)
		portdata->rts_state = 0;
	if (clear & TIOCM_DTR)
		portdata->dtr_state = 0;
	return intfdata->send_setup(port);
}
EXPORT_SYMBOL(usb_wwan_tiocmset);

static int get_serial_info(struct usb_serial_port *port,
			   struct serial_struct __user *retinfo)
{
	struct serial_struct tmp;

	if (!retinfo)
		return -EFAULT;

	memset(&tmp, 0, sizeof(tmp));
	tmp.line            = port->serial->minor;
	tmp.port            = port->number;
	tmp.baud_base       = tty_get_baud_rate(port->port.tty);
	tmp.close_delay	    = port->port.close_delay / 10;
	tmp.closing_wait    = port->port.closing_wait == ASYNC_CLOSING_WAIT_NONE ?
				 ASYNC_CLOSING_WAIT_NONE :
				 port->port.closing_wait / 10;

	if (copy_to_user(retinfo, &tmp, sizeof(*retinfo)))
		return -EFAULT;
	return 0;
}

static int set_serial_info(struct usb_serial_port *port,
			   struct serial_struct __user *newinfo)
{
	struct serial_struct new_serial;
	unsigned int closing_wait, close_delay;
	int retval = 0;

	if (copy_from_user(&new_serial, newinfo, sizeof(new_serial)))
		return -EFAULT;

	close_delay = new_serial.close_delay * 10;
	closing_wait = new_serial.closing_wait == ASYNC_CLOSING_WAIT_NONE ?
			ASYNC_CLOSING_WAIT_NONE : new_serial.closing_wait * 10;

	mutex_lock(&port->port.mutex);

	if (!capable(CAP_SYS_ADMIN)) {
		if ((close_delay != port->port.close_delay) ||
		    (closing_wait != port->port.closing_wait))
			retval = -EPERM;
		else
			retval = -EOPNOTSUPP;
	} else {
		port->port.close_delay  = close_delay;
		port->port.closing_wait = closing_wait;
	}

	mutex_unlock(&port->port.mutex);
	return retval;
}

int usb_wwan_ioctl(struct tty_struct *tty,
		   unsigned int cmd, unsigned long arg)
{
	struct usb_serial_port *port = tty->driver_data;

	dbg("%s cmd 0x%04x", __func__, cmd);

	switch (cmd) {
	case TIOCGSERIAL:
		return get_serial_info(port,
				       (struct serial_struct __user *) arg);
	case TIOCSSERIAL:
		return set_serial_info(port,
				       (struct serial_struct __user *) arg);
	default:
		break;
	}

	dbg("%s arg not supported", __func__);

	return -ENOIOCTLCMD;
}
EXPORT_SYMBOL(usb_wwan_ioctl);

/* Write */
int usb_wwan_write(struct tty_struct *tty, struct usb_serial_port *port,
		   const unsigned char *buf, int count)
{
	struct usb_wwan_port_private *portdata;
	struct usb_wwan_intf_private *intfdata;
	int i;
	int left, todo;
	struct urb *this_urb = NULL;	/* spurious */
	int err;
	unsigned long flags;

	portdata = usb_get_serial_port_data(port);
	intfdata = port->serial->private;

	dbg("%s: write (%d chars)", __func__, count);

#ifdef CONFIG_MDM_HSIC_PM
	if (port->serial->dev->actconfig->desc.bNumInterfaces == 9)
		pr_info("mdm: efs, write %d chars", count);
#endif

#if defined(CONFIG_MACH_JA_KOR_LGT)
	if (atomic_read(&intfdata->sync_read_cnt)) {
		cancel_delayed_work(&intfdata->sync_check_work);
		atomic_dec(&intfdata->sync_read_cnt);
		intfdata->temp_tty=NULL;
	}
#endif

	i = 0;
	left = count;
	for (i = 0; left > 0 && i < N_OUT_URB; i++) {
		todo = left;
		if (todo > OUT_BUFLEN)
			todo = OUT_BUFLEN;

		this_urb = portdata->out_urbs[i];
		if (test_and_set_bit(i, &portdata->out_busy)) {
			if (time_before(jiffies,
					portdata->tx_start_time[i] + 10 * HZ))
				continue;
			usb_unlink_urb(this_urb);
			continue;
		}
		dbg("%s: endpoint %d buf %d", __func__,
		    usb_pipeendpoint(this_urb->pipe), i);

		usb_mark_last_busy(
				interface_to_usbdev(port->serial->interface));
		err = usb_autopm_get_interface_async(port->serial->interface);
		if (err < 0) {
			clear_bit(i, &portdata->out_busy);
			break;
		}

		/* send the data */
		memcpy(this_urb->transfer_buffer, buf, todo);
		this_urb->transfer_buffer_length = todo;

		spin_lock_irqsave(&intfdata->susp_lock, flags);
		if (intfdata->suspended) {
			usb_anchor_urb(this_urb, &portdata->delayed);
			spin_unlock_irqrestore(&intfdata->susp_lock, flags);
		} else {
			intfdata->in_flight++;
			spin_unlock_irqrestore(&intfdata->susp_lock, flags);
			err = usb_submit_urb(this_urb, GFP_ATOMIC);
			if (err) {
				dbg("usb_submit_urb %p (write bulk) failed "
				    "(%d)", this_urb, err);
				clear_bit(i, &portdata->out_busy);
				spin_lock_irqsave(&intfdata->susp_lock, flags);
				intfdata->in_flight--;
				spin_unlock_irqrestore(&intfdata->susp_lock,
						       flags);
				usb_autopm_put_interface_async(port->serial->interface);
				break;
			}
		}

		portdata->tx_start_time[i] = jiffies;
		buf += todo;
		left -= todo;
	}

	count -= left;
	dbg("%s: wrote (did %d)", __func__, count);
	return count;
}
EXPORT_SYMBOL(usb_wwan_write);

#ifdef CONFIG_MDM_HSIC_PM
#define HELLO_PACKET_SIZE 48
#endif
static void usb_wwan_indat_callback(struct urb *urb)
{
	int err;
	int endpoint;
	struct usb_serial_port *port;
	struct tty_struct *tty;
	unsigned char *data = urb->transfer_buffer;
	int status = urb->status;
	int rx_len = urb->actual_length, added;
	struct usb_wwan_intf_private *intfdata;
	struct usb_device *udev;

	dbg("%s: %p", __func__, urb);

	endpoint = usb_pipeendpoint(urb->pipe);
	port = urb->context;
	intfdata = port->serial->private;

	usb_mark_last_busy(interface_to_usbdev(port->serial->interface));
	if (status) {
		dbg("%s: nonzero status: %d on endpoint %02x.",
		    __func__, status, endpoint);
#ifdef CONFIG_MDM_HSIC_PM
		if (status == -ENOENT && (urb->actual_length > 0)) {
			pr_info("%s: handle dropped packet\n", __func__);
			udev = port->serial->dev;
			if (udev->actconfig->desc.bNumInterfaces == 9 &&
							hello_packet_rx) {
				pr_info("%s: dup packet handled\n", __func__);
				return;
			}
			goto handle_rx;
		}
#endif
	} else {
#ifdef CONFIG_MDM_HSIC_PM
handle_rx:
#endif
		tty = tty_port_tty_get(&port->port);
		if (tty) {
			if (urb->actual_length > 0) {
#ifdef CONFIG_MDM_HSIC_PM
				udev = port->serial->dev;
				/* corner case : efs packet rx at rpm suspend
				 * it can control twice in racing condition
				 * rx call back and suspend, both of then ca
				 * call this function , so clear the packet
				 * length once it handled
				 */
				urb->status = -EINPROGRESS;
				urb->actual_length = 0;
				if (udev->actconfig->desc.bNumInterfaces == 9) {
					pr_info("mdm: efs, read %d bytes\n",
									rx_len);
					if (rx_len == HELLO_PACKET_SIZE)
						hello_packet_rx = 1;
					else
						hello_packet_rx = 0;
				}
#endif
				added = tty_insert_flip_string(tty, data, rx_len);
				if(added != rx_len)
					pr_info("mdm: rx_len:(%d)!= added:(%d)\n", rx_len, added);

				tty_flip_buffer_push(tty);
#if defined(CONFIG_MACH_JA_KOR_LGT)
				if (udev->actconfig->desc.bNumInterfaces == 9) {
					if((16384 == rx_len) && (added == rx_len)) {
						intfdata->temp_tty = tty;
						queue_delayed_work(intfdata->wq,
							&intfdata->sync_check_work, msecs_to_jiffies(5000));
						atomic_inc(&intfdata->sync_read_cnt);
					}
				}
#endif
			} else
				dbg("%s: empty read urb received", __func__);
			tty_kref_put(tty);
		} else
			return;

#ifdef CONFIG_MDM_HSIC_PM
		/* do not re-submit urb for no entry status */
		if (status == -ENOENT)
			return;
#endif
		/* Resubmit urb so we continue receiving */
		if (status != -ESHUTDOWN || !intfdata->suspended) {
			err = usb_submit_urb(urb, GFP_ATOMIC);
			if (err) {
				if (err != -EPERM) {
					printk(KERN_ERR "%s: resubmit read urb failed. "
						"(%d)", __func__, err);
					/* busy also in error unless we are killed */
					usb_mark_last_busy(port->serial->dev);
				}
			} else {
				usb_mark_last_busy(port->serial->dev);
			}
			usb_mark_last_busy(
				interface_to_usbdev(port->serial->interface));
		}

	}
}

static void usb_wwan_outdat_callback(struct urb *urb)
{
	struct usb_serial_port *port;
	struct usb_wwan_port_private *portdata;
	struct usb_wwan_intf_private *intfdata;
	int i;

	dbg("%s", __func__);

	port = urb->context;
	intfdata = port->serial->private;

	usb_serial_port_softint(port);
	usb_autopm_put_interface_async(port->serial->interface);
	usb_mark_last_busy(interface_to_usbdev(port->serial->interface));
	portdata = usb_get_serial_port_data(port);
	spin_lock(&intfdata->susp_lock);
	intfdata->in_flight--;
	spin_unlock(&intfdata->susp_lock);

	for (i = 0; i < N_OUT_URB; ++i) {
		if (portdata->out_urbs[i] == urb) {
			smp_mb__before_clear_bit();
			clear_bit(i, &portdata->out_busy);
			break;
		}
	}
}

int usb_wwan_write_room(struct tty_struct *tty)
{
	struct usb_serial_port *port = tty->driver_data;
	struct usb_wwan_port_private *portdata;
	int i;
	int data_len = 0;
	struct urb *this_urb;

	portdata = usb_get_serial_port_data(port);

	for (i = 0; i < N_OUT_URB; i++) {
		this_urb = portdata->out_urbs[i];
		if (this_urb && !test_bit(i, &portdata->out_busy))
			data_len += OUT_BUFLEN;
	}

	dbg("%s: %d", __func__, data_len);
	return data_len;
}
EXPORT_SYMBOL(usb_wwan_write_room);

int usb_wwan_chars_in_buffer(struct tty_struct *tty)
{
	struct usb_serial_port *port = tty->driver_data;
	struct usb_wwan_port_private *portdata;
	int i;
	int data_len = 0;
	struct urb *this_urb;

	portdata = usb_get_serial_port_data(port);

	for (i = 0; i < N_OUT_URB; i++) {
		this_urb = portdata->out_urbs[i];
		/* FIXME: This locking is insufficient as this_urb may
		   go unused during the test */
		if (this_urb && test_bit(i, &portdata->out_busy))
			data_len += this_urb->transfer_buffer_length;
	}
	dbg("%s: %d", __func__, data_len);
	return data_len;
}
EXPORT_SYMBOL(usb_wwan_chars_in_buffer);

int usb_wwan_open(struct tty_struct *tty, struct usb_serial_port *port)
{
	struct usb_wwan_port_private *portdata;
	struct usb_wwan_intf_private *intfdata;
	struct usb_serial *serial = port->serial;
	int i, err;
	struct urb *urb;

	portdata = usb_get_serial_port_data(port);
	intfdata = serial->private;
	intfdata->suspended = 0;

	/* explicitly set the driver mode to raw */
	tty->raw = 1;
	tty->real_raw = 1;

	set_bit(TTY_NO_WRITE_SPLIT, &tty->flags);
	dbg("%s", __func__);

	if (port->interrupt_in_urb) {
		err = usb_submit_urb(port->interrupt_in_urb, GFP_KERNEL);
		if (err) {
			dev_dbg(&port->dev, "%s: submit int urb failed: %d\n",
				__func__, err);
		}
	}

	/* Start reading from the IN endpoint */
	for (i = 0; i < N_IN_URB; i++) {
		urb = portdata->in_urbs[i];
		if (!urb)
			continue;
		err = usb_submit_urb(urb, GFP_KERNEL);
		if (err) {
			dbg("%s: submit urb %d failed (%d) %d",
			    __func__, i, err, urb->transfer_buffer_length);
		}
	}

	if (intfdata->send_setup)
		intfdata->send_setup(port);

	serial->interface->needs_remote_wakeup = 1;
	spin_lock_irq(&intfdata->susp_lock);
	portdata->opened = 1;
	spin_unlock_irq(&intfdata->susp_lock);
	/* this balances a get in the generic USB serial code */
	usb_autopm_put_interface(serial->interface);

	return 0;
}
EXPORT_SYMBOL(usb_wwan_open);

static void unbusy_queued_urb(struct urb *urb,
					struct usb_wwan_port_private *portdata)
{
	int i;

	for (i = 0; i < N_OUT_URB; i++) {
		if (urb == portdata->out_urbs[i]) {
			clear_bit(i, &portdata->out_busy);
			break;
		}
	}
}

void usb_wwan_close(struct usb_serial_port *port)
{
	int i;
	struct usb_serial *serial = port->serial;
	struct usb_wwan_port_private *portdata;
	struct usb_wwan_intf_private *intfdata = port->serial->private;
	struct urb *urb;

	dbg("%s", __func__);
	portdata = usb_get_serial_port_data(port);

	if (serial->dev) {
		/* Stop reading/writing urbs */
		spin_lock_irq(&intfdata->susp_lock);
		portdata->opened = 0;
		spin_unlock_irq(&intfdata->susp_lock);

		for (;;) {
			urb = usb_get_from_anchor(&portdata->delayed);
			if (!urb)
				break;
			unbusy_queued_urb(urb, portdata);
			usb_autopm_put_interface_async(serial->interface);
		}

		for (i = 0; i < N_IN_URB; i++)
			usb_kill_urb(portdata->in_urbs[i]);
		for (i = 0; i < N_OUT_URB; i++)
			usb_kill_urb(portdata->out_urbs[i]);
		usb_kill_urb(port->interrupt_in_urb);
		/* balancing - important as an error cannot be handled*/
		usb_autopm_get_interface_no_resume(serial->interface);
		serial->interface->needs_remote_wakeup = 0;
	}
}
EXPORT_SYMBOL(usb_wwan_close);

/* Helper functions used by usb_wwan_setup_urbs */
static struct urb *usb_wwan_setup_urb(struct usb_serial *serial, int endpoint,
				      int dir, void *ctx, char *buf, int len,
				      void (*callback) (struct urb *))
{
	struct urb *urb;

	if (endpoint == -1)
		return NULL;	/* endpoint not needed */

	urb = usb_alloc_urb(0, GFP_KERNEL);	/* No ISO */
	if (urb == NULL) {
		dbg("%s: alloc for endpoint %d failed.", __func__, endpoint);
		return NULL;
	}

	/* Fill URB using supplied data. */
	usb_fill_bulk_urb(urb, serial->dev,
			  usb_sndbulkpipe(serial->dev, endpoint) | dir,
			  buf, len, callback, ctx);

	return urb;
}

/* Setup urbs */
static void usb_wwan_setup_urbs(struct usb_serial *serial)
{
	int i, j;
	struct usb_serial_port *port;
	struct usb_wwan_port_private *portdata;

	dbg("%s", __func__);

	for (i = 0; i < serial->num_ports; i++) {
		port = serial->port[i];
		portdata = usb_get_serial_port_data(port);

		/* Do indat endpoints first */
		for (j = 0; j < N_IN_URB; ++j) {
			portdata->in_urbs[j] = usb_wwan_setup_urb(serial,
								  port->
								  bulk_in_endpointAddress,
								  USB_DIR_IN,
								  port,
								  portdata->
								  in_buffer[j],
								  IN_BUFLEN,
								  usb_wwan_indat_callback);
		}

		/* outdat endpoints */
		for (j = 0; j < N_OUT_URB; ++j) {
			portdata->out_urbs[j] = usb_wwan_setup_urb(serial,
								   port->
								   bulk_out_endpointAddress,
								   USB_DIR_OUT,
								   port,
								   portdata->
								   out_buffer
								   [j],
								   OUT_BUFLEN,
								   usb_wwan_outdat_callback);
		}
	}
}

#ifdef CONFIG_MDM_HSIC_PM
/* prevent runtime mem re-allocation when it once attached */
static struct usb_wwan_port_private *mdm_portdata[MAX_NUM_PORTS];
#endif
int usb_wwan_startup(struct usb_serial *serial)
{
	int i, j;
	struct usb_serial_port *port;
	struct usb_wwan_port_private *portdata;
	u8 *buffer;
#ifdef CONFIG_MDM_HSIC_PM
	struct usb_wwan_intf_private *data = usb_get_serial_data(serial);
#endif

	dbg("%s", __func__);

	/* Now setup per port private data */
	for (i = 0; i < serial->num_ports; i++) {
		port = serial->port[i];
#ifdef CONFIG_MDM_HSIC_PM
		if (data && data->buf_reuse && mdm_portdata[i]) {
			portdata = mdm_portdata[i];
			goto set_port_data;
		}
#endif
		portdata = kzalloc(sizeof(*portdata), GFP_KERNEL);
		if (!portdata) {
			dbg("%s: kmalloc for usb_wwan_port_private (%d) failed!.",
			    __func__, i);
			return 1;
		}
		init_usb_anchor(&portdata->delayed);

		for (j = 0; j < N_IN_URB; j++) {
			buffer = kmalloc(IN_BUFLEN, GFP_KERNEL);
			if (!buffer)
				goto bail_out_error;
			portdata->in_buffer[j] = buffer;
		}

		for (j = 0; j < N_OUT_URB; j++) {
			buffer = kmalloc(OUT_BUFLEN, GFP_KERNEL);
			if (!buffer)
				goto bail_out_error2;
			portdata->out_buffer[j] = buffer;
		}
#ifdef CONFIG_MDM_HSIC_PM
		if (data && data->buf_reuse)
			mdm_portdata[i] = portdata;
set_port_data:
#endif
		usb_set_serial_port_data(port, portdata);
	}
	usb_wwan_setup_urbs(serial);
#if defined(CONFIG_MACH_JA_KOR_LGT)
	atomic_set(&data->sync_read_cnt, 0);
	INIT_DELAYED_WORK(&data->sync_check_work, usb_wwan_mdm_checksync);

	data->wq = create_singlethread_workqueue("usbwwand");
	if (!data->wq) {
		pr_err("%s: fail to create wq\n", __func__);
	}
#endif
	return 0;

bail_out_error2:
	for (j = 0; j < N_OUT_URB; j++)
		kfree(portdata->out_buffer[j]);
bail_out_error:
	for (j = 0; j < N_IN_URB; j++)
		kfree(portdata->in_buffer[j]);
	kfree(portdata);
	return 1;
}
EXPORT_SYMBOL(usb_wwan_startup);

static void stop_read_write_urbs(struct usb_serial *serial)
{
	int i, j;
	struct usb_serial_port *port;
	struct usb_wwan_port_private *portdata;

	/* Stop reading/writing urbs */
	for (i = 0; i < serial->num_ports; ++i) {
		port = serial->port[i];
		portdata = usb_get_serial_port_data(port);
		for (j = 0; j < N_IN_URB; j++)
			usb_kill_urb(portdata->in_urbs[j]);
		for (j = 0; j < N_OUT_URB; j++)
			usb_kill_urb(portdata->out_urbs[j]);
	}
}

void usb_wwan_disconnect(struct usb_serial *serial)
{
	struct usb_wwan_intf_private *intfdata = serial->private;
	dbg("%s", __func__);

	intfdata->suspended = 1;
	stop_read_write_urbs(serial);
}
EXPORT_SYMBOL(usb_wwan_disconnect);

void usb_wwan_release(struct usb_serial *serial)
{
	int i, j;
	struct usb_serial_port *port;
	struct usb_wwan_port_private *portdata;
#ifdef CONFIG_MDM_HSIC_PM
	struct usb_wwan_intf_private *data = usb_get_serial_data(serial);
#endif

	dbg("%s", __func__);
#if defined(CONFIG_MACH_JA_KOR_LGT)
	if (atomic_read(&data->sync_read_cnt)) {
		cancel_delayed_work(&data->sync_check_work);
		atomic_set(&data->sync_read_cnt, 0);
		data->temp_tty=NULL;
	}
	destroy_workqueue(data->wq);
#endif
	/* Now free them */
	for (i = 0; i < serial->num_ports; ++i) {
		port = serial->port[i];
		portdata = usb_get_serial_port_data(port);

		for (j = 0; j < N_IN_URB; j++) {
			usb_free_urb(portdata->in_urbs[j]);
#ifdef CONFIG_MDM_HSIC_PM
			if (data && !data->buf_reuse)
				kfree(portdata->in_buffer[j]);
#else
			kfree(portdata->in_buffer[j]);
#endif
			portdata->in_urbs[j] = NULL;
		}
		for (j = 0; j < N_OUT_URB; j++) {
			usb_free_urb(portdata->out_urbs[j]);
#ifdef CONFIG_MDM_HSIC_PM
			if (data && !data->buf_reuse)
				kfree(portdata->out_buffer[j]);
#else
			kfree(portdata->out_buffer[j]);
#endif
			portdata->out_urbs[j] = NULL;
		}
	}

	/* Now free per port private data */
#ifdef CONFIG_MDM_HSIC_PM
	if (data && data->buf_reuse)
		return;
#endif
	for (i = 0; i < serial->num_ports; i++) {
		port = serial->port[i];
		kfree(usb_get_serial_port_data(port));
	}
}
EXPORT_SYMBOL(usb_wwan_release);

#ifdef CONFIG_PM
int usb_wwan_suspend(struct usb_serial *serial, pm_message_t message)
{
	struct usb_wwan_intf_private *intfdata = serial->private;

	dbg("%s entered", __func__);

	spin_lock_irq(&intfdata->susp_lock);
	if (message.event & PM_EVENT_AUTO) {
		if (intfdata->in_flight) {
			spin_unlock_irq(&intfdata->susp_lock);
			return -EBUSY;
		}
	}

	intfdata->suspended = 1;
	spin_unlock_irq(&intfdata->susp_lock);

	stop_read_write_urbs(serial);

	return 0;
}
EXPORT_SYMBOL(usb_wwan_suspend);

static int play_delayed(struct usb_serial_port *port)
{
	struct usb_wwan_intf_private *data;
	struct usb_wwan_port_private *portdata;
	struct urb *urb;
	int err = 0;
{
	struct usb_wwan_intf_private *data;
	struct usb_wwan_port_private *portdata;
	struct urb *urb;
	int err = 0;

	portdata = usb_get_serial_port_data(port);
	data = port->serial->private;
	while ((urb = usb_get_from_anchor(&portdata->delayed))) {
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (!err) {
			data->in_flight++;
		} else {
			/* we have to throw away the rest */
			do {
				unbusy_queued_urb(urb, portdata);
				usb_autopm_put_interface_no_suspend(port->serial->interface);
			} while ((urb = usb_get_from_anchor(&portdata->delayed)));
			break;
		}
	}

	return err;
}

int usb_wwan_resume(struct usb_serial *serial)
{
	int i, j;
	struct usb_serial_port *port;
	struct usb_wwan_intf_private *intfdata = serial->private;
	struct usb_wwan_port_private *portdata;
	struct urb *urb;
	int err;
	int err_count = 0;

	dbg("%s entered", __func__);

	spin_lock_irq(&intfdata->susp_lock);
	for (i = 0; i < serial->num_ports; i++) {
		/* walk all ports */
		port = serial->port[i];
		portdata = usb_get_serial_port_data(port);

		/* skip closed ports */
		if (!portdata || !portdata->opened)
			continue;

		if (port->interrupt_in_urb) {
			err = usb_submit_urb(port->interrupt_in_urb,
					GFP_ATOMIC);
			if (err) {
				dev_err(&port->dev,
					"%s: submit int urb failed: %d\n",
					__func__, err);
				err_count++;
			}
		}

		err = play_delayed(port);
		if (err)
			err_count++;

		for (j = 0; j < N_IN_URB; j++) {
			urb = portdata->in_urbs[j];
			err = usb_submit_urb(urb, GFP_ATOMIC);
			if (err < 0) {
				err("%s: Error %d for bulk URB %d",
				    __func__, err, i);
				err_count++;
			}
		}
	}
	intfdata->suspended = 0;
	spin_unlock_irq(&intfdata->susp_lock);

	if (err_count)
		return -EIO;

	return 0;
}
EXPORT_SYMBOL(usb_wwan_resume);
#endif

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

module_param(debug, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug messages");
