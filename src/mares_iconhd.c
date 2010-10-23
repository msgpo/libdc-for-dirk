/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "device-private.h"
#include "mares_iconhd.h"
#include "serial.h"
#include "array.h"
#include "utils.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#ifdef _WIN32
#define BAUDRATE 256000
#else
#define BAUDRATE 230400
#endif

typedef struct mares_iconhd_device_t {
	device_t base;
	serial_t *port;
} mares_iconhd_device_t;

static device_status_t mares_iconhd_device_dump (device_t *abstract, dc_buffer_t *buffer);
static device_status_t mares_iconhd_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t mares_iconhd_device_close (device_t *abstract);

static const device_backend_t mares_iconhd_device_backend = {
	DEVICE_TYPE_MARES_ICONHD,
	NULL, /* set_fingerprint */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	mares_iconhd_device_dump, /* dump */
	mares_iconhd_device_foreach, /* foreach */
	mares_iconhd_device_close /* close */
};

static int
device_is_mares_iconhd (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &mares_iconhd_device_backend;
}


device_status_t
mares_iconhd_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) malloc (sizeof (mares_iconhd_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &mares_iconhd_device_backend);

	// Set the default values.
	device->port = NULL;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (256000 8N1).
	rc = serial_configure (device->port, BAUDRATE, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR/RTS lines.
	if (serial_set_dtr (device->port, 0) == -1 ||
		serial_set_rts (device->port, 0) == -1)
	{
		WARNING ("Failed to set the DTR/RTS line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t *) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_iconhd_device_close (device_t *abstract)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t*) abstract;

	if (! device_is_mares_iconhd (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Free memory.
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_iconhd_init (mares_iconhd_device_t *device)
{
	// Send the command to the dive computer.
	unsigned char command[2] = {0xE7, 0x42};
	int n = serial_write (device->port, command, sizeof (command));
	if (n != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	// Receive the answer of the dive computer.
	unsigned char answer[1] = {0};
	n = serial_read (device->port, answer, sizeof (answer));
	if (n != sizeof (answer)) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_iconhd_device_dump (device_t *abstract, dc_buffer_t *buffer)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	// Erase the current contents of the buffer and
	// pre-allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, MARES_ICONHD_MEMORY_SIZE)) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = MARES_ICONHD_MEMORY_SIZE;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Send the init command.
	device_status_t rc = mares_iconhd_init (device);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the command to the dive computer.
	unsigned char command[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00};
	int n = serial_write (device->port, command, sizeof (command));
	if (n != sizeof (command)) {
		WARNING ("Failed to send the command.");
		return EXITCODE (n);
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	unsigned int nbytes = 0;
	while (nbytes < MARES_ICONHD_MEMORY_SIZE) {
		// Set the minimum packet size.
		unsigned int len = 1024;

		// Increase the packet size if more data is immediately available.
		int available = serial_get_received (device->port);
		if (available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > MARES_ICONHD_MEMORY_SIZE)
			len = MARES_ICONHD_MEMORY_SIZE - nbytes;

		// Read the packet.
		n = serial_read (device->port, data + nbytes, len);
		if (n != len) {
			WARNING ("Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	// Receive the last byte.
	unsigned char answer[1] = {0};
	n = serial_read (device->port, answer, sizeof (answer));
	if (n != sizeof (answer)) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (n);
	}

	// Verify the last byte.
	if (answer[0] != 0xEA) {
		WARNING ("Unexpected answer byte.");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
mares_iconhd_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (MARES_ICONHD_MEMORY_SIZE);
	if (buffer == NULL)
		return DEVICE_STATUS_MEMORY;

	device_status_t rc = mares_iconhd_device_dump (abstract, buffer);
	if (rc != DEVICE_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	// Emit a device info event.
	unsigned char *data = dc_buffer_get_data (buffer);
	device_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_uint16_le (data + 12);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	rc = mares_iconhd_extract_dives (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


device_status_t
mares_iconhd_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	if (abstract && !device_is_mares_iconhd (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < MARES_ICONHD_MEMORY_SIZE)
		return DEVICE_STATUS_ERROR;

	unsigned int ndives = 0;
	unsigned int offset = 0xA000;
	while (offset + 4 <= MARES_ICONHD_MEMORY_SIZE) {
		// Get the length of the profile data.
		unsigned int len = array_uint32_le (data + offset);
		if (len == 0xFFFFFFFF)
			break;

		offset += len;
		ndives++;
	}

	for (unsigned int i = 0; i < ndives; ++i) {
		// Skip the older dives.
		unsigned int offset = 0xA000;
		unsigned int skip = ndives - i - 1;
		while (skip) {
			// Get the length of the profile data.
			unsigned int len = array_uint32_le (data + offset);
			// Move to the next dive.
			offset += len;
			skip--;
		}

		// Get the length of the profile data.
		unsigned int length = array_uint32_le (data + offset);

		if (callback && !callback (data + offset, length, NULL, 0, userdata))
			return DEVICE_STATUS_SUCCESS;
	}

	return DEVICE_STATUS_SUCCESS;
}