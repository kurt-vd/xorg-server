/*
 * Copyright © 2013 Kurt Van Dijck
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Kurt Van Dijck <kurt.van.dijck@eia.be>
 * Based on config/hal.c: Julien Cristau <jcristau@debian.org>
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif
#include <opaque.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "input.h"
#include "inputstr.h"
#include "hotplug.h"
#include "config-backends.h"
#include "os.h"
#include "globals.h"
#include "optionstr.h"

/* backend name for use in remove_device etc. */
#define BACKENDSTR	"x11hotplug"
/* prefix for log messages */
#define LOGPREFIX	"config/" BACKENDSTR ": "
/* prefix for socket bind (display number is appended) */
#define SOCKPREFIX	"@x11hotplug:"

/* x11hotplug socket */
static int sock = -1;

static char buf[1024];

#define safe_xfree(x)	{ if (x) xfree(x); }

static void
x11hotplug_wakeup_handler(pointer data, int result, pointer read_mask)
{
	char *key, *value, *lineend, *hotplug_action = NULL, *config_info,
		*device = NULL;
	InputAttributes attrs = { };
	const char *failure;
	int ret;
	InputOption *options =
		input_option_new(NULL,"_source", "server/" BACKENDSTR);
	DeviceIntPtr dev = NULL;
	/* remote peer name */
	struct sockaddr_un pname;
	unsigned int pnamelen;

	if ((result <= 0) || (sock < 0) || !FD_ISSET(sock, (fd_set *)read_mask))
		return;

	pnamelen = sizeof(pname);
	ret = recvfrom(sock, buf, sizeof(buf)-1, 0, (void *)&pname, &pnamelen);
	if (ret <= 0)
		return;

	/* null terminator is not send along */
	buf[ret] = 0;
	/* parse lines into options */
	key = buf;
	do {
		lineend = strchr(key, '\n');
		if (lineend)
			/* insert null temporarily */
			*lineend = 0;
		if (!strlen(key)) {
			/* empty line, erase the last '\n' for replies */
			*(--key) = 0;
			break;
		}
		value = strchr(key, '=');
		if (value)
			/* insert null (temporarily) */
			*value++ = 0;
		/* process option */
		if (!strcmp(key, "action"))
			hotplug_action = xstrdup(value);
		else if (!strcmp(key, "type")) {
			if (!strcmp(value, "keyboard"))
				attrs.flags |= ATTR_KEYBOARD;
			else if (!strcmp(value, "pointer"))
				attrs.flags |= ATTR_POINTER;
			else if (!strcmp(value, "joystick"))
				attrs.flags |= ATTR_JOYSTICK;
			else if (!strcmp(value, "tablet"))
				attrs.flags |= ATTR_TABLET;
			else if (!strcmp(value, "touchscreen"))
				attrs.flags |= ATTR_TOUCHSCREEN;
			else if (!strcmp(value, "touchpad"))
				attrs.flags |= ATTR_TOUCHPAD;
		} else if (!strcmp(key, "device")) {
			attrs.device = xstrdup(value);
			device = xstrdup(attrs.device);
		} else if (!strcmp(key, "vendor"))
			attrs.vendor = xstrdup(value);
		else if (!strcmp(key, "product"))
			attrs.product = xstrdup(value);
		else if (!strcmp(key, "pnp_id"))
			attrs.pnp_id = xstrdup(value);
		else if (!strcmp(key, "usb_id"))
			attrs.usb_id = xstrdup(value);

		options = input_option_new(options, key, value);
		/* restore buffer */
		if (lineend)
			*lineend = '\n';
		if (value)
			*(value-1) = '=';
		key = lineend+1;
	} while (lineend);

	if (!device) {
		failure = "no device";
		goto done;
	}

	if (asprintf(&config_info, "%s:%s", BACKENDSTR, device) < 0) {
		failure = "asprintf failed";
		goto done;
	}
	options = input_option_new(options, "config_info", config_info);

	if (!hotplug_action)
		failure = "no action for event";
	else if (!strcmp(hotplug_action, "add")) {
		remove_devices(BACKENDSTR, config_info);
		if (NewInputDeviceRequest(options, &attrs, &dev) != Success) {
			failure = "NewInputDeviceRequest failed";
			ErrorF(LOGPREFIX "NewInputDeviceRequest %s failed\n",
					device);
			goto failed;
		}
		LogMessage(X_INFO, LOGPREFIX "New input device %s\n", device);
		failure = NULL;
	} else if (!strcmp(hotplug_action, "remove")) {
		remove_devices(BACKENDSTR, config_info);
		/* copied snippet from config/hal.c */
		failure = NULL;
	} else
               failure = "unsupported action";
done:
failed:
	key += sprintf(key, "\nresult=%s", failure ?: "ok");
	if (sendto(sock, buf, key-buf, 0, (void *)&pname, pnamelen) < 0)
		ErrorF(LOGPREFIX "sendto failed\n");

	if (failure) {
		ErrorF(LOGPREFIX "%s\n", failure);
		/* unwind options, we don't eat memory :-) */
		input_option_free_list(&options);
		safe_xfree(attrs.device);
		safe_xfree(attrs.usb_id);
		safe_xfree(attrs.pnp_id);
		safe_xfree(attrs.vendor);
		safe_xfree(attrs.product);
	}
	safe_xfree(hotplug_action);
	safe_xfree(device);
}

static void
x11hotplug_block_handler(pointer data, struct timeval **tv, pointer read_mask)
{
       /*
        * config_x11hotplug_init is called before AddGeneralSocket should be called.`
        * The server is now about to go to sleep, so here is the last chance to install
        * our custom socket
        */
       if ((sock >= 0) && !FD_ISSET(sock, (fd_set *)read_mask)) {
               AddGeneralSocket(sock);
               /* looked into WaitForSomething(), we should set this too */
               FD_SET(sock, (fd_set *)read_mask);
       }
}

int
config_x11hotplug_init(void)
{
       int ret, namelen;
       struct sockaddr_un name = {
               .sun_family = AF_UNIX,
       };

       /* create the x11hotplug socket */
       sprintf(name.sun_path, SOCKPREFIX "%s", display);
       namelen = SUN_LEN(&name);
       if (name.sun_path[0] == '@')
               /* make socket name in abstract namespace */
               name.sun_path[0] = 0;
       sock = socket(AF_UNIX, SOCK_DGRAM, 0);
       if (sock < 0) {
               ErrorF(LOGPREFIX "failed to create socket\n");
               goto fail_sock;
       }
       ret = bind(sock, (const void *)&name, namelen);
       if (ret < 0) {
               ErrorF(LOGPREFIX "failed to bind socket\n");
               goto fail_bind;
       }

       /*
        * Here would be a normal spot to call AddGeneralSocket()
        * in order to make the server listen to our newly created
        * socket.
        * But the FD management has not initialized here.
        * We solve this by moving AddGeneralSocket() to the
        * first call of x11hotplug_block_handler()
        */

       /* register socket in Xserver */
       RegisterBlockAndWakeupHandlers(x11hotplug_block_handler,
                       x11hotplug_wakeup_handler, NULL);
       return 1;

fail_bind:
       close(sock);
fail_sock:
       return 0;
}

void
config_x11hotplug_fini(void)
{
       if (sock < 0)
               return;

       RemoveGeneralSocket(sock);
       RemoveBlockAndWakeupHandlers(x11hotplug_block_handler,
                       x11hotplug_wakeup_handler, NULL);
       close(sock);
       sock = -1;
}

