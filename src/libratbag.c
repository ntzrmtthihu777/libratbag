/*
 * Copyright © 2015 Red Hat, Inc.
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
 */

#include "config.h"
#include <assert.h>
#include <errno.h>
#include <libudev.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#include "usb-ids.h"
#include "libratbag-private.h"
#include "libratbag-util.h"

static enum ratbag_error_code
error_code(enum ratbag_error_code code)
{
	switch(code) {
	case RATBAG_SUCCESS:
	case RATBAG_ERROR_DEVICE:
	case RATBAG_ERROR_CAPABILITY:
	case RATBAG_ERROR_VALUE:
	case RATBAG_ERROR_SYSTEM:
	case RATBAG_ERROR_IMPLEMENTATION:
		break;
	default:
		assert(!"Invalid error code. This is a library bug.");
	}
	return code;
}

static void
ratbag_profile_destroy(struct ratbag_profile *profile);
static void
ratbag_button_destroy(struct ratbag_button *button);

static void
ratbag_default_log_func(struct ratbag *ratbag,
			enum ratbag_log_priority priority,
			const char *format, va_list args)
{
	const char *prefix;

	switch(priority) {
	case RATBAG_LOG_PRIORITY_RAW: prefix = "raw"; break;
	case RATBAG_LOG_PRIORITY_DEBUG: prefix = "debug"; break;
	case RATBAG_LOG_PRIORITY_INFO: prefix = "info"; break;
	case RATBAG_LOG_PRIORITY_ERROR: prefix = "error"; break;
	default: prefix="<invalid priority>"; break;
	}

	fprintf(stderr, "ratbag %s: ", prefix);
	vfprintf(stderr, format, args);
}

void
log_msg_va(struct ratbag *ratbag,
	   enum ratbag_log_priority priority,
	   const char *format,
	   va_list args)
{
	if (ratbag->log_handler &&
	    ratbag->log_priority <= priority)
		ratbag->log_handler(ratbag, priority, format, args);
}

void
log_msg(struct ratbag *ratbag,
	enum ratbag_log_priority priority,
	const char *format, ...)
{
	va_list args;

	va_start(args, format);
	log_msg_va(ratbag, priority, format, args);
	va_end(args);
}

void
log_buffer(struct ratbag *ratbag,
	enum ratbag_log_priority priority,
	const char *header,
	uint8_t *buf, size_t len)
{
	_cleanup_free_ char *output_buf = NULL;
	char *sep = "";
	unsigned int i, n;
	unsigned int buf_len;

	if (ratbag->log_handler &&
	    ratbag->log_priority > priority)
		return;

	buf_len = header ? strlen(header) : 0;
	buf_len += len * 3;
	buf_len += 1; /* terminating '\0' */

	output_buf = zalloc(buf_len);
	n = 0;
	if (header)
		n += snprintf_safe(output_buf, buf_len - n, "%s", header);

	for (i = 0; i < len; ++i) {
		n += snprintf_safe(&output_buf[n], buf_len - n, "%s%02x", sep, buf[i] & 0xFF);
		sep = " ";
	}

	log_msg(ratbag, priority, "%s\n", output_buf);
}

LIBRATBAG_EXPORT void
ratbag_log_set_priority(struct ratbag *ratbag,
			enum ratbag_log_priority priority)
{
	switch (priority) {
	case RATBAG_LOG_PRIORITY_RAW:
	case RATBAG_LOG_PRIORITY_DEBUG:
	case RATBAG_LOG_PRIORITY_INFO:
	case RATBAG_LOG_PRIORITY_ERROR:
		break;
	default:
		log_bug_client(ratbag,
			       "Invalid log priority %d. Using INFO instead\n",
			       priority);
		priority = RATBAG_LOG_PRIORITY_INFO;
	}
	ratbag->log_priority = priority;
}

LIBRATBAG_EXPORT enum ratbag_log_priority
ratbag_log_get_priority(const struct ratbag *ratbag)
{
	return ratbag->log_priority;
}

LIBRATBAG_EXPORT void
ratbag_log_set_handler(struct ratbag *ratbag,
		       ratbag_log_handler log_handler)
{
	ratbag->log_handler = log_handler;
}

struct ratbag_device*
ratbag_device_new(struct ratbag *ratbag, struct udev_device *udev_device,
		  const char *name, const struct input_id *id)
{
	struct ratbag_device *device = NULL;

	device = zalloc(sizeof(*device));
	device->name = strdup_safe(name);
	device->ratbag = ratbag_ref(ratbag);
	device->refcount = 1;
	device->udev_device = udev_device_ref(udev_device);
	device->ids = *id;
	list_init(&device->profiles);

	list_insert(&ratbag->devices, &device->link);

	/* We assume that most devices have this capability, so let's set it
	 * by default. The few devices that miss this capability should
	 * unset it instead.
	 */
	ratbag_device_set_capability(device,
				     RATBAG_DEVICE_CAP_QUERY_CONFIGURATION);

	return device;
}

void
ratbag_device_destroy(struct ratbag_device *device)
{
	struct ratbag_profile *profile, *next;

	if (!device)
		return;

	/* if we get to the point where the device is destroyed, profiles,
	 * buttons, etc. are at a refcount of 0, so we can destroy
	 * everything */
	if (device->driver && device->driver->remove)
		device->driver->remove(device);

	list_for_each_safe(profile, next, &device->profiles, link)
		ratbag_profile_destroy(profile);

	if (device->udev_device)
		udev_device_unref(device->udev_device);

	list_remove(&device->link);

	ratbag_unref(device->ratbag);
	free(device->name);
	free(device);
}

static inline bool
ratbag_sanity_check_device(struct ratbag_device *device)
{
	struct ratbag *ratbag = device->ratbag;
	_cleanup_profile_ struct ratbag_profile *profile = NULL;
	bool has_active = false;
	unsigned int nres, nprofiles;
	bool rc = false;
	unsigned int i;

	/* arbitrary number: max 16 profiles, does any mouse have more? but
	 * since we have num_profiles unsigned, it also checks for
	 * accidental negative */
	if (device->num_profiles == 0 || device->num_profiles > 16) {
		log_bug_libratbag(ratbag,
				  "%s: invalid number of profiles (%d)\n",
				  device->name,
				  device->num_profiles);
		goto out;
	}

	nprofiles = ratbag_device_get_num_profiles(device);
	for (i = 0; i < nprofiles; i++) {
		profile = ratbag_device_get_profile(device, i);
		if (!profile)
			goto out;

		/* Allow max 1 active profile */
		if (profile->is_active) {
			if (has_active) {
				log_bug_libratbag(ratbag,
						  "%s: multiple active profiles\n",
						  device->name);
				goto out;
			}
			has_active = true;
		}

		nres = ratbag_profile_get_num_resolutions(profile);
		if (nres == 0 || nres > 16) {
				log_bug_libratbag(ratbag,
						  "%s: invalid number of resolutions (%d)\n",
						  device->name,
						  nres);
				goto out;
		}

		ratbag_profile_unref(profile);
		profile = NULL;
	}

	/* Require 1 active profile */
	if (!has_active) {
		log_bug_libratbag(ratbag,
				  "%s: no profile set as active profile\n",
				  device->name);
		goto out;
	}

	rc = true;

out:
	return rc;
}

static inline bool
ratbag_test_driver(struct ratbag_device *device,
		   const struct input_id *dev_id,
		   const char *driver_name,
		   struct ratbag_test_device *test_device)
{
	struct ratbag *ratbag = device->ratbag;
	struct ratbag_driver *driver;
	int rc;

	list_for_each(driver, &ratbag->drivers, link) {
		if (streq(driver->id, driver_name)) {
			device->driver = driver;
			break;
		}
	}

	if (!device->driver) {
		log_error(ratbag, "%s: driver '%s' does not exist\n",
			  device->name, driver_name);
		goto error;
	}

	if (test_device)
		rc = device->driver->test_probe(device, test_device);
	else
		rc = device->driver->probe(device);
	if (rc == 0) {
		if (!ratbag_sanity_check_device(device)) {
			goto error;
		} else {
			log_debug(ratbag,
				  "driver match found: %s\n",
				  device->driver->name);
			return true;
		}
	}

	if (rc != -ENODEV)
		log_error(ratbag, "%s: error opening hidraw node (%s)\n",
			  device->name, strerror(-rc));

error:
	device->driver = NULL;

	return false;
}

static inline bool
ratbag_driver_fallback_logitech(struct ratbag_device *device,
				const struct input_id *dev_id)
{
	int rc;

	if (dev_id->vendor != USB_VENDOR_ID_LOGITECH)
		return false;

	rc = ratbag_test_driver(device, dev_id, "hidpp20", NULL);
	if (!rc)
		rc = ratbag_test_driver(device, dev_id, "hidpp10", NULL);

	return rc;
}

bool
ratbag_assign_driver(struct ratbag_device *device,
		     const struct input_id *dev_id,
		     struct ratbag_test_device *test_device)
{
	bool rc;
	const char *driver_name;

	if (!test_device) {
		driver_name = udev_prop_value(device->udev_device, "RATBAG_DRIVER");
	} else {
		driver_name = "test_driver";
	}

	if (driver_name) {
		rc = ratbag_test_driver(device, dev_id, driver_name, test_device);
	} else {
		rc = ratbag_driver_fallback_logitech(device, dev_id);
	}

	return rc;
}

static inline char*
get_device_name(struct udev_device *device)
{
	const char *prop;

	prop = udev_prop_value(device, "NAME");
	if (!prop)
		return NULL;

	/* udev name is enclosed by " */
	return strndup(&prop[1], strlen(prop) - 2);
}

static inline int
get_product_id(struct udev_device *device, struct input_id *id)
{
	const char *product;
	struct input_id ids;
	int rc;

	product = udev_prop_value(device, "PRODUCT");
	if (!product)
		return -1;

	rc = sscanf(product, "%hx/%hx/%hx/%hx", &ids.bustype,
		    &ids.vendor, &ids.product, &ids.version);
	if (rc != 4)
		return -1;

	*id = ids;
	return 0;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_device_new_from_udev_device(struct ratbag *ratbag,
				   struct udev_device *udev_device,
				   struct ratbag_device **device_out)
{
	struct ratbag_device *device = NULL;
	enum ratbag_error_code error = RATBAG_ERROR_DEVICE;
	_cleanup_free_ char *name = NULL;
	struct input_id id;

	assert(ratbag != NULL);
	assert(udev_device != NULL);
	assert(device_out != NULL);

	if (udev_prop_value(udev_device, "ID_INPUT_MOUSE") == NULL)
		goto out_err;

	if (get_product_id(udev_device, &id) != 0)
		goto out_err;

	name = get_device_name(udev_device);
	if (!name)
		goto out_err;

	device = ratbag_device_new(ratbag, udev_device, name, &id);
	if (!device)
		goto out_err;

	if (!ratbag_assign_driver(device, &device->ids, NULL))
		goto out_err;

	error = RATBAG_SUCCESS;

out_err:

	if (error != RATBAG_SUCCESS)
		ratbag_device_destroy(device);
	else
		*device_out = device;

	return error_code(error);
}

LIBRATBAG_EXPORT struct ratbag_device *
ratbag_device_ref(struct ratbag_device *device)
{
	assert(device->refcount < INT_MAX);

	device->refcount++;
	return device;
}

LIBRATBAG_EXPORT struct ratbag_device *
ratbag_device_unref(struct ratbag_device *device)
{
	if (device == NULL)
		return NULL;

	assert(device->refcount > 0);
	device->refcount--;
	if (device->refcount == 0)
		ratbag_device_destroy(device);

	return NULL;
}

LIBRATBAG_EXPORT const char *
ratbag_device_get_name(const struct ratbag_device* device)
{
	return device->name;
}

LIBRATBAG_EXPORT const char *
ratbag_device_get_svg_name(const struct ratbag_device* device)
{
	return udev_prop_value(device->udev_device, "RATBAG_SVG");
}

const char *
ratbag_device_get_udev_property(const struct ratbag_device* device,
				const char *name)
{
	return udev_prop_value(device->udev_device, name);
}

void
ratbag_register_driver(struct ratbag *ratbag, struct ratbag_driver *driver)
{
	if (!driver->name) {
		log_bug_libratbag(ratbag, "Driver is missing name\n");
		return;
	}

	if (!driver->probe || !driver->remove) {
		log_bug_libratbag(ratbag, "Driver %s is incomplete.\n", driver->name);
		return;
	}
	list_insert(&ratbag->drivers, &driver->link);
}

LIBRATBAG_EXPORT struct ratbag *
ratbag_create_context(const struct ratbag_interface *interface,
		      void *userdata)
{
	struct ratbag *ratbag;

	assert(interface != NULL);
	assert(interface->open_restricted != NULL);
	assert(interface->close_restricted != NULL);

	ratbag = zalloc(sizeof(*ratbag));
	ratbag->refcount = 1;
	ratbag->interface = interface;
	ratbag->userdata = userdata;

	list_init(&ratbag->drivers);
	list_init(&ratbag->devices);
	ratbag->udev = udev_new();
	if (!ratbag->udev) {
		free(ratbag);
		return NULL;
	}

	ratbag->log_handler = ratbag_default_log_func;
	ratbag->log_priority = RATBAG_LOG_PRIORITY_INFO;

	ratbag_register_driver(ratbag, &etekcity_driver);
	ratbag_register_driver(ratbag, &hidpp20_driver);
	ratbag_register_driver(ratbag, &hidpp10_driver);
	ratbag_register_driver(ratbag, &roccat_driver);

	return ratbag;
}

LIBRATBAG_EXPORT struct ratbag *
ratbag_ref(struct ratbag *ratbag)
{
	ratbag->refcount++;
	return ratbag;
}

LIBRATBAG_EXPORT struct ratbag *
ratbag_unref(struct ratbag *ratbag)
{
	if (ratbag == NULL)
		return NULL;

	assert(ratbag->refcount > 0);
	ratbag->refcount--;
	if (ratbag->refcount == 0) {
		ratbag->udev = udev_unref(ratbag->udev);
		free(ratbag);
	}

	return NULL;
}

static struct ratbag_button *
ratbag_create_button(struct ratbag_profile *profile, unsigned int index)
{
	struct ratbag_device *device = profile->device;
	struct ratbag_button *button;

	button = zalloc(sizeof(*button));
	button->refcount = 0;
	button->profile = profile;
	button->index = index;

	list_insert(&profile->buttons, &button->link);

	if (device->driver->read_button)
		device->driver->read_button(button);

	return button;
}

static int
ratbag_profile_init_buttons(struct ratbag_profile *profile, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		ratbag_create_button(profile, i);

	profile->device->num_buttons = count;

	return 0;
}

static struct ratbag_profile *
ratbag_create_profile(struct ratbag_device *device,
		      unsigned int index,
		      unsigned int num_resolutions,
		      unsigned int num_buttons)
{
	struct ratbag_profile *profile;
	unsigned i;

	profile = zalloc(sizeof(*profile));
	profile->refcount = 0;
	profile->device = device;
	profile->index = index;
	profile->resolution.modes = zalloc(num_resolutions *
					   sizeof(*profile->resolution.modes));
	profile->resolution.num_modes = num_resolutions;

	list_insert(&device->profiles, &profile->link);
	list_init(&profile->buttons);

	for (i = 0; i < num_resolutions; i++)
		ratbag_resolution_init(profile, i, 0, 0, 0);

	assert(device->driver->read_profile);
	device->driver->read_profile(profile, index);

	ratbag_profile_init_buttons(profile, num_buttons);

	return profile;
}

int
ratbag_device_init_profiles(struct ratbag_device *device,
			    unsigned int num_profiles,
			    unsigned int num_resolutions,
			    unsigned int num_buttons)
{
	unsigned int i;

	for (i = 0; i < num_profiles; i++) {
		ratbag_create_profile(device, i, num_resolutions, num_buttons);
	}

	device->num_profiles = num_profiles;

	if (num_profiles > 1) {
		ratbag_device_set_capability(device, RATBAG_DEVICE_CAP_SWITCHABLE_PROFILE);

		/* having more than one profile means we can remap the buttons
		 * at least */
		ratbag_device_set_capability(device, RATBAG_DEVICE_CAP_BUTTON_KEY);
	}

	if (num_resolutions > 1)
		ratbag_device_set_capability(device, RATBAG_DEVICE_CAP_SWITCHABLE_RESOLUTION);

	return 0;
}

LIBRATBAG_EXPORT struct ratbag_profile *
ratbag_profile_ref(struct ratbag_profile *profile)
{
	assert(profile->refcount < INT_MAX);

	ratbag_device_ref(profile->device);
	profile->refcount++;
	return profile;
}

static void
ratbag_profile_destroy(struct ratbag_profile *profile)
{
	struct ratbag_button *button, *next;

	/* if we get to the point where the profile is destroyed, buttons,
	 * resolutions , etc. are at a refcount of 0, so we can destroy
	 * everything */
	list_for_each_safe(button, next, &profile->buttons, link)
		ratbag_button_destroy(button);

	free(profile->resolution.modes);

	list_remove(&profile->link);
	free(profile);
}

LIBRATBAG_EXPORT struct ratbag_profile *
ratbag_profile_unref(struct ratbag_profile *profile)
{
	if (profile == NULL)
		return NULL;

	assert(profile->refcount > 0);
	profile->refcount--;

	ratbag_device_unref(profile->device);

	return NULL;
}

LIBRATBAG_EXPORT struct ratbag_profile *
ratbag_device_get_profile(struct ratbag_device *device, unsigned int index)
{
	struct ratbag_profile *profile;

	if (index >= ratbag_device_get_num_profiles(device)) {
		log_bug_client(device->ratbag, "Requested invalid profile %d\n", index);
		return NULL;
	}

	list_for_each(profile, &device->profiles, link) {
		if (profile->index == index)
			return ratbag_profile_ref(profile);
	}

	log_bug_libratbag(device->ratbag, "Profile %d not found\n", index);

	return NULL;
}

LIBRATBAG_EXPORT int
ratbag_profile_is_active(struct ratbag_profile *profile)
{
	return profile->is_active;
}

LIBRATBAG_EXPORT unsigned int
ratbag_device_get_num_profiles(struct ratbag_device *device)
{
	return device->num_profiles;
}

LIBRATBAG_EXPORT unsigned int
ratbag_device_get_num_buttons(struct ratbag_device *device)
{
	return device->num_buttons;
}

void
ratbag_device_set_capability(struct ratbag_device *device,
			     enum ratbag_device_capability cap)
{
	device->capabilities |= (1UL << cap);
}

LIBRATBAG_EXPORT int
ratbag_device_has_capability(const struct ratbag_device *device,
			     enum ratbag_device_capability cap)
{
	if (cap == RATBAG_DEVICE_CAP_NONE)
		abort();

	return !!(device->capabilities & (1UL << cap));
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_profile_set_active(struct ratbag_profile *profile)
{
	struct ratbag_device *device = profile->device;
	struct ratbag_profile *p;
	int rc;

	if (!ratbag_device_has_capability(device, RATBAG_DEVICE_CAP_SWITCHABLE_PROFILE))
		return RATBAG_ERROR_CAPABILITY;

	assert(device->driver->write_profile);
	rc = device->driver->write_profile(profile);
	if (rc)
		return RATBAG_ERROR_DEVICE;

	assert(device->driver->set_active_profile);
	rc = device->driver->set_active_profile(device, profile->index);
	if (rc)
		return RATBAG_ERROR_DEVICE;

	list_for_each(p, &device->profiles, link)
		p->is_active = false;

	profile->is_active = true;
	return RATBAG_SUCCESS;
}

LIBRATBAG_EXPORT unsigned int
ratbag_profile_get_num_resolutions(struct ratbag_profile *profile)
{
	return profile->resolution.num_modes;
}

LIBRATBAG_EXPORT struct ratbag_resolution *
ratbag_profile_get_resolution(struct ratbag_profile *profile, unsigned int idx)
{
	struct ratbag_resolution *res;
	unsigned max = ratbag_profile_get_num_resolutions(profile);

	if (idx >= max) {
		log_bug_client(profile->device->ratbag,
			       "Requested invalid resolution %d\n", idx);
		return NULL;
	}

	res = &profile->resolution.modes[idx];

	return ratbag_resolution_ref(res);
}

LIBRATBAG_EXPORT struct ratbag_resolution *
ratbag_resolution_ref(struct ratbag_resolution *resolution)
{
	assert(resolution->refcount < INT_MAX);

	ratbag_profile_ref(resolution->profile);
	resolution->refcount++;
	return resolution;
}

LIBRATBAG_EXPORT struct ratbag_resolution *
ratbag_resolution_unref(struct ratbag_resolution *resolution)
{
	if (resolution == NULL)
		return NULL;

	assert(resolution->refcount > 0);
	resolution->refcount--;

	ratbag_profile_unref(resolution->profile);

	return NULL;
}

LIBRATBAG_EXPORT int
ratbag_resolution_has_capability(struct ratbag_resolution *resolution,
				 enum ratbag_resolution_capability cap)
{
	switch (cap) {
	case RATBAG_RESOLUTION_CAP_INDIVIDUAL_REPORT_RATE:
	case RATBAG_RESOLUTION_CAP_SEPARATE_XY_RESOLUTION:
		break;
	default:
		return 0;
	}

	return !!(resolution->capabilities & (1 << cap));
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_resolution_set_dpi(struct ratbag_resolution *resolution,
			  unsigned int dpi)
{
	struct ratbag_profile *profile = resolution->profile;
	int rc;

	resolution->dpi_x = dpi;
	resolution->dpi_y = dpi;

	assert(profile->device->driver->write_resolution_dpi);
	rc = profile->device->driver->write_resolution_dpi(resolution, dpi, dpi);

	return rc == 0 ? RATBAG_SUCCESS : RATBAG_ERROR_DEVICE;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_resolution_set_dpi_xy(struct ratbag_resolution *resolution,
			     unsigned int x, unsigned int y)
{
	struct ratbag_profile *profile = resolution->profile;
	int rc;

	if (!ratbag_resolution_has_capability(resolution,
					      RATBAG_RESOLUTION_CAP_SEPARATE_XY_RESOLUTION))
		return RATBAG_ERROR_CAPABILITY;

	if ((x == 0 && y != 0) || (x != 0 && y == 0))
		return RATBAG_ERROR_VALUE;

	assert(profile->device->driver->write_resolution_dpi);
	rc = profile->device->driver->write_resolution_dpi(resolution, x, y);
	return rc == 0 ? RATBAG_SUCCESS : RATBAG_ERROR_DEVICE;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_resolution_set_report_rate(struct ratbag_resolution *resolution,
				  unsigned int hz)
{
	resolution->hz = hz;
	/* FIXME: call into the driver */
	return RATBAG_ERROR_IMPLEMENTATION;
}

LIBRATBAG_EXPORT int
ratbag_resolution_get_dpi(struct ratbag_resolution *resolution)
{
	return resolution->dpi_x;
}

LIBRATBAG_EXPORT int
ratbag_resolution_get_dpi_x(struct ratbag_resolution *resolution)
{
	return resolution->dpi_x;
}

LIBRATBAG_EXPORT int
ratbag_resolution_get_dpi_y(struct ratbag_resolution *resolution)
{
	return resolution->dpi_y;
}

LIBRATBAG_EXPORT int
ratbag_resolution_get_report_rate(struct ratbag_resolution *resolution)
{
	return resolution->hz;
}

LIBRATBAG_EXPORT int
ratbag_resolution_is_active(const struct ratbag_resolution *resolution)
{
	return resolution->is_active;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_resolution_set_active(struct ratbag_resolution *resolution)
{
	resolution->is_active = true;
	/* FIXME: call into the driver */
	return RATBAG_ERROR_IMPLEMENTATION;
}


LIBRATBAG_EXPORT int
ratbag_resolution_is_default(const struct ratbag_resolution *resolution)
{
	return resolution->is_default;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_resolution_set_default(struct ratbag_resolution *resolution)
{
	resolution->is_default = true;
	/* FIXME: call into the driver */
	return RATBAG_ERROR_IMPLEMENTATION;
}

LIBRATBAG_EXPORT struct ratbag_button*
ratbag_profile_get_button(struct ratbag_profile *profile,
				   unsigned int index)
{
	struct ratbag_device *device = profile->device;
	struct ratbag_button *button;

	if (index >= ratbag_device_get_num_buttons(device)) {
		log_bug_client(device->ratbag, "Requested invalid button %d\n", index);
		return NULL;
	}

	list_for_each(button, &profile->buttons, link) {
		if (button->index == index)
			return ratbag_button_ref(button);
	}

	log_bug_libratbag(device->ratbag, "Button %d, profile %d not found\n",
			  index, profile->index);

	return NULL;
}

LIBRATBAG_EXPORT enum ratbag_button_type
ratbag_button_get_type(struct ratbag_button *button)
{
	return button->type;
}

LIBRATBAG_EXPORT enum ratbag_button_action_type
ratbag_button_get_action_type(struct ratbag_button *button)
{
	return button->action.type;
}

LIBRATBAG_EXPORT int
ratbag_button_has_action_type(struct ratbag_button *button,
			      enum ratbag_button_action_type action_type)
{
	switch (action_type) {
	case RATBAG_BUTTON_ACTION_TYPE_BUTTON:
	case RATBAG_BUTTON_ACTION_TYPE_SPECIAL:
	case RATBAG_BUTTON_ACTION_TYPE_KEY:
	case RATBAG_BUTTON_ACTION_TYPE_MACRO:
		break;
	default:
		return 0;
	}

	return !!(button->action_caps & (1 << action_type));
}

LIBRATBAG_EXPORT unsigned int
ratbag_button_get_button(struct ratbag_button *button)
{
	if (button->action.type != RATBAG_BUTTON_ACTION_TYPE_BUTTON)
		return 0;

	return button->action.action.button;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_button_set_button(struct ratbag_button *button, unsigned int btn)
{
	struct ratbag_button_action action = {0};
	int rc;

	if (!button->profile->device->driver->write_button)
		return RATBAG_ERROR_CAPABILITY;

	action.type = RATBAG_BUTTON_ACTION_TYPE_BUTTON;
	action.action.button = btn;

	rc = button->profile->device->driver->write_button(button, &action);

	if (rc == 0)
		button->action = action;

	return rc == 0 ? RATBAG_SUCCESS : RATBAG_ERROR_DEVICE;
}

LIBRATBAG_EXPORT enum ratbag_button_action_special
ratbag_button_get_special(struct ratbag_button *button)
{
	if (button->action.type != RATBAG_BUTTON_ACTION_TYPE_SPECIAL)
		return RATBAG_BUTTON_ACTION_SPECIAL_INVALID;

	return button->action.action.special;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_button_set_special(struct ratbag_button *button,
			  enum ratbag_button_action_special act)
{
	struct ratbag_button_action action = {0};
	int rc;

	/* FIXME: range checks */

	if (!button->profile->device->driver->write_button)
		return RATBAG_ERROR_CAPABILITY;

	action.type = RATBAG_BUTTON_ACTION_TYPE_SPECIAL;
	action.action.special = act;

	rc = button->profile->device->driver->write_button(button, &action);

	if (rc == 0)
		button->action = action;

	return rc == 0 ? RATBAG_SUCCESS : RATBAG_ERROR_DEVICE;
}

LIBRATBAG_EXPORT unsigned int
ratbag_button_get_key(struct ratbag_button *button,
		      unsigned int *modifiers,
		      size_t *sz)
{
	if (button->action.type != RATBAG_BUTTON_ACTION_TYPE_KEY)
		return 0;

	/* FIXME: modifiers */
	*sz = 0;
	return button->action.action.key.key;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_button_set_key(struct ratbag_button *button,
		      unsigned int key,
		      unsigned int *modifiers,
		      size_t sz)
{
	struct ratbag_button_action action = {0};
	int rc;

	/* FIXME: range checks */

	if (!button->profile->device->driver->write_button)
		return RATBAG_ERROR_CAPABILITY;

	/* FIXME: modifiers */

	action.type = RATBAG_BUTTON_ACTION_TYPE_KEY;
	action.action.key.key = key;
	rc = button->profile->device->driver->write_button(button, &action);

	if (rc == 0)
		button->action = action;

	return rc == 0 ? RATBAG_SUCCESS : RATBAG_ERROR_DEVICE;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_button_disable(struct ratbag_button *button)
{
	struct ratbag_button_action action;
	int rc;

	if (!button->profile->device->driver->write_button)
		return RATBAG_ERROR_CAPABILITY;

	action.type = RATBAG_BUTTON_ACTION_TYPE_NONE;
	rc = button->profile->device->driver->write_button(button, &action);

	if (rc == 0)
		button->action = action;

	return rc == 0 ? RATBAG_SUCCESS : RATBAG_ERROR_DEVICE;
}

LIBRATBAG_EXPORT struct ratbag_button *
ratbag_button_ref(struct ratbag_button *button)
{
	assert(button->refcount < INT_MAX);

	ratbag_profile_ref(button->profile);
	button->refcount++;
	return button;
}

static void
ratbag_button_destroy(struct ratbag_button *button)
{
	list_remove(&button->link);
	if (button->action.macro) {
		if (button->action.macro->name)
			free(button->action.macro->name);
		free(button->action.macro);
	}
	free(button);
}

LIBRATBAG_EXPORT struct ratbag_button *
ratbag_button_unref(struct ratbag_button *button)
{
	if (button == NULL)
		return NULL;

	assert(button->refcount > 0);
	button->refcount--;

	ratbag_profile_unref(button->profile);

	return NULL;
}

LIBRATBAG_EXPORT void
ratbag_set_user_data(struct ratbag *ratbag, void *userdata)
{
	ratbag->userdata = userdata;
}

LIBRATBAG_EXPORT void
ratbag_device_set_user_data(struct ratbag_device *ratbag_device, void *userdata)
{
	ratbag_device->userdata = userdata;
}

LIBRATBAG_EXPORT void
ratbag_profile_set_user_data(struct ratbag_profile *ratbag_profile, void *userdata)
{
	ratbag_profile->userdata = userdata;
}

LIBRATBAG_EXPORT void
ratbag_button_set_user_data(struct ratbag_button *ratbag_button, void *userdata)
{
	ratbag_button->userdata = userdata;
}

LIBRATBAG_EXPORT void
ratbag_resolution_set_user_data(struct ratbag_resolution *ratbag_resolution, void *userdata)
{
	ratbag_resolution->userdata = userdata;
}

LIBRATBAG_EXPORT void*
ratbag_get_user_data(const struct ratbag *ratbag)
{
	return ratbag->userdata;
}

LIBRATBAG_EXPORT void*
ratbag_device_get_user_data(const struct ratbag_device *ratbag_device)
{
	return ratbag_device->userdata;
}

LIBRATBAG_EXPORT void*
ratbag_profile_get_user_data(const struct ratbag_profile *ratbag_profile)
{
	return ratbag_profile->userdata;
}

LIBRATBAG_EXPORT void*
ratbag_button_get_user_data(const struct ratbag_button *ratbag_button)
{
	return ratbag_button->userdata;
}

LIBRATBAG_EXPORT void*
ratbag_resolution_get_user_data(const struct ratbag_resolution *ratbag_resolution)
{
	return ratbag_resolution->userdata;
}

LIBRATBAG_EXPORT struct ratbag_button_macro *
ratbag_button_get_macro(struct ratbag_button *button)
{
	struct ratbag_button_macro *macro;

	if (button->action.type != RATBAG_BUTTON_ACTION_TYPE_MACRO)
		return NULL;

	macro = ratbag_button_macro_new(button->action.macro->name);
	memcpy(macro->macro.events,
	       button->action.macro->events,
	       sizeof(macro->macro.events));

	return macro;
}

void
ratbag_button_copy_macro(struct ratbag_button *button,
			 const struct ratbag_button_macro *macro)
{
	if (!button->action.macro)
		button->action.macro = zalloc(sizeof(struct ratbag_macro));
	else {
		if (button->action.macro->name)
			free(button->action.macro->name);
		memset(button->action.macro, 0, sizeof(struct ratbag_macro));
	}

	button->action.type = RATBAG_BUTTON_ACTION_TYPE_MACRO;
	memcpy(button->action.macro->events,
	       macro->macro.events,
	       sizeof(macro->macro.events));
	button->action.macro->name = strdup_safe(macro->macro.name);
	button->action.macro->group = strdup_safe(macro->macro.group);
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_button_set_macro(struct ratbag_button *button,
			const struct ratbag_button_macro *macro)
{
	int rc;

	ratbag_button_copy_macro(button, macro);

	rc = button->profile->device->driver->write_button(button, &button->action);

	return rc == 0 ? RATBAG_SUCCESS : RATBAG_ERROR_DEVICE;
}

LIBRATBAG_EXPORT enum ratbag_error_code
ratbag_button_macro_set_event(struct ratbag_button_macro *m,
			      unsigned int index,
			      enum ratbag_macro_event_type type,
			      unsigned int data)
{
	struct ratbag_macro *macro = &m->macro;

	if (index >= MAX_MACRO_EVENTS)
		return RATBAG_ERROR_VALUE;

	switch (type) {
	case RATBAG_MACRO_EVENT_KEY_PRESSED:
	case RATBAG_MACRO_EVENT_KEY_RELEASED:
		macro->events[index].type = type;
		macro->events[index].event.key = data;
		break;
	case RATBAG_MACRO_EVENT_WAIT:
		macro->events[index].type = type;
		macro->events[index].event.timeout = data;
		break;
	case RATBAG_MACRO_EVENT_NONE:
		macro->events[index].type = type;
		break;
	default:
		return RATBAG_ERROR_VALUE;
	}

	return 0;
}

LIBRATBAG_EXPORT enum ratbag_macro_event_type
ratbag_button_macro_get_event_type(struct ratbag_button_macro *macro, unsigned int index)
{
	if (index >= MAX_MACRO_EVENTS)
		return RATBAG_MACRO_EVENT_INVALID;

	return macro->macro.events[index].type;
}

LIBRATBAG_EXPORT int
ratbag_button_macro_get_event_key(struct ratbag_button_macro *m, unsigned int index)
{
	struct ratbag_macro *macro = &m->macro;

	if (index >= MAX_MACRO_EVENTS)
		return 0;

	if (macro->events[index].type != RATBAG_MACRO_EVENT_KEY_PRESSED &&
	    macro->events[index].type != RATBAG_MACRO_EVENT_KEY_RELEASED)
		return -EINVAL;

	return macro->events[index].event.key;
}

LIBRATBAG_EXPORT int
ratbag_button_macro_get_event_timeout(struct ratbag_button_macro *m,
				      unsigned int index)
{
	struct ratbag_macro *macro = &m->macro;

	if (index >= MAX_MACRO_EVENTS)
		return 0;

	if (macro->events[index].type != RATBAG_MACRO_EVENT_WAIT)
		return 0;

	return macro->events[index].event.timeout;
}

LIBRATBAG_EXPORT unsigned int
ratbag_button_macro_get_num_events(struct ratbag_button_macro *macro)
{
	return MAX_MACRO_EVENTS;
}

LIBRATBAG_EXPORT const char *
ratbag_button_macro_get_name(struct ratbag_button_macro *macro)
{
	return macro->macro.name;
}

static void
ratbag_button_macro_destroy(struct ratbag_button_macro *macro)
{
	assert(macro->refcount == 0);
	free(macro->macro.name);
	free(macro->macro.group);
	free(macro);
}

LIBRATBAG_EXPORT struct ratbag_button_macro *
ratbag_button_macro_ref(struct ratbag_button_macro *macro)
{
	assert(macro->refcount < INT_MAX);

	macro->refcount++;
	return macro;
}

LIBRATBAG_EXPORT struct ratbag_button_macro *
ratbag_button_macro_unref(struct ratbag_button_macro *macro)
{
	if (macro == NULL)
		return NULL;

	assert(macro->refcount > 0);
	macro->refcount--;
	if (macro->refcount == 0)
		ratbag_button_macro_destroy(macro);

	return NULL;
}

LIBRATBAG_EXPORT struct ratbag_button_macro *
ratbag_button_macro_new(const char *name)
{
	struct ratbag_button_macro *macro;

	macro = zalloc(sizeof *macro);
	macro->refcount = 1;
	macro->macro.name = strdup_safe(name);

	return macro;
}
