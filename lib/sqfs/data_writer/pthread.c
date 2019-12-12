/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * pthread.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#define SQFS_BUILDING_DLL
#include "internal.h"

struct compress_worker_t {
	sqfs_data_writer_t *shared;
	sqfs_compressor_t *cmp;
	pthread_t thread;
	sqfs_u8 scratch[];
};

static void *worker_proc(void *arg)
{
	compress_worker_t *worker = arg;
	sqfs_data_writer_t *shared = worker->shared;
	sqfs_block_t *blk = NULL;
	int status = 0;

	for (;;) {
		pthread_mutex_lock(&shared->mtx);
		if (blk != NULL) {
			data_writer_store_done(shared, blk, status);
			pthread_cond_broadcast(&shared->done_cond);
		}

		while (shared->queue == NULL && shared->status == 0)
			pthread_cond_wait(&shared->queue_cond, &shared->mtx);

		blk = data_writer_next_work_item(shared);
		pthread_mutex_unlock(&shared->mtx);

		if (blk == NULL)
			break;

		status = data_writer_do_block(blk, worker->cmp,
					      worker->scratch,
					      shared->max_block_size);
	}
	return NULL;
}

sqfs_data_writer_t *sqfs_data_writer_create(size_t max_block_size,
					    sqfs_compressor_t *cmp,
					    unsigned int num_workers,
					    size_t max_backlog,
					    size_t devblksz,
					    sqfs_file_t *file)
{
	sqfs_data_writer_t *proc;
	sigset_t set, oldset;
	unsigned int i;
	int ret;

	if (num_workers < 1)
		num_workers = 1;

	proc = alloc_flex(sizeof(*proc),
			  sizeof(proc->workers[0]), num_workers);
	if (proc == NULL)
		return NULL;

	proc->mtx = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	proc->queue_cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	proc->done_cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;

	if (data_writer_init(proc, max_block_size, cmp, num_workers,
			     max_backlog, devblksz, file)) {
		goto fail_init;
	}

	for (i = 0; i < num_workers; ++i) {
		proc->workers[i] = alloc_flex(sizeof(compress_worker_t),
					      1, max_block_size);

		if (proc->workers[i] == NULL)
			goto fail_init;

		proc->workers[i]->shared = proc;
		proc->workers[i]->cmp = cmp->create_copy(cmp);

		if (proc->workers[i]->cmp == NULL)
			goto fail_init;
	}

	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, &oldset);

	for (i = 0; i < num_workers; ++i) {
		ret = pthread_create(&proc->workers[i]->thread, NULL,
				     worker_proc, proc->workers[i]);

		if (ret != 0)
			goto fail_thread;
	}

	pthread_sigmask(SIG_SETMASK, &oldset, NULL);

	return proc;
fail_thread:
	pthread_mutex_lock(&proc->mtx);
	proc->status = -1;
	pthread_cond_broadcast(&proc->queue_cond);
	pthread_mutex_unlock(&proc->mtx);

	for (i = 0; i < num_workers; ++i) {
		if (proc->workers[i]->thread > 0) {
			pthread_join(proc->workers[i]->thread, NULL);
		}
	}
	pthread_sigmask(SIG_SETMASK, &oldset, NULL);
fail_init:
	for (i = 0; i < num_workers; ++i) {
		if (proc->workers[i] != NULL) {
			if (proc->workers[i]->cmp != NULL) {
				proc->workers[i]->cmp->
					destroy(proc->workers[i]->cmp);
			}

			free(proc->workers[i]);
		}
	}
	pthread_cond_destroy(&proc->done_cond);
	pthread_cond_destroy(&proc->queue_cond);
	pthread_mutex_destroy(&proc->mtx);
	data_writer_cleanup(proc);
	return NULL;
}

void sqfs_data_writer_destroy(sqfs_data_writer_t *proc)
{
	unsigned int i;

	pthread_mutex_lock(&proc->mtx);
	proc->status = -1;
	pthread_cond_broadcast(&proc->queue_cond);
	pthread_mutex_unlock(&proc->mtx);

	for (i = 0; i < proc->num_workers; ++i) {
		pthread_join(proc->workers[i]->thread, NULL);

		proc->workers[i]->cmp->destroy(proc->workers[i]->cmp);
		free(proc->workers[i]);
	}

	pthread_cond_destroy(&proc->done_cond);
	pthread_cond_destroy(&proc->queue_cond);
	pthread_mutex_destroy(&proc->mtx);

	data_writer_cleanup(proc);
}

static int append_to_work_queue(sqfs_data_writer_t *proc,
				sqfs_block_t *block, bool signal_threads)
{
	int status;

	pthread_mutex_lock(&proc->mtx);
	status = proc->status;
	if (status != 0) {
		free(block);
		pthread_mutex_unlock(&proc->mtx);
		return status;
	}

	if (block != NULL) {
		if (proc->queue_last == NULL) {
			proc->queue = proc->queue_last = block;
		} else {
			proc->queue_last->next = block;
			proc->queue_last = block;
		}

		block->sequence_number = proc->enqueue_id++;
		block->next = NULL;
		proc->backlog += 1;
	}

	if (signal_threads)
		pthread_cond_broadcast(&proc->queue_cond);
	pthread_mutex_unlock(&proc->mtx);
	return 0;
}

static sqfs_block_t *try_dequeue(sqfs_data_writer_t *proc)
{
	sqfs_block_t *queue, *it, *prev;

	it = proc->done;
	prev = NULL;

	while (it != NULL && it->sequence_number == proc->dequeue_id) {
		prev = it;
		it = it->next;
		proc->dequeue_id += 1;
	}

	if (prev == NULL) {
		queue = NULL;
	} else {
		queue = proc->done;
		prev->next = NULL;
		proc->done = it;
	}

	return queue;
}

static sqfs_block_t *queue_merge(sqfs_block_t *lhs, sqfs_block_t *rhs)
{
	sqfs_block_t *it, *head = NULL, **next_ptr = &head;

	while (lhs != NULL && rhs != NULL) {
		if (lhs->sequence_number <= rhs->sequence_number) {
			it = lhs;
			lhs = lhs->next;
		} else {
			it = rhs;
			rhs = rhs->next;
		}

		*next_ptr = it;
		next_ptr = &it->next;
	}

	it = (lhs != NULL ? lhs : rhs);
	*next_ptr = it;
	return head;
}

static int process_done_queue(sqfs_data_writer_t *proc, sqfs_block_t *queue)
{
	sqfs_block_t *it, *block = NULL;
	int status = 0;

	while (queue != NULL && status == 0) {
		it = queue;
		queue = it->next;
		proc->backlog -= 1;

		if (it->flags & SQFS_BLK_IS_FRAGMENT) {
			block = NULL;
			status = process_completed_fragment(proc, it, &block);

			if (block != NULL && status == 0) {
				pthread_mutex_lock(&proc->mtx);
				proc->dequeue_id = it->sequence_number;
				block->sequence_number = it->sequence_number;

				if (proc->queue == NULL) {
					proc->queue = block;
					proc->queue_last = block;
				} else {
					block->next = proc->queue;
					proc->queue = block;
				}

				proc->backlog += 1;
				proc->done = queue_merge(queue, proc->done);
				pthread_cond_broadcast(&proc->queue_cond);
				pthread_mutex_unlock(&proc->mtx);

				queue = NULL;
			} else {
				free(block);
			}
		} else {
			status = process_completed_block(proc, it);
		}

		free(it);
	}

	free_blk_list(queue);
	return status;
}

int test_and_set_status(sqfs_data_writer_t *proc, int status)
{
	pthread_mutex_lock(&proc->mtx);
	if (proc->status == 0) {
		proc->status = status;
	} else {
		status = proc->status;
	}
	pthread_cond_broadcast(&proc->queue_cond);
	pthread_mutex_unlock(&proc->mtx);
	return status;
}

static int wait_completed(sqfs_data_writer_t *proc)
{
	sqfs_block_t *queue;
	int status;

	pthread_mutex_lock(&proc->mtx);
	for (;;) {
		queue = try_dequeue(proc);
		status = proc->status;

		if (queue != NULL || status != 0)
			break;

		pthread_cond_wait(&proc->done_cond, &proc->mtx);
	}
	pthread_mutex_unlock(&proc->mtx);

	if (status != 0) {
		free_blk_list(queue);
		return status;
	}

	status = process_done_queue(proc, queue);
	if (status != 0)
		return test_and_set_status(proc, status);

	return status;
}

int data_writer_enqueue(sqfs_data_writer_t *proc, sqfs_block_t *block)
{
	int status;

	while (proc->backlog > proc->max_backlog) {
		status = wait_completed(proc);
		if (status)
			return status;
	}

	if (proc->backlog == proc->max_backlog)
		proc->notify_threads = true;

	return append_to_work_queue(proc, block, proc->notify_threads);
}

int sqfs_data_writer_finish(sqfs_data_writer_t *proc)
{
	int status;

	append_to_work_queue(proc, NULL, true);

	while (proc->backlog > 0) {
		status = wait_completed(proc);
		if (status)
			return status;
	}

	if (proc->frag_block != NULL) {
		status = data_writer_do_block(proc->frag_block,
					      proc->workers[0]->cmp,
					      proc->workers[0]->scratch,
					      proc->max_block_size);
		if (status)
			return status;

		status = process_done_queue(proc, proc->frag_block);
		proc->frag_block = NULL;
		if (status)
			return status;
	}

	return 0;
}
