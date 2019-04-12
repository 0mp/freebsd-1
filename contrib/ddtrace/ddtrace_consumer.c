/*-
 * Copyright (c) 2018 (Graeme Jenkinson)
 * All rights reserved.
 *
 * This software was developed by BAE Systems, the University of Cambridge
 * Computer Laboratory, and Memorial University under DARPA/AFRL contract
 * FA8650-15-C-7558 ("CADETS"), as part of the DARPA Transparent Computing
 * (TC) research program.
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

#include <dt_impl.h>

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef PRIVATE_RDKAFKA 
#include <private/rdkafka/rdkafka.h>
#else
#include <librdkafka/rdkafka.h>
#endif

#include "dl_assert.h"
#include "dl_bbuf.h"
#include "dl_memory.h"
#include "dl_utils.h"

const dlog_malloc_func dlog_alloc = malloc;
const dlog_free_func dlog_free = free;

static int dtc_get_buf(dtrace_hdl_t *, int, dtrace_bufdesc_t **);
static void dtc_put_buf(dtrace_hdl_t *, dtrace_bufdesc_t *b);
static int dtc_buffered_handler(const dtrace_bufdata_t *, void *);
static int dtc_setup_rx_topic(char *, char *, char *, char *, char *,
    char *);
static int dtc_setup_tx_topic(char *, char *, char *, char *, char *,
    char *);
static int dtc_register_daemon(void);

static char const * const DTC_PIDFILE = "/var/run/ddtracec.pid";

static char *g_pname;
static int g_status = 0;
static volatile int g_intr = 0;
static rd_kafka_t *rx_rk;
static rd_kafka_t *tx_rk;
static rd_kafka_topic_t *tx_topic;
static rd_kafka_topic_t *rx_topic;

static inline void 
dtc_usage(FILE * fp)
{

	(void) fprintf(fp,
	    "Usage: %s -b brokers [-df] "
	    "-i input_topic [-o output_topic] "
	    "[-c client_certificate] [-a ca_cert] [-p password] "
	    "[-k private_key] [-q poll_interval] "
	    "-s script script_args\n", g_pname);

	(void) fprintf(fp, "\n"
	    "\t-d\t--debug\t\t Increase debug output\n"
	    "\t-f\t--frombeginning\t Read from beginning of input topic\n"
	    "\t-b\t--brokers\t Kafka broker connection string\n"
	    "\t-i\t--intopic\t Kafka topic to read from\n"
	    "\t-o\t--outtopic\t Kafka topic to write to\n"
	    "\t-a\t--cacert\t CA_cert path (for TLS support)\n"
	    "\t-c\t--clientcert\t Client certificate path (for TLS support)\n"
	    "\t-p\t--password\t Password for private key (for TLS support)\n"
	    "\t-q\t--poll\t\t Kafka poll interval (in us)\n"
	    "\t-k\t--privkey\t Private key (for TLS support)\n"
	    "\t-s\t\t\t DTrace script.\n"
	    "All remaining arguments will be passed to DTrace.\n");
}

/*ARGSUSED*/
static inline void
dtc_intr(int signo)
{
	DLOGTR1(PRIO_NORMAL, "Stopping %s...\n", g_pname);
	g_intr = 1;
}
	
/*ARGSUSED*/
static int
chew(const dtrace_probedata_t *data, void *arg)
{

	return (DTRACE_CONSUME_THIS);
}
	
/*ARGSUSED*/
static int
chewrec(const dtrace_probedata_t * data, const dtrace_recdesc_t * rec,
    void * arg)
{
	dtrace_actkind_t act;
	uintptr_t addr;

	/* Check if the final record has been processed. */
	if (rec == NULL) {

		return (DTRACE_CONSUME_NEXT); 
	}

	act = rec->dtrd_action;
	addr = (uintptr_t)data->dtpda_data;

	if (act == DTRACEACT_EXIT) {
		g_status = *((uint32_t *) addr);
		return (DTRACE_CONSUME_NEXT);
	}

	return (DTRACE_CONSUME_THIS); 
}

static int
dtc_get_buf(dtrace_hdl_t *dtp, int cpu, dtrace_bufdesc_t **bufp)
{
	dtrace_optval_t size;
	dtrace_bufdesc_t *buf;
	rd_kafka_message_t *rkmessage;
	int partition = 0;
	
	DL_ASSERT(dtp != NULL, ("DTrace handle cannot be NULL"));

	buf = dt_zalloc(dtp, sizeof(*buf));
	if (buf == NULL)
		return -1;

	rkmessage = rd_kafka_consume(rx_topic, partition, 0);
	if (rkmessage != NULL) {

		/* Check that the key of the received Kafka message indicated
		 * the message was produced by Distributed DTrace.
		 *
		 * If the Message key indicates that the message was not
		 * produced by Distribted DTrace, processing the message can
		 * have dire consequences as libdtrace implicitly trusts the
		 * buffers that it processes.
		 */

		if (!rkmessage->err && rkmessage->len > 0) {
			if (rkmessage->key != NULL &&
			    strncmp(rkmessage->key, "ddtrace",
				rkmessage->key_len) == 0) {

					buf->dtbd_data = dt_zalloc(dtp, rkmessage->len);
					if (buf->dtbd_data == NULL) {

						dt_free(dtp, buf);
						return -1;
					}
					buf->dtbd_size = rkmessage->len;
					buf->dtbd_cpu = cpu;

					memcpy(buf->dtbd_data, rkmessage->payload,
					    rkmessage->len);
			} else {

				if (rkmessage->key == NULL) {
					DLOGTR1(PRIO_LOW,
					    "%s: key of Kafka message is NULL\n",
					    g_pname);
				} else {
					DLOGTR2(PRIO_LOW,
					    "%s: key of Kafka message %s is invalid\n",
					    g_pname, rkmessage->key);
				}

				if (rkmessage->payload == NULL) {
					DLOGTR1(PRIO_LOW,
					    "%s: payload of Kafka message is NULL\n",
					    g_pname);
				}
				buf->dtbd_size = 0;
			}
		} else {
			if (rkmessage->err ==
				RD_KAFKA_RESP_ERR__PARTITION_EOF) {
				DLOGTR1(PRIO_LOW,
				    "%s: no message in log\n", g_pname);
			}
			buf->dtbd_size = 0;
		}
			
		rd_kafka_message_destroy(rkmessage);
	}

	*bufp = buf;
	return 0;
}

static void
dtc_put_buf(dtrace_hdl_t *dtp, dtrace_bufdesc_t *buf)
{

	DL_ASSERT(dtp != NULL, ("DTrace handle cannot be NULL"));
	DL_ASSERT(buf != NULL, ("Buffer instance to free cannot be NULL"));

	if (buf->dtbd_data != NULL)
		dt_free(dtp, buf->dtbd_data);
	dt_free(dtp, buf);
}

static int
dtc_buffered_handler(const dtrace_bufdata_t *buf_data, void *arg)
{
	rd_kafka_topic_t *tx_topic = (rd_kafka_topic_t *) arg;
	static struct dl_bbuf *output_buf = NULL;
	size_t buf_len;
	int rc;

	DL_ASSERT(tx_topic != NULL, ("Transmit topic cannot be NULL"));


	/* '{' indicates the start of the JSON message.
	 * Allocate a buffer into which the message is written.
	 */
	if (buf_data->dtbda_buffered[0] == '{') {

		DLOGTR0(PRIO_LOW, "Start of JSON message\n");
		DL_ASSERT(output_buf == NULL,
		    ("Output buffer should be NULL at the start of message."));
		dl_bbuf_new_auto(&output_buf) ;
	} 

	/* Buffer the received data until the end of the JSON message 
	 * is received.
	 * */
	buf_len = strlen(buf_data->dtbda_buffered);
	dl_bbuf_bcat(output_buf, buf_data->dtbda_buffered, buf_len);

	/* '}' indicates the end of the JSON message.
	 * Allocate a buffer into which the message is written.
	 */
	if (buf_data->dtbda_buffered[0] == '}') {

		DLOGTR0(PRIO_LOW, "End of JSON message\n");
retry:
		if (rd_kafka_produce(
			/* Topic object */
			tx_topic,
			/* Use builtin partitioner to select partition*/
			RD_KAFKA_PARTITION_UA,
			/* Make a copy of the payload. */
			RD_KAFKA_MSG_F_COPY,
			/* Message payload (value) and length */
			dl_bbuf_data(output_buf),
			dl_bbuf_pos(output_buf),
			/* Optional key and its length */
			NULL, 0,
			/* Message opaque, provided in
			* delivery report callback as
			* msg_opaque. */
			NULL) == -1) {
			/**
			* Failed to *enqueue* message for producing.
			*/
			DLOGTR2(PRIO_HIGH,
			"%% Failed to produce to topic %s: %s\n",
			rd_kafka_topic_name(rx_topic),
			rd_kafka_err2str(rd_kafka_last_error()));

			/* Poll to handle delivery reports */
			if (rd_kafka_last_error() ==
			    RD_KAFKA_RESP_ERR__QUEUE_FULL) {
				/* If the internal queue is full, wait for
				 * messages to be delivered and then retry.
				 * The internal queue represents both
				 * messages to be sent and messages that have
				 * been sent or failed, awaiting their
				 * delivery report callback to be called.
				 *
				 * The internal queue is limited by the
				 * configuration property
				 * queue.buffering.max.messages */
				rd_kafka_poll(tx_rk, 1000 /*block for max 1000ms*/);
				goto retry;
			}
		} else {
			DLOGTR2(PRIO_LOW,
			    "%% Enqueued message (%zd bytes) for topic %s\n",
			    buf_len, rd_kafka_topic_name(tx_topic));
		}

		/* Free the buffer for the start of the next JSON message */
		dl_bbuf_delete(output_buf);
		output_buf = NULL;
	}
	return 0;
}

static int
dtc_setup_rx_topic(char *topic_name, char *brokers, char *ca_cert,
    char *client_cert, char *priv_key, char *password)
{
	rd_kafka_conf_t *conf;
	char errstr[512];

	DL_ASSERT(topic_name != NULL,
	    ("Receive topic name cannot be NULL"));
	DL_ASSERT(brokers != NULL,
	    ("Receive topic brokers cannot be NULL"));

	/* Setup the Kafka topic used for receiving DTrace records. */
	conf = rd_kafka_conf_new();
	if (conf == NULL) {

		DLOGTR2(PRIO_HIGH, "%s: failed to create Kafka conf: %s\n",
		    g_pname, rd_kafka_err2str(rd_kafka_last_error()));
		goto configure_rx_topic_err;
	}

	if (rd_kafka_conf_set(conf, "client.id", g_pname,
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_rx_topic_new_err;
        }

	if (rd_kafka_conf_set(conf, "socket.nagle.disable", "true",
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_rx_topic_new_err;
        }

	/* Set bootstrap broker(s) as a comma-separated list of
         * host or host:port (default port 9092).
         * librdkafka will use the bootstrap brokers to acquire the full
         * set of brokers from the cluster.
	 */
        if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers,
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_rx_topic_new_err;
        }

	if (rd_kafka_conf_set(conf, "enable.auto.commit", "true",
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_rx_topic_new_err;
        }

	if (rd_kafka_conf_set(conf, "auto.commit.interval.ms", "1000",
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_rx_topic_new_err;
        }

	if (rd_kafka_conf_set(conf, "enable.auto.offset.store", "true",
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_rx_topic_new_err;
        }

	if (rd_kafka_conf_set(conf, "auto.offset.reset", "earliest",
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_rx_topic_new_err;
        }

	if (rd_kafka_conf_set(conf, "check.crcs", "true",
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_rx_topic_new_err;
        }

	if (rd_kafka_conf_set(conf, "group.id", g_pname,
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_rx_topic_new_err;
        }

	if (ca_cert != NULL && client_cert != NULL && priv_key != NULL &&
	    password != NULL) {
		/* Configure TLS support:
		 * https://github.com/edenhill/librdkafka/wiki/Using-SSL-with-librdkafka
		*/
		if (rd_kafka_conf_set(conf, "metadata.broker.list", brokers,
		    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_rx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "security.protocol", "ssl",
		    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_rx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "ssl.ca.location", ca_cert,
		    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_rx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "ssl.certificate.location",
		    client_cert, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_rx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "ssl.key.location", priv_key,
		    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_rx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "ssl.key.password", password,
		    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_rx_topic_new_err;
		}
	}

	/* Create the Kafka consumer.
	 * The configuration instance does not need to be freed after
	 * this succeeds.
	 */
	if (!(rx_rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr,
	    sizeof(errstr)))) {

		DLOGTR2(PRIO_HIGH, "%s: failed to create Kafka consumer: %s\n",
		    g_pname, errstr);
		goto configure_rx_topic_new_err;
	}

	if (!(rx_topic = rd_kafka_topic_new(rx_rk, topic_name, NULL))) {

		DLOGTR3(PRIO_HIGH,
		    "%s: failed to create Kafka topic %s: %s\n",
		    g_pname, topic_name,
		    rd_kafka_err2str(rd_kafka_last_error()));
		goto configure_rx_topic_new_err;
	}

	return 0;

configure_rx_topic_new_err:
	rd_kafka_destroy(rx_rk);

configure_rx_topic_conf_err:
	rd_kafka_conf_destroy(conf);

configure_rx_topic_err:
	return -1;

}

static int
dtc_setup_tx_topic(char *topic_name, char *brokers, char *ca_cert,
    char *client_cert, char *priv_key, char *password)
{
	rd_kafka_conf_t *conf;
	char errstr[512];

	DL_ASSERT(topic_name != NULL,
	    ("Transmit topic name cannot be NULL"));
	DL_ASSERT(brokers != NULL,
	    ("Receive topic brokers cannot be NULL"));

	/* Setup the Kafka topic used for receiving DTrace records. */
	conf = rd_kafka_conf_new();
	if (conf == NULL) {

		DLOGTR2(PRIO_HIGH, "%s: failed to create Kafka conf: %s\n",
		    g_pname, rd_kafka_err2str(rd_kafka_last_error()));
		goto configure_tx_topic_err;
	}

       	/* Set bootstrap broker(s) as a comma-separated list of
         * host or host:port (default port 9092).
         * librdkafka will use the bootstrap brokers to acquire the full
         * set of brokers from the cluster.
	 */
        if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers,
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_tx_topic_conf_err;
        }

	if (rd_kafka_conf_set(conf, "compression.codec", "gzip",
		errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

		DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_tx_topic_conf_err;
	}

	if (rd_kafka_conf_set(conf, "socket.nagle.disable", "true",
	    errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

                DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_tx_topic_new_err;
        }

	if (rd_kafka_conf_set(conf, "linger.ms", "10",
		errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

		DLOGTR1(PRIO_HIGH, "%s\n", errstr);
		goto configure_tx_topic_conf_err;
	}

	if (ca_cert != NULL && client_cert != NULL && priv_key != NULL &&
	    password != NULL) {
		/* Configure TLS support:
		* https://github.com/edenhill/librdkafka/wiki/Using-SSL-with-librdkafkaxi
		*/
		if (rd_kafka_conf_set(conf, "metadata.broker.list", brokers,
		errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_tx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "security.protocol", "ssl",
		errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_tx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "ssl.ca.location", ca_cert,
		errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_tx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "ssl.certificate.location", client_cert,
		errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_tx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "ssl.key.location", priv_key,
		errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_tx_topic_new_err;
		}

		if (rd_kafka_conf_set(conf, "ssl.key.password", password,
		errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {

			DLOGTR1(PRIO_HIGH, "%s\n", errstr);
			goto configure_tx_topic_new_err;
		}
	}

	/* Create the Kafka producer.
	 * The configuration instance does not need to be freed after
	 * this succeeds.
	 */
	if (!(tx_rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr,
	    sizeof(errstr)))) {

		DLOGTR2(PRIO_HIGH,
		    "%s: failed to create Kafka consumer: %s\n",
		    g_pname, errstr);
		goto configure_tx_topic_conf_err;
	}

	if (!(tx_topic = rd_kafka_topic_new(tx_rk, topic_name, NULL))) {

		DLOGTR3(PRIO_HIGH,
		    "%s: failed to create Kafka topic %s: %s\n",
		    g_pname, topic_name,
		    rd_kafka_err2str(rd_kafka_last_error()));
		goto configure_tx_topic_new_err;
	}

	return 0;

configure_tx_topic_new_err:
	rd_kafka_destroy(tx_rk);

configure_tx_topic_conf_err:
	rd_kafka_conf_destroy(conf);

configure_tx_topic_err:
	return -1;
}

static void
dtc_close_pidfile(void)
{

	/* Unlink the dlogd pid file. */
	DLOGTR0(PRIO_LOW, "Unlinking dlogd pid file\n");
	if (unlink(DTC_PIDFILE) == -1 && errno != ENOENT)
		DLOGTR0(PRIO_HIGH,
		    "Error unlinking ddtrace_consumer pid file\n");
}

static int
dtc_register_daemon(void)
{
	FILE * pidfile;
	struct sigaction act;
	int fd;
	pid_t pid;

	(void) sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = dtc_intr;
	(void) sigaction(SIGINT, &act, NULL);
	(void) sigaction(SIGTERM, &act, NULL);

	if ((pidfile = fopen(DTC_PIDFILE, "a")) == NULL) {
	
		DLOGTR0(PRIO_HIGH,
		    "Failed to open pid file for DDTrace consumer\n");
		return (-1);
	}

	/* Attempt to lock the pid file; if a lock is present, exit. */
	fd = fileno(pidfile);
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {

		DLOGTR0(PRIO_HIGH,
		    "Failed to lock pid file for DDTrace consumer\n");
		return (-1);
	}

	pid = getpid();
	ftruncate(fd, 0);
	if (fprintf(pidfile, "%u\n", pid) < 0) {

		/* Should not start the daemon. */
		DLOGTR0(PRIO_HIGH,
		    "Failed write pid file for DDTrace consumer\n");
	}

	fflush(pidfile);
	atexit(dtc_close_pidfile);
	return 0;
}

/*
 * Prototype distributed dtrace agent.
 * The agent recieves DTrace records from an Apache Kafka topic and prints
 * them using libdtrace.
 */
int
main(int argc, char *argv[])
{
	dtrace_consumer_t con;
	dtrace_prog_t *prog;
	dtrace_proginfo_t info;
	dtrace_hdl_t *dtp;
	FILE *fp = NULL;
	int c, err, partition = 0, ret = 0, script_argc = 0;
	char *args, *brokers, *rx_topic_name = NULL;
	char *tx_topic_name = NULL, *client_cert = NULL;
	char *ca_cert = NULL, *priv_key = NULL, *password = NULL;
	char **script_argv;
	int64_t start_offset = RD_KAFKA_OFFSET_STORED;
	useconds_t poll_period = 100000; /* 100ms */
	static struct option dtc_options[] = {
		{"brokers", required_argument, 0, 'b'},
		{"cacert", required_argument, NULL, 'a'},
		{"clientcert", required_argument, NULL, 'c'},
		{"debug", no_argument, NULL, 'd'},
		{"frombeginning", no_argument, NULL, 'f'},
		{"intopic", required_argument, NULL, 'i'},
		{"outtopic", required_argument, NULL, 'o'},
		{"password", required_argument, NULL, 'p'},
		{"poll", required_argument, NULL, 'q'},
		{"privkey", required_argument, NULL, 'k'},
		{"script", required_argument, NULL, 's'},
		{0, 0, 0, 0}
	};
	int fds[2];
	bool debug = false;

	g_pname = basename(argv[0]); 	

	/** Allocate space required for any arguments being passed to the
	 *  D-language script.
	 */
	script_argv = (char **) malloc(sizeof(char *) * argc);
	if (script_argv == NULL) {

		DLOGTR1(PRIO_HIGH,
		    "%s: failed to allocate script arguments\n", g_pname);
		exit(EXIT_FAILURE);
	}

	while ((c = getopt_long(argc, argv, "a:b:c:dfi:k:o:p:q:s:",
	    dtc_options, NULL)) != -1) {
		switch (c) {
		case 'a':
			/* CA certifcate file for TLS */
			ca_cert = optarg;
			break;
		case 'b':
			/* Kafka broker string */
			brokers = optarg;
			break;
		case 'c':
			/* Client certificate file for TLS */
			client_cert = optarg;
			break;
		case 'd':
			/* Debug flag */
			debug = true;
			break;
		case 'f':
			/* Kafla offset from beginning of topic */
			start_offset = RD_KAFKA_OFFSET_BEGINNING;
			break;
		case 'i':
			/* Kafla input topic */
			rx_topic_name = optarg;
			break;
		case 'k':
			/* Client private key file for TLS */
			priv_key = optarg;
			break;
		case 'o':
			/* Kafla output topic */
			tx_topic_name = optarg;
			break;
		case 'p':
			/* Client private key password for TLS */
			password = optarg;
			break;
		case 'q':
			/* Poll period (us) */
			sscanf(optarg, "%ul", &poll_period);
			break;
		case 's':
			/* DTrace script used to interpret the
			 * records within the kafka topic.
			 */
			if ((fp = fopen(optarg, "r")) == NULL) {

				DLOGTR2(PRIO_HIGH,
					"%s: failed to open script file "
					"%s\n", optarg, g_pname);
				ret = -1;
				goto free_script_args;
			}
			break;
		case '?':
			/* FALLTHROUGH */
		default:
			dtc_usage(stderr);
			ret = -1;
			goto free_script_args;
			break;
		}
	};
	
	/* Pass the remaining command line arguments to the DTrace script. */
	script_argv[script_argc++] = g_pname;
	while (optind < argc) {
		script_argv[script_argc++] = argv[optind++];
	}

	if (brokers == NULL || rx_topic_name == NULL || fp == NULL) {

		dtc_usage(stderr);
		ret = -1;
		goto free_script_args;
	}

	/* Daemonise */
	if (debug == false && daemon(0, 0) == -1) {

		DLOGTR0(PRIO_HIGH, "Failed registering dlogd as daemon\n");
		ret = -1;
		goto free_script_args;
	}
	
	DLOGTR1(PRIO_LOW, "%s daemon starting...\n", g_pname);

	if (dtc_register_daemon() != 0) {

		DLOGTR0(PRIO_HIGH, "Failed registering dlogd as daemon\n");
		ret = -1;
		goto free_script_args;
	}

	if (dtc_setup_rx_topic(rx_topic_name, brokers, ca_cert, client_cert,
	    priv_key, password) != 0){

		DLOGTR1(PRIO_HIGH, "Failed to setup receive topic %s\n",
		    rx_topic_name);
		ret = -1;
		goto free_script_args;
	}

	if (rd_kafka_consume_start(rx_topic, partition, start_offset) == -1) {

		DLOGTR2(PRIO_HIGH, "%s: failed to start consuming: %s\n",
		    g_pname, rd_kafka_err2str(rd_kafka_last_error()));
		if (errno == EINVAL) {
	        	DLOGTR1(PRIO_HIGH,
			    "%s: broker based offset storage "
			    "requires a group.id, "
			    "add: -X group.id=yourGroup\n", g_pname);
		}
		ret = -1;
		goto destroy_rx_kafka;
	}
	
	con.dc_consume_probe = chew;
	con.dc_consume_rec = chewrec;
	con.dc_put_buf = dtc_put_buf;
	con.dc_get_buf = dtc_get_buf;

	if ((dtp = dtrace_open(DTRACE_VERSION, 0, &err)) == NULL) {

		DLOGTR2(PRIO_HIGH, "%s: failed to initialize dtrace %s",
		    g_pname, dtrace_errmsg(dtp, dtrace_errno(dtp)));
		ret = -1;
		goto destroy_rx_kafka;
	}
	DLOGTR1(PRIO_LOW, "%s: dtrace initialized\n", g_pname);

	/* Configure dtrace.
	 * Trivially small buffers can be configured as trace collection
	 * does not occure locally.
	 * Destructive tracing prevents dtrace from being terminated
	 * (though this shouldn't happen as tracing is never enabled).
	 */
	(void) dtrace_setopt(dtp, "aggsize", "4k");
	(void) dtrace_setopt(dtp, "bufsize", "4k");
	(void) dtrace_setopt(dtp, "bufpolicy", "switch");
	(void) dtrace_setopt(dtp, "destructive", "1");
	DLOGTR1(PRIO_LOW, "%s: dtrace options set\n", g_pname);

	if ((prog = dtrace_program_fcompile(dtp, fp,
	    DTRACE_C_PSPEC | DTRACE_C_CPP, script_argc, script_argv)) == NULL) {

		DLOGTR2(PRIO_HIGH, "%s: failed to compile dtrace program %s",
		    g_pname, dtrace_errmsg(dtp, dtrace_errno(dtp)));
		ret = -1;
		goto destroy_dtrace;
	}
	DLOGTR1(PRIO_LOW, "%s: dtrace program compiled\n", g_pname);
	
	(void) fclose(fp);
	
	if (dtrace_program_exec(dtp, prog, &info) == -1) {

		DLOGTR2(PRIO_HIGH, "%s: failed to enable dtrace probes %s",
		    g_pname, dtrace_errmsg(dtp, dtrace_errno(dtp)));
		ret = -1;
		goto destroy_dtrace;
	}
	DLOGTR1(PRIO_LOW, "%s: dtrace probes enabled\n", g_pname);

	/* If the transmit topic name is configured create a new tranmitting
	 * topic and register a buffered handler to write to this
	 * topic with dtrace.
	 */
        if (tx_topic_name != NULL) {	
		if (dtc_setup_tx_topic(tx_topic_name, brokers, ca_cert,
		    client_cert, priv_key, password)
		    != 0) {
	
			ret = -1;
			goto destroy_dtrace;
		}

		if (dtrace_handle_buffered(dtp, dtc_buffered_handler,
		    tx_topic) == -1) {

			DLOGTR2(PRIO_HIGH,
			    "%s: failed registering dtrace "
			    "buffered handler %s",
			    g_pname, dtrace_errmsg(dtp, dtrace_errno(dtp)));
			ret = -1;
			goto destroy_tx_kafka;
		}
	}

	int done = 0;
	do {
		if (!done || !g_intr)
			usleep(poll_period);	

		if (done || g_intr) {
			done = 1;
		}

		/* Poll to handle delivery reports. */
		rd_kafka_poll(tx_rk, 0);
		rd_kafka_poll(rx_rk, 0);

		switch (dtrace_work_detached(dtp, NULL, &con, rx_topic)) {
		case DTRACE_WORKSTATUS_DONE:
			done = 1;
			break;
		case DTRACE_WORKSTATUS_OKAY:
			break;
		case DTRACE_WORKSTATUS_ERROR:
		default:
			if (dtrace_errno(dtp) != EINTR) 
				DLOGTR2(PRIO_HIGH, "%s : %s", g_pname,
				    dtrace_errmsg(dtp, dtrace_errno(dtp)));
				done = 1;
			break;
		}

	} while (!done);


destroy_tx_kafka:
	rd_kafka_flush(tx_rk, 10*1000);

	if (tx_topic_name != NULL) {	
		/* Destroy the Kafka transmit topic */
		DLOGTR1(PRIO_LOW, "%s: destroy kafka transmit topic\n",
		    g_pname);
		rd_kafka_topic_destroy(tx_topic);

		/* Destroy the Kafka transmit handle. */
		DLOGTR1(PRIO_LOW, "%s: destroy kafka transmit handle\n",
		    g_pname);
		rd_kafka_destroy(tx_rk);
	}

destroy_dtrace:
	/* Destroy dtrace the handle. */
	DLOGTR1(PRIO_LOW, "%s: closing dtrace\n", g_pname);
	dtrace_close(dtp);

destroy_rx_kafka:
	DLOGTR1(PRIO_LOW, "%s: destroy kafka receive topic\n", g_pname);

	rd_kafka_consume_stop(rx_topic, partition);

	/* Destroy the Kafka receive topic */
	rd_kafka_topic_destroy(rx_topic);

	/* Destroy the Kafka recieve handle. */
	DLOGTR1(PRIO_LOW, "%s: destroy kafka receive handle\n", g_pname);
	rd_kafka_destroy(rx_rk);

	/* Let background threads clean up and terminate cleanly. */
	int run = 5;
	while (run-- > 0 && rd_kafka_wait_destroyed(1000) == -1)
		printf("Waiting for librdkafka to decommission\n");
	if (run <= 0)
		rd_kafka_dump(stdout, rx_rk);

free_script_args:	
	/* Free the memory used to hold the script arguments. */	
	free(script_argv);

	return ret;
}
