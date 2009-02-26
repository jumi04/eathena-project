#ifndef _IF_MAP_H_
#define _IF_MAP_H_

#include "../common/mmo.h"

#define MAX_MAP_SERVERS 30

struct mmo_map_server
{
	int fd;
	uint32 ip;
	uint16 port;
	int users;
	unsigned short map[MAX_MAP_PER_SERVER];
};

extern struct mmo_map_server server[MAX_MAP_SERVERS];


extern int parse_frommap(int fd);

#endif /* _IF_MAP_H_ */