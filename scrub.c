/*
 * Copyright (C) 2013 Andrea Mazzoleni
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "portable.h"

#include "util.h"
#include "elem.h"
#include "state.h"
#include "parity.h"
#include "handle.h"
#include "raid.h"

/****************************************************************************/
/* scrub */

static int state_scrub_process(struct snapraid_state* state, struct snapraid_parity* parity, struct snapraid_parity* qarity, block_off_t blockstart, block_off_t blockmax, time_t timelimit, block_off_t countlimit, time_t now)
{
	struct snapraid_handle* handle;
	unsigned diskmax;
	block_off_t i;
	unsigned j;
	void* buffer_alloc;
	unsigned char* buffer_aligned;
	unsigned char** buffer;
	unsigned buffermax;
	data_off_t countsize;
	block_off_t countpos;
	block_off_t countmax;
	block_off_t blocklimit;
	block_off_t autosavedone;
	block_off_t autosavelimit;
	block_off_t autosavemissing;
	int ret;
	unsigned error;

	/* maps the disks to handles */
	handle = handle_map(state, &diskmax);

	/* we need disk + 2 for each parity level buffers */
	buffermax = diskmax + state->level * 2;

	buffer_aligned = malloc_nofail_align(buffermax * state->block_size, &buffer_alloc);
	buffer = malloc_nofail(buffermax * sizeof(void*));
	for(i=0;i<buffermax;++i) {
		buffer[i] = buffer_aligned + i * state->block_size;
	}

	error = 0;

	/* first count the number of blocks to process */
	countmax = 0;
	blocklimit = blockmax; /* block address for which we should stop */
	for(i=blockstart;i<blockmax;++i) {
		time_t blocktime;
		snapraid_info info;

		/* if it's unused */
		info = info_get(&state->infoarr, i);
		if (info == 0) {
			/* skip it */
			continue;
		}

		/* if it's too new */
		blocktime = info_get_time(info);
		if (blocktime > timelimit) {
			/* skip it */
			continue;
		}
		if (countmax >= countlimit) {
			blocklimit = i;
			break;
		}

		++countmax;
	}

	/* compute the autosave size for all disk, even if not read */
	/* this makes sense because the speed should be almost the same */
	/* if the disks are read in parallel */
	autosavelimit = state->autosave / (diskmax * state->block_size);
	autosavemissing = countmax; /* blocks to do */
	autosavedone = 0; /* blocks done */

	countsize = 0;
	countpos = 0;
	state_progress_begin(state, blockstart, blockmax, countmax);
	for(i=blockstart;i<blocklimit;++i) {
		time_t blocktime;
		snapraid_info info;
		int error_on_this_block;
		int silent_error_on_this_block;
		int ret;

		/* if it's unused */
		info = info_get(&state->infoarr, i);
		if (info == 0) {
			/* skip it */
			continue;
		}

		/* if it's too new */
		blocktime = info_get_time(info);
		if (blocktime > timelimit) {
			/* skip it */
			continue;
		}

		/* one more block processed for autosave */
		++autosavedone;
		--autosavemissing;

		/* by default process the block, and skip it if something go wrong */
		error_on_this_block = 0;
		silent_error_on_this_block = 0;

		/* for each disk, process the block */
		for(j=0;j<diskmax;++j) {
			int read_size;
			unsigned char hash[HASH_SIZE];
			struct snapraid_block* block;

			/* if the disk position is not used */
			if (!handle[j].disk) {
				/* use an empty block */
				memset(buffer[j], 0, state->block_size);
				continue;
			}

			/* if the block is not used */
			block = disk_block_get(handle[j].disk, i);
			if (!block_has_file(block)) {
				/* use an empty block */
				memset(buffer[j], 0, state->block_size);
				continue;
			}

			/* if the file is different than the current one, close it */
			if (handle[j].file != block_file_get(block)) {
				ret = handle_close(&handle[j]);
				if (ret == -1) {
					/* This one is really an unexpected error, because we are only reading */
					/* and closing a descriptor should never fail */
					fprintf(stderr, "DANGER! Unexpected close error in a data disk, it isn't possible to scrub.\n");
					printf("Stopping at block %u\n", i);
					++error;
					goto bail;
				}
			}

			ret = handle_open(&handle[j], block_file_get(block), stderr, state->skip_sequential);
			if (ret == -1) {
				fprintf(stdlog, "error:%u:%s:%s: Open error at position %u\n", i, handle[j].disk->name, block_file_get(block)->sub, block_file_pos(block));
				++error;
				error_on_this_block = 1;
				continue;
			}

			read_size = handle_read(&handle[j], block, buffer[j], state->block_size, stderr);
			if (read_size == -1) {
				fprintf(stdlog, "error:%u:%s:%s: Read error at position %u\n", i, handle[j].disk->name, block_file_get(block)->sub, block_file_pos(block));
				++error;
				error_on_this_block = 1;
				continue;
			}

			countsize += read_size;

			/* now compute the hash */
			memhash(state->hash, state->hashseed, hash, buffer[j], read_size);

			if (block_has_hash(block)) {
				/* compare the hash */
				if (memcmp(hash, block->hash, HASH_SIZE) != 0) {
					fprintf(stdlog, "error:%u:%s:%s: Data error at position %u\n", i, handle[j].disk->name, block_file_get(block)->sub, block_file_pos(block));
					++error;
					silent_error_on_this_block = 1;
					continue;
				}
			}
		}

		/* if we have read all the data required, proceed with the parity */
		if (!error_on_this_block && !silent_error_on_this_block) {
			unsigned char* buffer_parity;
			unsigned char* buffer_qarity;

			/* buffers for parity read and not computed */
			if (state->level == 1) {
				buffer_parity = buffer[diskmax + 1];
				buffer_qarity = 0;
			} else {
				buffer_parity = buffer[diskmax + 2];
				buffer_qarity = buffer[diskmax + 3];
			}

			/* read the parity */
			ret = parity_read(parity, i, buffer_parity, state->block_size, stdlog);
			if (ret == -1) {
				buffer_parity = 0;
				fprintf(stdlog, "error:%u:parity: Read error\n", i);
				++error;
				error_on_this_block = 1;
			}

			/* read the qarity */
			if (state->level >= 2) {
				ret = parity_read(qarity, i, buffer_qarity, state->block_size, stdlog);
				if (ret == -1) {
					buffer_qarity = 0;
					fprintf(stdlog, "error:%u:qarity: Read error\n", i);
					++error;
					error_on_this_block = 1;
				}
			}

			/* compute the parity */
			raid_gen(state->level, buffer, diskmax, state->block_size);

			/* compare the parity */
			if (buffer_parity && memcmp(buffer[diskmax], buffer_parity, state->block_size) != 0) {
				fprintf(stdlog, "error:%u:parity: Data error\n", i);
				++error;
				silent_error_on_this_block = 1;
			}
			if (state->level >= 2) {
				if (buffer_qarity && memcmp(buffer[diskmax + 1], buffer_qarity, state->block_size) != 0) {
					fprintf(stdlog, "error:%u:qarity: Data error\n", i);
					++error;
					silent_error_on_this_block = 1;
				}
			}
		}

		if (error_on_this_block) {
			/* do nothing, as this is a generic error */
			/* maybe just caused by a not synched array */
		} else if (silent_error_on_this_block) {
			/* set the error status keeping the existing time */
			snapraid_info info = info_get(&state->infoarr, i);

			info_set(&state->infoarr, i, info_set_error(info));
		} else {
			/* update the time info of the block */
			info_set(&state->infoarr, i, info_make(now, 0));
		}

		/* mark the state as needing write */
		state->need_write = 1;

		/* count the number of processed block */
		++countpos;

		/* progress */
		if (state_progress(state, i, countpos, countmax, countsize)) {
			break;
		}

		/* autosave */
		if (state->autosave != 0
			&& autosavedone >= autosavelimit /* if we have reached the limit */
			&& autosavemissing >= autosavelimit /* if we have at least a full step to do */
		) {
			autosavedone = 0; /* restart the counter */

			state_progress_stop(state);

			printf("Autosaving...\n");
			state_write(state);

			state_progress_restart(state);
		}
	}

	state_progress_end(state, countpos, countmax, countsize);

bail:
	for(j=0;j<diskmax;++j) {
		ret = handle_close(&handle[j]);
		if (ret == -1) {
			fprintf(stderr, "DANGER! Unexpected close error in a data disk.\n");
			++error;
			/* continue, as we are already exiting */
		}
	}

	free(handle);
	free(buffer_alloc);
	free(buffer);

	if (error != 0)
		return -1;
	return 0;
}

int state_scrub(struct snapraid_state* state)
{
	block_off_t blockmax;
	block_off_t countlimit;
	block_off_t i;
	time_t timelimit;
	time_t recentlimit;
	unsigned count;
	int ret;
	struct snapraid_parity parity;
	struct snapraid_parity qarity;
	struct snapraid_parity* parity_ptr;
	struct snapraid_parity* qarity_ptr;
	snapraid_info* infomap;
	unsigned error;
	time_t now;

	/* get the present time */
	now = time(0);

	printf("Initializing...\n");

	blockmax = parity_size(state);

	/* by default scrub 1/12 of the array */
	countlimit = blockmax / 12;

	/* by default use a 10 day time limit */
	recentlimit = now - 10 * 24 * 3600;

	/* identify the time limit */
	/* we sort all the block times, and we identify the time limit for which we reach the quota */
	/* this allow to process first the oldest blocks */
	infomap = malloc_nofail(blockmax * sizeof(snapraid_info));

	/* copy the info in the temp vector */
	count = 0;
	for(i=0;i<blockmax;++i) {
		snapraid_info info = info_get(&state->infoarr, i);

		/* skip unused blocks */
		if (info == 0)
			continue;

		infomap[count++] = info;
	}

	if (!count) {
		fprintf(stderr, "The array appears to be empty.\n");
		exit(EXIT_FAILURE);
	}

	/* sort it */
	qsort(infomap, count, sizeof(snapraid_info), info_time_compare);

	/* don't check more block than the available ones */
	if (countlimit >= count)
		countlimit = count - 1;

	/* get the time limit */
	timelimit = info_get_time(infomap[countlimit]);

	/* don't scrub too recent blocks */
	if (timelimit > recentlimit)
		timelimit = recentlimit;

	/* free the temp vector */
	free(infomap);

	/* open the file for reading */
	parity_ptr = &parity;
	ret = parity_open(parity_ptr, state->parity, state->skip_sequential);
	if (ret == -1) {
		fprintf(stderr, "WARNING! Without an accessible Parity file, it isn't possible to scrub.\n");
		exit(EXIT_FAILURE);
	}

	if (state->level >= 2) {
		qarity_ptr = &qarity;
		ret = parity_open(qarity_ptr, state->qarity, state->skip_sequential);
		if (ret == -1) {
			fprintf(stderr, "WARNING! Without an accessible Q-Parity file, it isn't possible to scrub.\n");
			exit(EXIT_FAILURE);
		}
	} else {
		qarity_ptr = 0;
	}

	printf("Scrubbing...\n");

	error = 0;

	ret = state_scrub_process(state, parity_ptr, qarity_ptr, 0, blockmax, timelimit, countlimit, now);
	if (ret == -1) {
		++error;
		/* continue, as we are already exiting */
	}

	ret = parity_close(parity_ptr);
	if (ret == -1) {
		fprintf(stderr, "DANGER! Unexpected close error in Parity disk.\n");
		++error;
		/* continue, as we are already exiting */
	}

	if (state->level >= 2) {
		ret = parity_close(qarity_ptr);
		if (ret == -1) {
			fprintf(stderr, "DANGER! Unexpected close error in Q-Parity disk.\n");
			++error;
			/* continue, as we are already exiting */
		}
	}

	/* abort if required */
	if (error != 0)
		return -1;
	return 0;
}

