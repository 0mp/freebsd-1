/*-
 * Copyright (c) 2019 (Graeme Jenkinson)
 * All rights reserved.
 *
 * This software was developed by BAE Systems, the University of Cambridge
 * Computer Laboratory, and Memorial University under DARPA/AFRL contract
 * FA8650-15-C-7558 ("CADETS"), as part of the DARPA Transparent Computing
 * (TC) research program.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract FA8750-10-C-0237
 * ("CTSRD"), as part of the DARPA CRASH research programme.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/types.h>
#include <machine/atomic.h>
#include <netinet/in.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/queue.h>
#include <sys/hash.h>
#include <sys/nv.h>
#include <sys/conf.h>
#include <sys/sysctl.h>
#include <fs/devfs/devfs_int.h>
#include <sys/eventhandler.h>
#include <dev/virtio/dtrace/virtio_dtrace.h>

#include <dtrace.h>
#include <dtrace_impl.h>

#include "dlog_client.h"
#include "dl_assert.h"
#include "dl_config.h"
#include "dl_protocol.h"
#include "dl_utils.h"

// extern hrtime_t dtrace_gethrtime(void);

struct client
{
	LIST_ENTRY(client)
	client_entries;
	struct cv ddtrace_cv;
	struct mtx ddtrace_mtx;
	struct proc *ddtrace_pid;
	struct dlog_handle *ddtrace_dlog_handle;
	dtrace_state_t *ddtrace_state;
	int ddtrace_exit;
};

LIST_HEAD(clients, client);

MALLOC_DECLARE(M_DDTRACE);
MALLOC_DEFINE(M_DDTRACE, "ddtrace", "DDTrace memory");

static int ddtrace_event_handler(struct module *, int, void *);
static void ddtrace_thread(void *);

static void ddtrace_buffer_switch(dtrace_state_t *, struct dlog_handle *);
static int ddtrace_persist_metadata(dtrace_state_t *, struct dlog_handle *);
static void ddtrace_persist_trace(dtrace_state_t *, struct dlog_handle *,
								  dtrace_bufdesc_t *);

static void ddtrace_open(void *, struct dtrace_state *);
static void ddtrace_close(void *, struct dtrace_state *);
static void ddtrace_stop(struct clients *);

static dtrace_dops_t dops = {
	.dtdops_open = ddtrace_open,
	.dtdops_close = ddtrace_close,
};
static dtrace_dist_id_t did;

extern kmutex_t dtrace_lock;
extern dtrace_probe_t **dtrace_probes; /* array of all probes */
extern int dtrace_nprobes;			   /* number of probes */

static char const *const DDTRACE_NAME = "ddtrace";
static char *DDTRACE_KEY = "ddtrace";
static char *DDTRACE_EPROBE_KEY = "eprobe";
static char *DDTRACE_FORMAT_KEY = "format";
static char *DDTRACE_PROBE_KEY = "probe";
static char *DDTRACE_NFORMAT_KEY = "nformat";
static char *DDTRACE_NPROBE_KEY = "nprobe";

static moduledata_t ddtrace_conf = {
	DDTRACE_NAME,
	ddtrace_event_handler,
	NULL};

static const int DDTRACE_NHASH_BUCKETS = 16;
static struct clients *ddtrace_hashtbl = NULL;
static u_long ddtrace_hashmask;

SYSCTL_NODE(_kern, OID_AUTO, ddtrace, CTLFLAG_RW, 0, "DDTrace");

/* Poll period in ms; at the expiration of the poll period the in-kernel
 * consumer performs a swap of the per-CPU trace buffers and processes
 * the records.
 */
static uint32_t ddtrace_poll_ms = 1000;
SYSCTL_U32(_kern_ddtrace, OID_AUTO, poll_period_ms, CTLFLAG_RW,
		   &ddtrace_poll_ms, 0, "DDTrace poll period (ms)");

/* Maximum record size before compression; the default value is a heurstic
 * based on the level of compression seen in DTrace buffers.
 */
static uint32_t ddtrace_record_bound = 1024 * 1024;
SYSCTL_U32(_kern_ddtrace, OID_AUTO, record_bound, CTLFLAG_RW,
		   &ddtrace_record_bound, 0,
		   "DDTrace maximum record size (before compression)");

static eventhandler_tag ddtrace_pre_sync = NULL;

static inline void
ddtrace_assert_integrity(const char *func, struct client *self)
{

	DL_ASSERT(self != NULL, ("%s client instance is NULL", func));
	DL_ASSERT(self->ddtrace_dlog_handle != NULL,
			  ("%s client instance Dlog handle field is NULL", func));
	DL_ASSERT(self->ddtrace_state != NULL,
			  ("%s client instance DTrace state field is NULL", func));
	DL_ASSERT(self->ddtrace_pid != NULL,
			  ("%s client instance proc is field NULL", func));
}

static int
ddtrace_event_handler(struct module *module, int event, void *arg)
{
	int e = 0;

	switch (event)
	{
	case MOD_LOAD:
		DLOGTR0(PRIO_LOW, "Loading DDTrace kernel module\n");

		/* Initialise the hash table of client instances. */
		ddtrace_hashtbl = hashinit(DDTRACE_NHASH_BUCKETS, M_DDTRACE,
								   &ddtrace_hashmask);
		DL_ASSERT(ddtrace_hashtbl != NULL,
				  ("Failed to allocate new client hash table instance."));

		/* Register with DTrace. After successfully
		 * registering the client with be informed of lifecycle
		 * events (open/close) that result from DTrace consumers.
		 */
		if (dtrace_dist_register(DDTRACE_NAME, &dops, NULL,
								 &did) == 0)
		{

			DLOGTR0(PRIO_NORMAL,
					"Successfully registered with DTrace\n");

			ddtrace_pre_sync = EVENTHANDLER_REGISTER(
				shutdown_pre_sync, ddtrace_stop, ddtrace_hashtbl,
				SHUTDOWN_PRI_DEFAULT);
		}
		else
		{

			DLOGTR0(PRIO_HIGH,
					"Failed to register with DTrace\n");
			e = -1;
		}
		break;
	case MOD_UNLOAD:
		DLOGTR0(PRIO_LOW, "Unloading DDTrace kernel module\n");

		if (ddtrace_pre_sync != NULL)
			EVENTHANDLER_DEREGISTER(shutdown_pre_sync, ddtrace_pre_sync);

		ddtrace_stop(ddtrace_hashtbl);
		break;
	default:
		e = EOPNOTSUPP;
		break;
	}

	return e;
}

static void
ddtrace_stop(struct clients *ddtrace_hashtbl)
{
	struct client *k, *k_tmp;
	int i, rc;

	/* Unregister and stop any client threads. */
	for (i = 0; i < DDTRACE_NHASH_BUCKETS; i++)
	{
		LIST_FOREACH_SAFE(k, &ddtrace_hashtbl[i],
						  client_entries, k_tmp)
		{

			DLOGTR1(PRIO_LOW,
					"Stopping client thread %p..\n", k);
			/* Signal client and wait for completion. */
			mtx_lock(&k->ddtrace_mtx);
			k->ddtrace_exit = 1;
			mtx_unlock(&k->ddtrace_mtx);
			cv_broadcast(&k->ddtrace_cv);
			rc = tsleep(k->ddtrace_pid, 0,
						"Waiting for client process to stop",
						60 * (10 * hz / 9));
			DL_ASSERT(rc == 0,
					  ("Failed to stop client thread"));

			/* Remove the client and destroy. */
			DLOGTR0(PRIO_LOW,
					"DDTrace thread stoppped successfully\n");
			LIST_REMOVE(k, client_entries);
			mtx_destroy(&k->ddtrace_mtx);
			cv_destroy(&k->ddtrace_cv);
			free(k, M_DDTRACE);
		}
	}

	/* Destroy the hash table of client instances. */
	hashdestroy(ddtrace_hashtbl, M_DDTRACE, ddtrace_hashmask);

	/* Unregister with DTrace.
	 * Note that dtrace_lock must be held to manipulate the mutable dtrace
	 * state (the list of in-kernel clients).
	 */
	dtrace_dist_unregister(&did);
}

static void
ddtrace_buffer_switch(dtrace_state_t *state, struct dlog_handle *handle)
{
	caddr_t cached;
	dtrace_bufdesc_t desc;
	dtrace_buffer_t *buf;

	DL_ASSERT(state != NULL, ("DTrace state cannot be NULL\n"));
	DL_ASSERT(handle != NULL, ("DLog handle cannot be NULL\n"));

	/* Process each of the per-CPU buffers.
	 * The tomax and xamot buffers are first swtich using a xcall.
	 * Provided that the xcvall is successful in switching the buffers,
	 * the buffer is then persisted into Dlog.
	 * Persisting the buffer may involving splitting into portions portions
	 * on a record boundary.
	 */
	for (int cpu = 0; cpu < mp_ncpus; cpu++)
	{

		/* NOTE:
		 * Unlike in the BUFSNAP ioctl it is unnecessary to acquire
		 * dtrace_lock.
		 */

		buf = &state->dts_buffer[cpu];
		DL_ASSERT(
			(buf->dtb_flags & (DTRACEBUF_RING | DTRACEBUF_FILL)) == 0,
			("DTrace ring/fill buffer policy is not supported"));

		if (buf->dtb_tomax == NULL)
			break;

		cached = buf->dtb_tomax;
		DL_ASSERT(!(buf->dtb_flags & DTRACEBUF_NOSWITCH),
				  ("DTrace buffer no switch flag set."));

		/* Perform xcall to swap the CPU's DTrace buffers. */
		dtrace_xcall(cpu, (dtrace_xcall_t)dtrace_buffer_switch, buf);

		/* Check that xcall of dtrace_buffer_switch succeeded. */
		if (buf->dtb_tomax == cached)
		{

			DL_ASSERT(buf->dtb_xamot != cached,
					  ("DTrace buffers pointers are inconsistent"));
			continue;
		}

		DL_ASSERT(cached == buf->dtb_xamot,
				  ("DTrace buffers pointers are inconsistent"));

		state->dts_errors += buf->dtb_xamot_errors;

		desc.dtbd_data = buf->dtb_xamot;
		desc.dtbd_size = buf->dtb_xamot_offset;
		desc.dtbd_drops = buf->dtb_xamot_drops;
		desc.dtbd_errors = buf->dtb_xamot_errors;
		desc.dtbd_oldest = 0;
		desc.dtbd_timestamp = buf->dtb_switched;

		/* If the buffer contains records persist them to the
		 * distributed log.
		 */
		
		if (desc.dtbd_size != 0) {
			DLOGTR0(PRIO_LOW, "About to persist trace data");
			ddtrace_persist_trace(state, handle, &desc);
		}
	}
}

static void
ddtrace_thread(void *arg)
{
	struct client *k = (struct client *)arg;
	struct timespec curtime;

	ddtrace_assert_integrity(__func__, k);

	/* Write the metadata to the log before processing the trace
	 * buffers.
	 */
	if (ddtrace_persist_metadata(k->ddtrace_state,
								 k->ddtrace_dlog_handle))
	{

		DLOGTR0(PRIO_HIGH, "Failed persisting metadata.\n");
		return;
	}

	/* Process the trace buffers. */
	for (;;)
	{

		mtx_lock(&k->ddtrace_mtx);
		cv_timedwait_sbt(&k->ddtrace_cv, &k->ddtrace_mtx,
						 SBT_1MS * ddtrace_poll_ms, SBT_1MS, 0);
		if (k->ddtrace_exit)
		{

			mtx_unlock(&k->ddtrace_mtx);
			DLOGTR0(PRIO_LOW, "Stopping client thread...\n");
			break;
		}
		mtx_unlock(&k->ddtrace_mtx);

		/* Mimic the userpsace STATUS ioctl.
		 * Without updating the dts_alive field DTrace
		 * will transition to the KILLED state.
		 */
		nanouptime(&curtime);
		k->ddtrace_state->dts_alive = INT64_MAX;
		dtrace_membar_producer();
		// TODO(MARA): solve this hack
		k->ddtrace_state->dts_alive = (hrtime_t) dtrace_gethrtime();

		/* Switch the buffer and write the contents to DLog. */
		ddtrace_buffer_switch(k->ddtrace_state,
							  k->ddtrace_dlog_handle);
	}

	/* Switch the buffer and write the contetnts to DLog before exiting.
	 * This ensure that the userspace DTrace process recieves an
	 * empty buffer on termination.
	 */
	ddtrace_buffer_switch(k->ddtrace_state,
						  k->ddtrace_dlog_handle);

	DLOGTR0(PRIO_NORMAL, "DDTrace thread exited successfully.\n");
	kthread_exit();
}

static int
ddtrace_persist_metadata(dtrace_state_t *state, struct dlog_handle *hdl)
{
	dtrace_action_t *act;
	dtrace_ecb_t *ecb;
	dtrace_eprobedesc_t epdesc;
	dtrace_probe_t *probe;
	dtrace_probedesc_t pdesc;
	char *fmt_str;
	size_t size;
	uintptr_t dest;
	void *buf;
	int fmt_len, nrecs;

	DL_ASSERT(state != NULL, ("DTrace state cannot be NULL."));
	DL_ASSERT(hdl != NULL, ("DLog handle cannot be NULL."));

	/* Write the formats to the log:
	 * this mirrors the DTRACEIOC_FORMAT ioctl.
	 */
	DLOGTR0(PRIO_LOW, "Persisting dtrace format string metadata\n");

	mutex_enter(&dtrace_lock);

#if 0
	if (dlog_produce(hdl, DDTRACE_NFORMAT_KEY,
					 (unsigned char *)&state->dts_nformats, sizeof(int)) != 0)
	{

		DLOGTR0(PRIO_HIGH,
				"Error producing format metadata to DLog\n");
		return -1;
	}
#endif

	for (int fmt = 1; fmt <= state->dts_nformats; fmt++)
	{
		/*
		 * Format strings are allocated contiguously and they are
		 * never freed; if a format index is less than the number
		 * of formats, we can assert that the format map is non-NULL
		 * and that the format for the specified index is non-NULL.
		 */
		DL_ASSERT(state->dts_formats != NULL,
				  ("Format array cannot be NULL"));
		fmt_str = state->dts_formats[fmt - 1];
		DL_ASSERT(fmt_str != NULL, ("Format string cannor be NULL"));
		fmt_len = strlen(fmt_str) + 1;

#if 0
		/* Persit the format string to dlog. */
		if (dlog_produce(hdl, DDTRACE_FORMAT_KEY, fmt_str, fmt_len) != 0)
		{

			DLOGTR0(PRIO_HIGH,
					"Error producing format metadata to DLog\n");
			return -1;
		}

#endif
	}

	mutex_exit(&dtrace_lock);

	/* Write the eprobedesc to the log: this duplicates the
	 * DTRACEIOC_EPROBE ioctl.
	 */
	DLOGTR0(PRIO_LOW, "Persisting dtrace eprobe metadata\n");

	/* TODO: Some though is needed here dtrace_nprobes is not part of
	 * the dtrace state. How is it updated (under what lock)?
	 * I truth I only what to send the epids, can these be rescaled
	 * to monotonical increase for 1?
	 */

#if 0
	if (dlog_produce(hdl, DDTRACE_NPROBE_KEY,
					 (unsigned char *)&dtrace_nprobes, sizeof(int)) != 0)
	{

		DLOGTR0(PRIO_HIGH,
				"Error producing format metadata to DLog\n");
		return -1;
	}
#endif

	mutex_enter(&dtrace_lock);
	DL_ASSERT(state->dts_necbs > 0 && state->dts_ecbs != NULL,
			  ("dtrace ecb state is invalid"));
	for (dtrace_epid_t epid = 1; epid <= state->dts_epid; epid++)
	{

		DLOGTR1(PRIO_LOW, "Persisting dtrace eprobe (%d) metadata\n",
				epid);

		DL_ASSERT(state->dts_necbs > 0 && state != NULL,
				  ("DTace ECB state is invalid"));
		DL_ASSERT((ecb = state->dts_ecbs[epid - 1]) == NULL ||
					  ecb->dte_epid == epid,
				  ("DTrace ECBS state is inconsistent"));

		ecb = state->dts_ecbs[epid - 1];
		if (ecb == NULL || ecb->dte_probe == NULL)
			continue;

		/* TODO: Only persist the metadata where the probe is matched
		 * for the current zone; see the DTRACEIOC_PROBES/_PROBEMATCH ioctl
		 * implementation.
		 */
		if ((probe = dtrace_probes[ecb->dte_probe->dtpr_id - 1]) != NULL)
		{

			bzero(&pdesc, sizeof(dtrace_probedesc_t));
			pdesc.dtpd_provider[DTRACE_PROVNAMELEN - 1] = '\0';
			pdesc.dtpd_mod[DTRACE_MODNAMELEN - 1] = '\0';
			pdesc.dtpd_func[DTRACE_FUNCNAMELEN - 1] = '\0';
			pdesc.dtpd_name[DTRACE_NAMELEN - 1] = '\0';

			/* Construct the probe description and
			 * persist the metadata to dlog.
			 */
			pdesc.dtpd_id = epid; // TODO: temporary fix
			(void)strncpy(pdesc.dtpd_provider,
						  probe->dtpr_provider->dtpv_name,
						  DTRACE_PROVNAMELEN - 1);
			(void)strncpy(pdesc.dtpd_mod, probe->dtpr_mod,
						  DTRACE_MODNAMELEN - 1);
			(void)strncpy(pdesc.dtpd_func, probe->dtpr_func,
						  DTRACE_FUNCNAMELEN - 1);
			(void)strncpy(pdesc.dtpd_name, probe->dtpr_name,
						  DTRACE_NAMELEN - 1);
#if 0
			if (dlog_produce(hdl, DDTRACE_PROBE_KEY,
							 (unsigned char *)&pdesc,
							 sizeof(dtrace_probedesc_t)) != 0)
			{

				DLOGTR0(PRIO_HIGH,
						"Error producing format probe metadata "
						"to DLog\n");
				return -1;
			}
#endif

			epdesc.dtepd_epid = epid;
			epdesc.dtepd_probeid = ecb->dte_probe->dtpr_id;
			epdesc.dtepd_uarg = ecb->dte_uarg;
			epdesc.dtepd_size = ecb->dte_size;
			epdesc.dtepd_nrecs = 0;
			for (act = ecb->dte_action; act != NULL; act = act->dta_next)
			{
				if (DTRACEACT_ISAGG(act->dta_kind) || act->dta_intuple)
					continue;

				epdesc.dtepd_nrecs++;
			}
			nrecs = epdesc.dtepd_nrecs;

			/*
			 * now that we have the size, we need to allocate a temporary
			 * buffer in which to store the complete description.  we need
			 * the temporary buffer to be able to drop dtrace_lock()
			 * across the copyout(), below.
			 */
			size = sizeof(dtrace_eprobedesc_t) +
				   (epdesc.dtepd_nrecs * sizeof(dtrace_recdesc_t));

			buf = malloc(size, M_DDTRACE, M_NOWAIT);
			dest = (uintptr_t)buf;

			bcopy(&epdesc, (void *)dest, sizeof(epdesc));
			dest += offsetof(dtrace_eprobedesc_t, dtepd_rec[0]);

			for (act = ecb->dte_action; act != NULL; act = act->dta_next)
			{
				if (DTRACEACT_ISAGG(act->dta_kind) || act->dta_intuple)
					continue;

				if (nrecs-- == 0)
					break;

				bcopy(&act->dta_rec, (void *)dest,
					  sizeof(dtrace_recdesc_t));
				dest += sizeof(dtrace_recdesc_t);
			}
#if 0
			/* Persist the EPROBE metadata to dlog. */
			if (dlog_produce(hdl, DDTRACE_EPROBE_KEY, buf, size) != 0)
			{

				DLOGTR0(PRIO_HIGH,
						"Error producing format eprobe metadata to DLog\n");
				free(buf, M_DDTRACE);
				return -1;
			}
#endif

			free(buf, M_DDTRACE);
		}
	}
	mutex_exit(&dtrace_lock);

	return 0;
}

static void
ddtrace_persist_trace(dtrace_state_t *state, struct dlog_handle *hdl,
					  dtrace_bufdesc_t *desc)
{
	dtrace_epid_t epid;
	struct vtdtr_traceq *tq;
	struct vtdtr_trace_entry *trc_entry;
	struct virtio_dtrace_trace *trc;
	size_t msg_start = 0, msg_size = 0, size = 0;

	DL_ASSERT(state != NULL, ("DTrace state cannot be NULL."));
	DL_ASSERT(hdl != NULL, ("DLog handle cannot be NULL."));
	DL_ASSERT(desc != NULL,
			  ("DTrace buffer description cannot be NULL."));
	DL_ASSERT(desc->dtbd_data != NULL,
			  ("ddtrace_persist_trace called with NULL buffer."));
	DL_ASSERT(desc->dtbd_size != 0,
			  ("ddtrace_persist_trace called with empty buffer."));

	DLOGTR0(PRIO_LOW, "Persisting trace data. \n");

	tq = virtio_dtrace_device_register();
	DL_ASSERT(tq != NULL, ("vtdtr_traceq was not initialised.\n"));
	
	trc_entry = malloc(sizeof(struct vtdtr_trace_entry), M_DEVBUF, M_NOWAIT | M_ZERO);
	DL_ASSERT(trc_entry != NULL, "Failed allocating memory for trace entry.");
	
	trc = &trc_entry->trace;
	trc->dtbd_size = desc->dtbd_size;
	trc->dtbd_cpu = desc->dtbd_cpu;
	trc->dtbd_errors = desc->dtbd_errors;
	trc->dtbd_drops = desc->dtbd_drops;
	trc->dtbd_data = desc->dtbd_data;
	trc->dtbd_oldest = desc->dtbd_oldest;

	printf("Allocated fields.");

	DLOGTR2(PRIO_LOW, "Trace data size is: %zu. Copied trace data size: %zu. \n", desc->dtbd_size, trc->dtbd_size);
	DL_ASSERT(trc->dtbd_data != NULL, "Cannot enqueue trace entry with NULL trace data buffer");
	
	mtx_lock(&tq->mtx);
	vtdtr_tq_enqueue(tq, trc_entry);
	DLOGTR0(PRIO_LOW, "Successfully enqueued trace data, unlocking queue.");
	printf("Successfully enqueued trace data, unlocking queue.");
	DL_ASSERT(tq->n_entries != 0, "Failed enqueueing.")
	mtx_unlock(&tq->mtx);
	

#if 0
	while (size < desc->dtbd_size)
	{

		epid = *(dtrace_epid_t *)((uintptr_t)desc->dtbd_data + size);
		if (epid == DTRACE_EPIDNONE)
		{

			size += sizeof(epid);
			continue;
		}

		if (dtrace_epid2size(state, epid) == 0)
		{

			DLOGTR1(PRIO_HIGH,
					"Error payload size is 0 for epid = %u\n", epid);
			break;
		}

		/* Check whether the record would take the msg_size
		 * over the MTU configured for the distributed log.
		 */

		/* As the zlib in kernel is significantly out of date, it
		 * doesn't provide the defalateBounds() method which would
		 * allow me to determine the size of the compressed output.
		 *
		 * Therefore, I am using a configurable parameter.
		 */
		if (msg_size + dtrace_epid2size(state, epid) >
			ddtrace_record_bound)
		{

			/* The umsg_size is zero this occurs when the
			 * DTrace record size is greater than the log
			 * MTU. This should have been checked during 
			 * ddtrace_open().
			 */
			DL_ASSERT(msg_size != 0,
					  ("Error DTrace record size %zu is greater "
					   "than log MTU %d\n",
					   dtrace_epid2size(state, epid), DL_MTU));

#if 0
			if (dlog_produce(hdl,
							 DDTRACE_KEY,
							 &desc->dtbd_data[msg_start], msg_size) != 0)
			{

				DLOGTR0(PRIO_HIGH,
						"Error producing message to DLog\n");
			}
#endif

			/* Reset the msg_size and set the msg_start
			 * to the location in the buffer at which the
			 * next message starts.
			 */
			msg_start += msg_size;
			msg_size = 0;
		}
		else
		{

			/* Increment the message and total size by the
			 * payload of the current record.
			 */
			size += dtrace_epid2size(state, epid);
			msg_size += dtrace_epid2size(state, epid);

			/* Check whether the record is the last in the
			 * buffer.
			 */
			if (msg_size == desc->dtbd_size)
			{
#if 0
				if (dlog_produce(hdl,
								 DDTRACE_KEY,
								 &desc->dtbd_data[msg_start],
								 msg_size) != 0)
				{

					DLOGTR0(PRIO_HIGH,
							"Error producing message to DLog\n");
				}
#endif

				/* Reset the msg_size and set the msg_start
				 * to the location in the buffer at which
				 * the next message starts.
				 */
				msg_start += msg_size;
				msg_size = 0;
			}
		}
	}
#endif
}

static void
ddtrace_open(void *arg, struct dtrace_state *state)
{
	struct cdev_privdata *p;
	struct dlog_handle *handle;
	struct file *fp;
	struct filedesc *fdp = curproc->p_fd;
	struct client *k;
	dtrace_epid_t epid;
	dtrace_dist_t *dist = (dtrace_dist_t *)arg;
	uint32_t hash;
	int rc;

	DL_ASSERT(state != NULL, ("DTrace state cannot be NULL."));
	DL_ASSERT(dist != NULL, ("DTrace client instance cannot be NULL."));

	DLOGTR0(PRIO_LOW, "ddtrace_open\n");

	/* Check the the payload of the enabled probes is less than the
	 * configured MTU of the distributed log.
	 */
	for (epid = 1; epid < state->dts_epid; epid++)
	{
		if (dtrace_epid2size(state, epid) > DL_MTU)
		{
			DLOGTR3(PRIO_HIGH,
					"DDTrace (%s) rendezvous with DLog state "
					"DTrace record size %zu is greater "
					"than log MTU %d\n",
					dist->dtd_name,
					dtrace_epid2size(state, epid), DL_MTU);
			return;
		}
	}

	/* Confirm that the DTrace buffer policy is "switch". */
	if (state->dts_options[DTRACEOPT_BUFPOLICY] !=
		DTRACEOPT_BUFPOLICY_SWITCH)
	{

		DLOGTR1(PRIO_HIGH,
				"DDTrace (%s) rendezvous with DLog state failed "
				"DTrace bufpolicy must be switch\n",
				dist->dtd_name);
		return;
	}

	/* Convert the DLog file descriptor into a struct dlog_handle */
	if (state->dts_options[DTRACEOPT_DDTRACEARG] == DTRACEOPT_UNSET)
	{

		DLOGTR1(PRIO_HIGH,
				"DDTrace (%s) rendezvous with DLog state failed "
				"DTrace ddtracearg option is unset\n",
				dist->dtd_name);
		return;
	}

	FILEDESC_SLOCK(fdp);
	fp = fget_locked(fdp, state->dts_options[DTRACEOPT_DDTRACEARG]);
	FILEDESC_SUNLOCK(fdp);
	if (fp == NULL)
	{

		DLOGTR1(PRIO_HIGH,
				"DDTrace (%s) rendezvous with DLog state failed "
				"DTrace ddtracearg is not a valid file decriptor\n",
				dist->dtd_name);
		return;
	}

	p = fp->f_cdevpriv;
	if (p == NULL)
	{

		DLOGTR1(PRIO_HIGH,
				"DDTrace (%s) rendezvous with DLog state failed "
				"DTrace ddtracearg file descriptor is not associated with "
				"dlog handle\n",
				dist->dtd_name);
		return;
	}

	handle = (struct dlog_handle *)p->cdpd_data;
	if (handle == NULL)
	{

		DLOGTR1(PRIO_HIGH,
				"DDTrace (%s) rendezvous with DLog state failed "
				"DTrace ddtracearg file secriptor is not associated with "
				"dlog handle\n",
				dist->dtd_name);
		return;
	}

	/* ALlocate a new DDTrace instance. */
	k = (struct client *)malloc(sizeof(struct client), M_DDTRACE,
								M_NOWAIT);
	DL_ASSERT(k != NULL, ("Failed to allocate new client instance."));

	bzero(k, sizeof(struct client));
	mtx_init(&k->ddtrace_mtx, "ddtrace mtx", DDTRACE_NAME, MTX_DEF);
	cv_init(&k->ddtrace_cv, "ddtrace cv");
	k->ddtrace_state = state;
	k->ddtrace_exit = 0;
	k->ddtrace_dlog_handle = handle;
	rc = kproc_kthread_add(ddtrace_thread, k, &k->ddtrace_pid, NULL, 0, 0,
						   NULL, NULL);
	DL_ASSERT(rc == 0, ("DDTrace open kproc_kthread_add failed %d\n", rc));

	ddtrace_assert_integrity(__func__, k);

	/* Added the new client instance into the hashmap, index by the
	 * dtrace_state pointer(the pointer is hashed as the state itself
	 * changes over the execution).
	 */
	hash = murmur3_32_hash(&state, sizeof(struct dtrace_state *), 0) &
		   ddtrace_hashmask;
	LIST_INSERT_HEAD(&ddtrace_hashtbl[hash], k, client_entries);
}

static void
ddtrace_close(void *arg, struct dtrace_state *state)
{
	struct client *k, *k_tmp;
	uint32_t hash;

	DL_ASSERT(state != NULL, ("DTrace state cannot be NULL"));
	DL_ASSERT(MUTEX_HELD(&dtrace_lock),
			  ("dtrace_lock should be held in dtrace_state_stop()"));

	/* Lookup the DDTrace instance based on the DTrace state passed into
	 * ddtrace_close.
	 */
	hash = murmur3_32_hash(&state, sizeof(struct dtrace_state *), 0) &
		   ddtrace_hashmask;
	LIST_FOREACH_SAFE(k, &ddtrace_hashtbl[hash], client_entries, k_tmp)
	{

		ddtrace_assert_integrity(__func__, k);
		if (state == k->ddtrace_state)
		{

			/* Stop the client thread and wait until it has
			 * persisted all oustanding DTrace records to DLog.
			 */
			mtx_lock(&k->ddtrace_mtx);
			k->ddtrace_exit = 1;
			mtx_unlock(&k->ddtrace_mtx);
			cv_broadcast(&k->ddtrace_cv);
			tsleep(k->ddtrace_pid, 0,
				   "Waiting for client thread to stop",
				   60 * (10 * hz / 9));

			/* Remove the client instance from the hash map
			 * and destroy it.
			 */
			DLOGTR0(PRIO_NORMAL,
					"DDTrace thread stoppped successfully\n");
			LIST_REMOVE(k, client_entries);
			mtx_destroy(&k->ddtrace_mtx);
			cv_destroy(&k->ddtrace_cv);
			free(k, M_DDTRACE);
			return;
		}
	}

	DL_ASSERT(1, ("ddtrace_close called with invalid DTrace state."));
}

DECLARE_MODULE(ddtrace, ddtrace_conf, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_VERSION(ddtrace, 1);
MODULE_DEPEND(ddtrace, dlog, 1, 1, 1);
MODULE_DEPEND(ddtrace, dtrace, 1, 1, 1);
