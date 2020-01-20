/*
 * dnstap/dtstream.h - Frame Streams implementation for unbound DNSTAP
 *
 * Copyright (c) 2020, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * \file
 *
 * An implementation of the Frame Streams data transport protocol for
 * the Unbound DNSTAP message logging facility.
 */

#ifndef DTSTREAM_H
#define DTSTREAM_H

#include "util/locks.h"
struct dt_msg_entry;
struct dt_io_list_item;

/**
 * A message buffer with dnstap messages queued up.  It is per-worker.
 * It has locks to synchronize.  If the buffer is full, a new message
 * cannot be added and is discarded.  A thread reads the messages and sends
 * them.
 */
struct dt_msg_queue {
	/** lock of the buffer structure.  Hold this lock to add or remove
	 * entries to the buffer.  Release it so that other threads can also
	 * put messages to log, or a message can be taken out to send away
	 * by the writer thread.
	 */
	lock_basic_type lock;
	/** the maximum size of the buffer, in bytes */
	size_t maxsize;
	/** current size of the buffer, in bytes.  data bytes of messages.
	 * If a new message make it more than maxsize, the buffer is full */
	size_t cursize;
	/** list of messages.  The messages are added to the back and taken
	 * out from the front. */
	struct dt_msg_entry* first, *last;
};

/** 
 * An entry in the dt_msg_queue. contains one DNSTAP message.
 * It is malloced.
 */
struct dt_msg_entry {
	/** next in the list. */
	struct dt_msg_entry* next;
	/** the buffer with the data to send, an encoded DNSTAP message */
	void* buf;
	/** the length to send. */
	size_t len;
};

/**
 * IO thread that reads from the queues and writes them.
 */
struct dt_io_thread {
	/** event base, for event handling */
	void* event_base;
	/** list of queues that is registered to get written */
	struct dt_io_list_item* io_list;
	/** file descriptor that the thread writes to */
	int fd;
	/** event structure that the thread uses */
	void* event;

	/** command pipe that stops the pipe if closed.  Used to quit
	 * the program. [0] is read, [1] is written to. */
	int commandpipe[2];
	/** the event to listen to the commandpipe */
	void* command_event;

	/** If the log server is connected to over unix domain sockets,
	 * eg. a file is named that is created to log onto. */
	int upstream_is_unix;
	/** if the log server is connected to over TCP.  The ip address and
	 * port are used */
	int upstream_is_tcp;
	/** if the log server is connected to over TLS.  ip address, port,
	 * and client certificates can be used for authentication. */
	int upstream_is_tls;

	/** the file path for unix socket (or NULL) */
	char* socket_path;
	/** the ip address and port number (or NULL) */
	char* ip_str;
	/** is the TLS upstream authenticated by name, if nonNULL,
	 * we use the same cert bundle as used by other TLS streams. */
	char* tls_server_name;
	/** are client certificates in use */
	int use_client_certs;
	/** client cert files: the .key file */
	char* client_key_file;
	/** client cert files: the .pem file */
	char* client_cert_file;
};

/* Frame Streams data transfer protocol encode for DNSTAP messages.
 * The protocol looks to be specified in the libfstrm library.
 */
/* routine to send START message. */
/* routine to send a frame. */
/* routine to send STOP message. */


#endif /* DTSTREAM_H */