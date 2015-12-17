/*
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
*/

/*
 * An example of using libtcmu to back one or more types of LIO
 * userspace passthrough devices.
 */

#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <endian.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <poll.h>

#include <stdint.h>
#include <scsi/scsi.h>
#include <errno.h>
#define _BITS_UIO_H
#include <linux/target_core_user.h>
#include "libtcmu.h"

/*
 * Debug API implementation
 */
void dbgp(const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	vprintf(fmt, va);
	va_end(va);
}

void errp(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
}

struct tcmu_device *tcmu_dev_array[128];
size_t dev_array_len = 0;

struct foo_state {
	int fd;
	uint64_t num_lbas;
	uint32_t block_size;
};

static int set_medium_error(uint8_t *sense)
{
	return tcmu_set_sense_data(sense, MEDIUM_ERROR, ASC_READ_ERROR, NULL);
}

static bool foo_check_config(const char *cfgstring, char **reason)
{
	dbgp("checking: %s\n", cfgstring);
	return true;
}

static int foo_open(struct tcmu_device *dev)
{
	struct foo_state *state;

        /* alloc private struct 'foo_state' and stash away disk info */
        state = malloc(sizeof(*state));
	if (!state)
		return -ENOMEM;
	state->block_size = tcmu_get_attribute(dev, "hw_block_size");
	state->num_lbas = tcmu_get_device_size(dev) / state->block_size;

	dbgp("openning: %s\n", tcmu_get_dev_cfgstring(dev));
	
	/* open the backing file */
	state->fd = open("storage", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	if (state->fd == -1) {
		errp("could not open file: error=%u\n", errno);
		free(state);
		return -EINVAL;
	}
	
	/* Save a ptr to it in dev->hm_private */
	tcmu_set_dev_private(dev, state);
	
	/* Add new device to our horrible fixed-length array */
	tcmu_dev_array[dev_array_len] = dev;
	dev_array_len++;

	return 0;
}

static void foo_close(struct tcmu_device *dev)
{
	/* not supported in this example */
}

static int foo_handle_cmd(
	struct tcmu_device *dev,
	uint8_t *cdb,
	struct iovec *iovec,
	size_t iov_cnt,
	uint8_t *sense)
{
	struct foo_state *state = tcmu_get_dev_private(dev);
	uint8_t cmd;

	cmd = cdb[0];

	switch (cmd) {
	case INQUIRY:
		return tcmu_emulate_inquiry(dev, cdb, iovec, iov_cnt, sense);
		break;
	case TEST_UNIT_READY:
		return tcmu_emulate_test_unit_ready(cdb, iovec, iov_cnt, sense);
		break;
	case SERVICE_ACTION_IN_16:
		if (cdb[1] == READ_CAPACITY_16)
			return tcmu_emulate_read_capacity_16(state->num_lbas,
							     state->block_size,
							     cdb, iovec, iov_cnt, sense);
		else
			return TCMU_NOT_HANDLED;
		break;
	case MODE_SENSE:
	case MODE_SENSE_10:
		return tcmu_emulate_mode_sense(cdb, iovec, iov_cnt, sense);
		break;
	case MODE_SELECT:
	case MODE_SELECT_10:
		return tcmu_emulate_mode_select(cdb, iovec, iov_cnt, sense);
		break;
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		// A real "read" implementation goes here!
		return set_medium_error(sense);

	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
		// A real "write" implemention goes here!
		return SAM_STAT_GOOD;

	default:
		errp("unknown command %x\n", cdb[0]);
		return TCMU_NOT_HANDLED;
	}
}

static struct tcmulib_handler foo_handler = {
	.name = "File-backed Handler (example code)",
	.subtype = "file",
	.cfg_desc = "The path to the file to use as a backstore.",

	.check_config = foo_check_config,

	.added = foo_open,
	.removed = foo_close,
};

int main(int argc, char **argv)
{
	struct tcmulib_context *tcmulib_cxt;
	struct pollfd pollfds[16];
	int i;
	int ret;

	/* If any TCMU devices that exist that match subtype,
	   handler->added() will now be called from within
	   tcmulib_initialize(). */
	tcmulib_cxt = tcmulib_initialize(&foo_handler, 1, errp);
	if (tcmulib_cxt <= 0) {
		errp("tcmulib_initialize failed with %p\n", tcmulib_cxt);
		exit(1);
	}

	while (1) {
		pollfds[0].fd = tcmulib_get_master_fd(tcmulib_cxt);
		pollfds[0].events = POLLIN;
		pollfds[0].revents = 0;

		for (i = 0; i < dev_array_len; i++) {
			pollfds[i+1].fd = tcmu_get_dev_fd(tcmu_dev_array[i]);
			pollfds[i+1].events = POLLIN;
			pollfds[i+1].revents = 0;
		}
		
		ret = poll(pollfds, dev_array_len+1, -1);

		if (ret <= 0) {
			errp("poll() returned %d, exiting\n", ret);
			exit(1);
		}

		if (pollfds[0].revents) {
			/* If any tcmu devices have been added or removed, the
			   added() and removed() handler callbacks will be called
			   from within this. */
			tcmulib_master_fd_ready(tcmulib_cxt);

			/* Since devices (may) have changed, re-poll() instead of
			   processing per-device fds. */
			continue;
		}

		for (i = 0; i < dev_array_len; i++) {
			int completed = 0;
		  
			if (pollfds[i+1].revents) {
				struct tcmulib_cmd cmd;
				struct tcmu_device *dev = tcmu_dev_array[i];

				while (tcmulib_get_next_command(dev, &cmd)) {
					ret = foo_handle_cmd(dev,
							     cmd.cdb,
							     cmd.iovec,
							     cmd.iov_cnt,
							     cmd.sense_buf);
					if (ret != TCMU_ASYNC_HANDLED) {
						tcmulib_command_complete(dev, &cmd, ret);
						completed = 1;
					}
				}

				if (completed)
					tcmulib_processing_complete(dev);
			}
		}
	}

	return 0;
}
