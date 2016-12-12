#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "net.h"
#include "common.h"
#include "hash.h"
#include "debug.h"

#include "client.h"

#include <time.h>

#define CMD_PUT 1
#define CMD_GET 2
#define CMD_PRINT 3
#define CMD_LIST 4
#define CMD_DEBUG 99

/**
 * Yahaloo,
 *
 * To whomever is reading this file, here’s the basic organisation of most of
 * the client’s code:
 *
 *   - all functions that are used to send a specific request with specific data
 *     are called send_<request_name>.
 *     To understand them, you want to look at their matching structure in
 *     `common.h`.
 *   - all functions that are used to parse a specific request and take action
 *     based on its content are called handle_<request_name>.
 *     Again, you’ll want to look at `common.h`. You may also need to take a
 *     look at the project’s subject to understand the behavioral’s details.
 *
 * event_loop() is where most of the action takes place. It’s the main loop,
 * and basically all incoming requests are handled in it.
 */

int
send_ec_str(struct net* net, const char* str)
{
	char buffer[1024];
	RequestEC* r = (void*) buffer;

	r->type = REQUEST_EC;
	r->subtype = 0;

	int len = snprintf((void*) r->data, sizeof(buffer) - sizeof(*r), "%s", str);

	r->size = len;

	net_write(net, r, sizeof(*r) + r->size, 0);

	return 0;
}

int
send_list(
	const unsigned char * hash,
	RegisteredFile * rf,
	vec_void_t* connected_clients
)
{
	if (!hash)
		return NET_FAIL;

	int ret = 0;

	SegmentClient * client;

	if ( rf->related_clients.length < 1 ) {
		orz("No clients for \"%s\"", hash_data_schar(hash));
		return -1;
	}

	client = rf->related_clients.data[rand() % rf->related_clients.length];

	int sin_family = client->v4.ipv == 6 ? AF_INET : AF_INET6;

	char address[64];
	inet_ntop(sin_family, client->v6.address, address, sizeof(address));
	orz("LIST>");
	orz(" - hash: %s - ", hash_data_schar(hash));
	orz(" - ipv%d, %s:%d - ", client->v4.ipv == 6 ? 4 : 6, address, ntohs(client->v4.port));

	struct net* peer = NULL;
	int already_registered = 0;
	int i;
	vec_foreach (connected_clients, peer, i) {
		int equal = peer->addr.v4.sin_family == sin_family;
		equal = equal && peer->addr.v4.sin_port == client->v4.port;
		equal = equal && (0 == memcmp(&peer->addr.v4.sin_addr, &client->v4.address, client->v4.ipv == 6 ? 4 : 16));

		if (equal) {
			already_registered = 1;

			break;
		}
	}

	if (!already_registered) {
		peer = malloc(sizeof(*peer));

		int error = net_init_raw(
			peer,
			client->v4.port,
			(void*) &client->v4.address,
			NET_CLIENT,
			(client->v4.ipv == 6) ? NET_IPV4 : NET_IPV6
		);

		net_error(error);
	}

	struct timeval t;
	t.tv_sec  = 1;
	t.tv_usec = 0;

	unsigned char buffer[sizeof(RequestList)];
	RequestList * rq = (void*) buffer;

	rq->type = REQUEST_LIST;

	rq->hash.c    = 50;
	rq->hash.size = 32;
	memcpy(
		rq->hash.hash,
		hash,
		32
	);

	send_ec_str(peer, "PING");

	ret = net_write( peer, rq, sizeof(*rq), 0);
	orz(" - wrote %d - ", ret);

	if (!already_registered)
		vec_push(connected_clients, peer);

	return ret;
}

void
handle_list ( char * buffer, vec_void_t * registered_files , struct net * server, vec_void_t * connected_clients )
{
	orz("HANDLE LIST");
	RequestList    * rq = (void*) buffer;
	RequestListAck * rp = (void*) buffer;

	int i;
	RegisteredFile * rf;
	int check = 0;

	if ( rq->hash.c != 50 )
	{
		wtf("LIST> bad file hash %s", hash_data_schar(rq->hash.hash));
		send_ec_str(server, "Bad file Hash !");

		return;
	}

	vec_foreach ( registered_files, rf, i)
	{
		for ( int j = 0 ; j < 32 ; j++ )
		{
			check = memcmp( rf->hash_data->digest, rq->hash.hash, 32 );	
		}
	}
	if ( check != 0 )
	{
		wtf("LIST> we don't have %s", hash_data_schar(rq->hash.hash));
		send_ec_str(server, "No list !");
		return;
	}

	char address[64];
	address[0] = 0x00;
	inet_ntop(
		server->version == NET_IPV4 ? AF_INET : AF_INET6,
		&server->current->v4.sin_addr,
		address,
		sizeof(address)
	);

	rf = registered_files->data[i-1];

	rp->type = REQUEST_LIST_ACK;
	rp->size = rf->hash_data->chunkDigests.length;
	unsigned char * hash;

	vec_foreach( &rf->hash_data->chunkDigests, hash, i )
	{
		rp->data[i].c    = 51;
		rp->data[i].size = 32;
		memcpy(
			rp->data[i].hash,
			hash,
			32
		);
		rp->data[i].index = i;	
	}

	srsly("addresse %s port %d", address, ntohs(server->current->v4.sin_port));
	net_write( server, rp, sizeof(*rp) + ((i+1) * sizeof(SegmentChunkHash)) , 0 );
}

void handle_list_ack ( char * buffer, vec_void_t * registered_files )
{
	RequestListAck * rq = (void*) buffer;

	if ( rq->file_hash_segment.c != 50 )
	{
		orz("LIST/ACK<<Bad file hash segment !");
		return;
	}

	short size = rq->size;
	if ( size <= 0 )
	{
		orz("LIST/ACK<< Bad number of file hash (%d)", size);
		return;
	}

	srsly("LIST/ACK<< :D !!!!");
	//mode get client
}

int
send_get_cli ( struct net* net, const HashData* hd, short index )
{
	if ( index >= hd->chunkDigests.length || index < 0 )
		return -1;

	int err = 0;

	char buffer[sizeof(RequestGetClient)];
	RequestGetClient * rq = (void*) buffer;

	rq->type   = 100;

	//hash file
	rq->file_hash_segment.c    = 50;
	rq->file_hash_segment.size = 32;
	memcpy(
		rq->file_hash_segment.hash,
		hd->digest,
		32
	);

	//chunk hash
	rq->chunk_hash_segment.c     = 51;
	rq->chunk_hash_segment.index = index; 
	memcpy(
		rq->chunk_hash_segment.hash,
		hd->chunkDigests.data[index],
		32
	);

	net_write(net, (void*)rq, sizeof(*rq), 0);

	return err;
}

int
send_put(struct net* net, struct net* srv, const HashData* hd)
{
	int err = 0;
	char buffer[sizeof(RequestPut)];
	RequestPut* rq = (void*) buffer;

	rq->type = REQUEST_PUT;

	rq->hash_segment.c = 50;
	rq->hash_segment.size = 32;
	memcpy((void*) rq->hash_segment.hash, hd->digest, sizeof(rq->hash_segment.hash));

	rq->client_segment.v4.c = 55;
	if (net->version == NET_IPV4)
		rq->client_segment.v4.ipv = 6;
	else
		rq->client_segment.v4.ipv = 18;

	struct sockaddr_in* saddr = (struct sockaddr_in*) &net->addr;
	rq->client_segment.v4.port = srv->addr.v4.sin_port;

	memcpy(
		(void*) rq->client_segment.v6.address,
		&saddr->sin_addr,
		net->version == 4 ? 4 : 16
	);

	net_write(net, (void*) rq, sizeof(*rq), 0);

	return err;
}


int
send_keep_alive(struct net* net, const HashData * hd)
{
	int err = 0;
	char buffer[sizeof(RequestKeepAlive)];
	RequestPut* rq = (void*) buffer;

	rq->type = REQUEST_KEEP_ALIVE;

	rq->hash_segment.c = 50;
	rq->hash_segment.size = 32;
	memcpy((void*) rq->hash_segment.hash, hd->digest, sizeof(rq->hash_segment.hash));

	net_write(net, (void*) rq, sizeof(*rq), 0);

	return err;
}

void
send_get(struct net* tracker, struct net* server, const HashData* hd)
{
	RequestGet r;
	struct sockaddr_in6* saddr = &server->addr.v6;

	r.type = REQUEST_GET;

	r.hash_segment.c = 50;
	r.hash_segment.size = 32;
	memcpy(&r.hash_segment.hash, hd->digest, 32);

	r.client_segment.v4.c = 55;
	r.client_segment.v4.ipv = server->version == 4 ? 6 : 18;
	r.client_segment.v6.port = saddr->sin6_port;
	memcpy(&r.client_segment.v6.address, (void*) &saddr->sin6_addr,
		server->version == 4 ? 4 : 16);

	net_write(tracker, (void*) &r, sizeof(r), 0);
}

const char*
status_string(RegisteredFile* rf)
{
	switch (rf->status) {
		case STATUS_PUT:
			return "PUT";
		case STATUS_KEEP_ALIVE:
			return "KEEP-ALIVE";
		case STATUS_GET:
			return "GET";
		case STATUS_LIST:
			return "LIST";
	}
}



void
handle_timeout(struct net* tracker, struct net* server, vec_void_t* registered_files, vec_void_t* connected_clients)
{
	int i;
	RegisteredFile* rf;

	vec_foreach(registered_files, rf, i) {
		if (--rf->timeout == 0) {
			switch (rf->status) {
				case STATUS_PUT:
					srsly("PUT> %s", hash_data_schar(rf->hash_data->digest));
					send_put(tracker, server, rf->hash_data);
					rf->timeout = 5;
					break;
				case STATUS_KEEP_ALIVE:
					orz("PUT> %s", hash_data_schar(rf->hash_data->digest));
					send_put(tracker, server, rf->hash_data);
					rf->status = STATUS_PUT;
					rf->timeout = 5;
					break;
				case STATUS_GET:
					srsly("GET> %s", hash_data_schar(rf->hash_data->digest));
					send_get(tracker, server, rf->hash_data);
					rf->timeout = 5;
					break;
				case STATUS_LIST:
					srsly("LIST> %s", hash_data_schar(rf->hash_data->digest));
					send_list( rf->hash_data->digest, rf, connected_clients );
					break;
			}
		} else if (rf->status == STATUS_KEEP_ALIVE) {
			if (rf->timeout <= 30 && rf->timeout % 5 == 0) {
				srsly("KEEP-ALIVE> %s", hash_data_schar(rf->hash_data->digest));
				send_keep_alive(tracker, rf->hash_data);
			}
		}
	}
}

void
handle_ec(char* buffer, int size)
{
	RequestEC* r = (void*) buffer;

	if ((unsigned) size < sizeof(*r) || (unsigned) size < sizeof(*r) + r->size) {
		orz("received broken EC, datagram was too short");

		return;	
	}

	if (r->subtype == 0) {
		char c = '\n';
		write(1, r->data, r->size);
		write(1, &c, 1);
	} else {
		wtf("Received unhandled EC type.");
	}
}

void
handle_put_ack(char* buffer, int size, vec_void_t* registered_files)
{
	RequestPutAck* r = (void*) buffer;
	int i;
	RegisteredFile* rf;

	if ((unsigned) size < sizeof(*r)) {
		orz("received broken PUT/ACK, datagram too short");

		return;
	}

	vec_foreach (registered_files, rf, i) {
		if (!memcmp(rf->hash_data->digest, r->hash_segment.hash, 32)) {
			rf->timeout = 60;
			rf->status = STATUS_KEEP_ALIVE;

			return;
		}
	}
}

void
handle_put_error(char* buffer, int size, vec_void_t* registered_files)
{
	RequestPutError* r = (void*) buffer;
	int i;
	RegisteredFile* rf;

	if ((unsigned) size < sizeof(*r)) {
		orz("received broken PUT/ERROR, datagram too short");

		return;
	}

	vec_foreach (registered_files, rf, i) {
		if (!memcmp(rf->hash_data->digest, r->hash_segment.hash, 32)) {
			wtf("PUT/ERROR << %s", hash_data_schar(r->hash_segment.hash));

			rf->timeout = 60;
			rf->status = STATUS_KEEP_ALIVE;

			return;
		}
	}
}

void
handle_keep_alive_ack(char* buffer, int size, vec_void_t* registered_files)
{
	RequestKeepAliveAck* r = (void*) buffer;
	int i;
	RegisteredFile* rf;

	if ((unsigned) size < sizeof(*r)) {
		orz("received broken KEEP-ALIVE/ACK, datagram too short");

		return;
	}

	vec_foreach (registered_files, rf, i) {
		if (!memcmp(rf->hash_data->digest, r->hash_segment.hash, 32)) {
			rf->timeout = 60;
			rf->status = STATUS_KEEP_ALIVE;

			return;
		}
	}
}

void
handle_keep_alive_error(struct net* tracker, struct net* server, char* buffer, int size, vec_void_t* registered_files)
{
	RequestKeepAliveError* r = (void*) buffer;
	int i;
	RegisteredFile* rf;

	if ((unsigned) size < sizeof(*r)) {
		orz("received broken KEEP-ALIVE/ERROR, datagram too short");

		return;
	}

	vec_foreach (registered_files, rf, i) {
		if (!memcmp(rf->hash_data->digest, r->hash_segment.hash, 32)) {
			send_put(tracker, server, rf->hash_data);

			rf->timeout = 60;
			rf->status = STATUS_PUT;

			return;
		}
	}
}

void
handle_get_ack( char* buffer, int count, vec_void_t* registered_files, vec_void_t* connected_clients)
{
	RequestGetAck* r = (void*) buffer;
	int i;
	RegisteredFile* rf;

	if ((unsigned) count < sizeof(*r)) {
		orz("received broken GET/ACK, datagram too short");
		return;
	}

	count -= sizeof(*r);

	srsly("GET/ACK <<");
	srsly(" - hash: %s - ", hash_data_schar(r->hash_segment.hash));

	vec_foreach (registered_files, rf, i) {
		if (!memcmp(rf->hash_data->digest, r->hash_segment.hash, 32)) {
			char* current_offset;
			int j;
			SegmentClient* s;

			vec_foreach_rev (&rf->related_clients, s, j) {
				free(s);

				vec_pop(&rf->related_clients);
			}

			current_offset = (char*) r->clients;

			for (; r->count > 0; r->count--) {

				s = (SegmentClient*) current_offset;

				size_t segment_size =
					s->v4.ipv == 6 ? sizeof(SegmentClient4) : sizeof(SegmentClient6);

				count -= segment_size;

				if (count < 0) {
					orz("broken GET/ACK received and partly handled");
					break;
				}

				void* clone = malloc(segment_size);

				memcpy(clone, current_offset, segment_size);

				current_offset += segment_size;

				vec_push(&rf->related_clients, clone);

				char address[64];
				inet_ntop(
					s->v4.ipv == 6 ? AF_INET : AF_INET6,
					(void*) &s->v4.address, address, sizeof(address)
				);

				srsly(" - new pair: %s:%d - ", address, ntohs(s->v4.port));
			}

			rf->timeout = 30;

			send_list( rf->hash_data->digest, rf, connected_clients );
			return;
		}
	}
}

void
handle_get_client(struct net* net, char* buffer, int count, vec_void_t* registered_files)
{
	RegisteredFile* rf;
	RequestGetClient* r = (void*) buffer;
	int i;

	if ((unsigned) count < sizeof(*r)) {
		orz(" - received broken GET-CLIENT - ");
		return;
	}

	vec_foreach (registered_files, rf, i) {
		if (!memcmp(rf->hash_data->digest, r->chunk_hash_segment.hash, 32)) {
			RequestGetClientAck* answer = (void*) buffer;

			srsly("GET-CLIENT> %s", rf->filename);

			FILE* f = fopen(rf->filename, "r");

			if (!f) {
				orz("fopen");
				return;
			}

			fseek(f, r->chunk_hash_segment.index, SEEK_SET);

			answer->fragment.index = answer->chunk_hash_segment.index;

			/* FIXME: does not wurk, update once handle_list_ack is done */
			for (int j = 0; j < CHUNK_SIZE / FRAGMENT_SIZE; j++) {
				int r = fread(answer->fragment.data, FRAGMENT_SIZE, 1, f);

				answer->fragment.c = 60;
				answer->fragment.size = r;
				answer->fragment.index += 1000;

				net_write(net, answer, sizeof(*answer) + answer->fragment.size, 0);
			}
		}
	}

	r->type = REQUEST_GET_CLIENT_ACK;

	return;
}

int
event_loop(struct net* net, struct net* srv, vec_void_t* registered_files, uint16_t tracker_port )
{
	char buffer[CHUNK_SIZE * 2];

	vec_void_t connected_clients;

	struct timeval t;
	t.tv_sec  = 0;
	t.tv_usec = 0;

	vec_init(&connected_clients);

	net_set_timeout(net, &t);
	net_set_timeout(srv, &t);

	for (int i = 0;; i++)
	{
		int count;

		count = net_read_vec(net,srv, &connected_clients, buffer, sizeof(buffer), 0);

		if (count < 0) {
			orz("something bad happened");
			net_error(count);
		} else if (count == 0) { /* timeout */
			handle_timeout(net, srv, registered_files, &connected_clients);

			t.tv_sec = 1;
		} else {
			RequestType type = buffer[0];

			switch (type) {
				case REQUEST_EC:
					handle_ec(buffer, count);
					break;
				case REQUEST_PUT_ACK:
					handle_put_ack(buffer, count, registered_files);
					break;
				case REQUEST_PUT_ERROR:
					handle_put_error(buffer, count, registered_files);
					break;
				case REQUEST_KEEP_ALIVE_ACK:
					handle_keep_alive_ack(buffer, count, registered_files);
					break;
				case REQUEST_KEEP_ALIVE_ERROR:
					handle_keep_alive_error(net, srv, buffer, count, registered_files);
					break;
				case REQUEST_GET_ACK:
					handle_get_ack(buffer, count, registered_files, &connected_clients);
					break;
				case REQUEST_LIST:
					handle_list(buffer, registered_files, srv, &connected_clients);
					break;
				case REQUEST_LIST_ACK:
					handle_list_ack(buffer, registered_files );
					break;
				default:
					orz("Unknown request type [%d]", type);
					break;
			}
		}
	}

	int i;
	struct net* client;
	vec_foreach (&connected_clients, client, i) {
		free(client);
	}
	vec_deinit(&connected_clients);

	return 0;
}

void
put_file(vec_void_t* files, const char* filename)
{
	RegisteredFile* rf = malloc(sizeof(*rf));

	strncpy(rf->filename, filename, PATH_MAX - 1);

	rf->status = STATUS_PUT;

	rf->timeout = 1;

	rf->hash_data = hash_data_new(filename);

	vec_push(files, rf);

	srsly("registered for PUT > %s", filename);
}

void
get_file(vec_void_t* files, const char* digest, const char* filename)
{
	RegisteredFile* rf = malloc(sizeof(*rf));

	strncpy(rf->filename, filename, PATH_MAX - 1);

	rf->status = STATUS_GET;

	rf->timeout = 1;

	rf->hash_data = malloc(sizeof(HashData));
	rf->hash_data->filename = strdup(filename);

	rf->hash_data->digest = malloc(32);
	memset(rf->hash_data->digest, 0, 32);
	for (int i = 0; i < 32; i++) {
		if (digest[i] == '\0')
			break;

		unsigned int digit;

		sscanf(digest + 2 * i, "%02x", &digit);

		rf->hash_data->digest[i] = digit;
	}

	vec_init(&rf->hash_data->chunkDigests);

	vec_init(&rf->related_clients);

	vec_push(files, rf);

	srsly("registered for GET > %s", filename);
}

/**
 * Hand-made parameters parsing.
 *
 * Read the man page to know what it parses or not.
 *
 * Short option syntax is supported, but won’t be of use considering that all
 * possible options require a parameter.
 *
 * TODO: Don’t hardcode stuff!
 */
int
parse_arg(
	int argc, const char* argv[],
	uint16_t* port, uint16_t* listening_port,
	const char** destination,
	vec_void_t* registered_files
)
{
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (argv[i][1] == '-') {
				if (!strcmp(argv[i], "--put")) {
					if ((i + 1) < argc) {
						put_file(registered_files, argv[++i]);
					} else {
						orz("--put expects a file name");

						return 1;
					}
				} else if (!strcmp(argv[i], "--get")) {
					if ((i + 2) < argc) {
						get_file(registered_files, argv[i+1], argv[i+2]);

						i += 2;
					} else {
						orz("--get expects a hash and a file name");

						return 1;
					}
				} else if (!strcmp(argv[i], "--port")) {
					if ((i + 1) < argc) {
						(*port) = atol(argv[i+1]);

						i++;
					} else {
						orz("--port expects a port number");

						return 1;
					}
				} else if (!strcmp(argv[i], "--listening-port")) {
					if ((i + 1) < argc) {
						(*listening_port) = atol(argv[i+1]);

						i++;
					} else {
						orz("--listening-port expects a port number");

						return 1;
					}
				} else if (!strcmp(argv[i], "--host")) {
					if ((i + 1) < argc) {
						(*destination) = argv[i+1];

						i++;
					} else {
						orz("--host expects a hostname or address");

						return 1;
					}
				}
			} else {
				for (int j = 1; argv[i][j]; j++) {
					if (argv[i][j] == 'p') {
						if (argv[i][j + 1] == '\0' && (i + 1) < argc) {
							(*port) = atol(argv[i+1]);

							i++;

							break;
						} else {
							orz("-p expects a port number");

							return 1;
						}
					} else if (argv[i][j] == 'l') {
						if (argv[i][j + 1] == '\0' && (i + 1) < argc) {
							(*listening_port) = atol(argv[i+1]);

							i++;

							break;
						} else {
							orz("-l expects a port number");

							return 1;
						}
					} else if (argv[i][j] == 'h') {
						if (argv[i][j + 1] == '\0' && (i + 1) < argc) {
							(*destination) = argv[i+1];

							i++;

							break;
						} else {
							orz("-h expects a hostname or address");

							return 1;
						}
					}
				}
			}
		} else {
			orz("unexpected extra operand: %s", argv[i]);
			return 1;
		}
	}

	return 0;
}

int
main ( int argc, const char* argv[] )
{
	srand(time(NULL));
	uint16_t tracker_port = 9000;
	uint16_t peers_port = 9001;
	const char* destination = NULL;

	vec_void_t registered_files;

	vec_init(&registered_files);

	if (0 != parse_arg(argc, argv, &tracker_port, &peers_port, &destination, &registered_files))
		return 1;

	/* TODO: Is a default destination required? */
	if (!destination) {
		orz("no destination set, use -h <addr>");

		return 1;
	}

	/* `net` will be the connection to the tracker,
	 * whereas `srv` will be the listening socket for incoming connections from
	 * other peers */
	struct net net;
	struct net srv;

	int err = net_init(&net, tracker_port, destination, NET_CLIENT, NET_IPV4);
	net_error(err);
	err = net_init(&srv, peers_port, "0.0.0.0", NET_SERVER, NET_IPV4);
	net_error(err);

	event_loop(&net, &srv, &registered_files, tracker_port);

	/* Memory freeing, obviously. */
	for (int i = 0; i < registered_files.length; i++) {
		RegisteredFile* rf = registered_files.data[i];

		hash_data_free(rf->hash_data);

		for (int j = 0; j < rf->related_clients.length; j++)
			free(rf->related_clients.data[j]);

		for (int j = 0; j < rf->received_fragments.length; j++)
			free(rf->related_clients.data[j]);

		free(registered_files.data[i]);
	}
	vec_deinit(&registered_files);

	return 0;
}

