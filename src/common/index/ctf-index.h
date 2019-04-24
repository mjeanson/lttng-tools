/*
 * Copyright (C) 2013 Julien Desfossez <jdesfossez@efficios.com>
 * Copyright (C) 2013 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (C) 2013 David Goulet <dgoulet@efficios.com>
 *
 * SPDX-License-Identifier: MIT
 *
 */

#ifndef LTTNG_INDEX_H
#define LTTNG_INDEX_H

#include <limits.h>

#define CTF_INDEX_MAGIC 0xC1F1DCC1
#define CTF_INDEX_MAJOR 1
#define CTF_INDEX_MINOR 1

/*
 * Header at the beginning of each index file.
 * All integer fields are stored in big endian.
 */
struct ctf_packet_index_file_hdr {
	uint32_t magic;
	uint32_t index_major;
	uint32_t index_minor;
	/* struct packet_index_len, in bytes */
	uint32_t packet_index_len;
} __attribute__((__packed__));

/*
 * Packet index generated for each trace packet stored in a trace file.
 * All integer fields are stored in big endian.
 */
struct ctf_packet_index {
	uint64_t offset;		/* offset of the packet in the file, in bytes */
	uint64_t packet_size;		/* packet size, in bits */
	uint64_t content_size;		/* content size, in bits */
	uint64_t timestamp_begin;
	uint64_t timestamp_end;
	uint64_t events_discarded;
	uint64_t stream_id;		/* ID of the channel */
	/* CTF_INDEX 1.0 limit */
	uint64_t stream_instance_id;	/* ID of the channel instance */
	uint64_t packet_seq_num;	/* packet sequence number */
} __attribute__((__packed__));

static inline size_t ctf_packet_index_len(uint32_t major, uint32_t minor)
{
	if (major == 1) {
		switch (minor) {
		case 0:
			return offsetof(struct ctf_packet_index, stream_id)
				+ member_sizeof(struct ctf_packet_index,
						stream_id);
		case 1:
			return offsetof(struct ctf_packet_index, packet_seq_num)
				+ member_sizeof(struct ctf_packet_index,
						packet_seq_num);
		default:
			abort();
		}
	}
	abort();
}

static inline uint32_t lttng_to_index_major(uint32_t lttng_major,
		uint32_t lttng_minor __attribute__((unused)))
{
	if (lttng_major == 2) {
		return 1;
	}
	abort();
}

static inline uint32_t lttng_to_index_minor(uint32_t lttng_major,
		uint32_t lttng_minor)
{
	if (lttng_major == 2) {
		if (lttng_minor < 8) {
			return 0;
		} else {
			return 1;
		}
	}
	abort();
}

#endif /* LTTNG_INDEX_H */
