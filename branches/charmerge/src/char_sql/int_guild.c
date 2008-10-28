
#define GS_MEMBER_UNMODIFIED 0x00
#define GS_MEMBER_MODIFIED 0x01
#define GS_MEMBER_NEW 0x02

#define GS_POSITION_UNMODIFIED 0x00
#define GS_POSITION_MODIFIED 0x01

// LSB = 0 => Alliance, LSB = 1 => Opposition
#define GUILD_ALLIANCE_TYPE_MASK 0x01
#define GUILD_ALLIANCE_REMOVE 0x08

static const char dataToHex[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

//Guild cache
static DBMap* guild_db_; // int guild_id -> struct guild*

struct guild_castle castles[MAX_GUILDCASTLE];

static unsigned int guild_exp[100];

int mapif_parse_GuildLeave(int fd,int guild_id,int account_id,int char_id,int flag,const char *mes);
int mapif_guild_broken(int guild_id,int flag);
static bool guild_check_empty(struct guild *g);
int guild_calcinfo(struct guild *g);
int mapif_guild_basicinfochanged(int guild_id,int type,const void *data,int len);
int mapif_guild_info(int fd,struct guild *g);
int guild_break_sub(int key,void *data,va_list ap);
int inter_guild_tosql(struct guild *g,int flag);

static int guild_save_timer(int tid, unsigned int tick, int id, intptr data)
{
	static int last_id = 0; //To know in which guild we were.
	int state = 0; //0: Have not reached last guild. 1: Reached last guild, ready for save. 2: Some guild saved, don't do further saving.
	DBIterator* iter;
	DBKey key;
	struct guild* g;

	if( last_id == 0 ) //Save the first guild in the list.
		state = 1;

	iter = guild_db_->iterator(guild_db_);
	for( g = (struct guild*)iter->first(iter,&key); iter->exists(iter); g = (struct guild*)iter->next(iter,&key) )
	{
		if( state == 0 && g->guild_id == last_id )
			state++; //Save next guild in the list.
		else
		if( state == 1 && g->save_flag&GS_MASK )
		{
			inter_guild_tosql(g, g->save_flag&GS_MASK);
			g->save_flag &= ~GS_MASK;

			//Some guild saved.
			last_id = g->guild_id;
			state++;
		}

		if( g->save_flag == GS_REMOVE )
		{// Nothing to save, guild is ready for removal.
			if (save_log)
				ShowInfo("Guild Unloaded (%d - %s)\n", g->guild_id, g->name);
			db_remove(guild_db_, key);
		}
	}
	iter->destroy(iter);

	if( state != 2 ) //Reached the end of the guild db without saving.
		last_id = 0; //Reset guild saved, return to beginning.

	state = guild_db_->size(guild_db_);
	if( state < 1 ) state = 1; //Calculate the time slot for the next save.
	add_timer(tick  + autosave_interval/state, guild_save_timer, 0, 0);
	return 0;
}

int inter_guild_removemember_tosql(int account_id, int char_id)
{
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE from `%s` where `account_id` = '%d' and `char_id` = '%d'", guild_member_db, account_id, char_id) )
		Sql_ShowDebug(sql_handle);
	if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `guild_id` = '0' WHERE `char_id` = '%d'", char_db, char_id) )
		Sql_ShowDebug(sql_handle);
	return 0;
}

// Save guild into sql
int inter_guild_tosql(struct guild *g,int flag)
{
	// Table guild (GS_BASIC_MASK)
	// GS_EMBLEM `emblem_len`,`emblem_id`,`emblem_data`
	// GS_CONNECT `connect_member`,`average_lv`
	// GS_MES `mes1`,`mes2`
	// GS_LEVEL `guild_lv`,`max_member`,`exp`,`next_exp`,`skill_point`
	// GS_BASIC `name`,`master`,`char_id`

	// GS_MEMBER `guild_member` (`guild_id`,`account_id`,`char_id`,`hair`,`hair_color`,`gender`,`class`,`lv`,`exp`,`exp_payper`,`online`,`position`,`name`)
	// GS_POSITION `guild_position` (`guild_id`,`position`,`name`,`mode`,`exp_mode`)
	// GS_ALLIANCE `guild_alliance` (`guild_id`,`opposition`,`alliance_id`,`name`)
	// GS_EXPULSION `guild_expulsion` (`guild_id`,`account_id`,`name`,`mes`)
	// GS_SKILL `guild_skill` (`guild_id`,`id`,`lv`) 

	// temporary storage for str convertion. They must be twice the size of the
	// original string to ensure no overflows will occur. [Skotlex]
	char t_info[256];
	char esc_name[NAME_LENGTH*2+1];
	char esc_master[NAME_LENGTH*2+1];
	char new_guild = 0;
	int i=0;

	if (g->guild_id<=0 && g->guild_id != -1) return 0;
	
	Sql_EscapeStringLen(sql_handle, esc_name, g->name, strnlen(g->name, NAME_LENGTH));
	Sql_EscapeStringLen(sql_handle, esc_master, g->master, strnlen(g->master, NAME_LENGTH));
	*t_info = '\0';

#ifndef TXT_SQL_CONVERT
	// Insert a new guild the guild
	if (flag&GS_BASIC && g->guild_id == -1)
	{
		strcat(t_info, " guild_create");

		// Create a new guild
		if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s` "
			"(`name`,`master`,`guild_lv`,`max_member`,`average_lv`,`char_id`) "
			"VALUES ('%s', '%s', '%d', '%d', '%d', '%d')",
			guild_db, esc_name, esc_master, g->guild_lv, g->max_member, g->average_lv, g->member[0].char_id) )
		{
			Sql_ShowDebug(sql_handle);
			if (g->guild_id == -1)
				return 0; //Failed to create guild!
		}
		else
		{
			g->guild_id = (int)Sql_LastInsertId(sql_handle);
			new_guild = 1;
		}
	}
#else
	// Insert a new guild the guild
	if (flag&GS_BASIC)
	{
		strcat(t_info, " guild_create");
		// Since the PK is guild id + master id, a replace will not be enough if we are overwriting data, we need to wipe the previous guild.
		if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` where `guild_id` = '%d'", guild_db, g->guild_id) )
			Sql_ShowDebug(sql_handle);
		// Create a new guild
		if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` "
			"(`guild_id`,`name`,`master`,`guild_lv`,`max_member`,`average_lv`,`char_id`) "
			"VALUES ('%d', '%s', '%s', '%d', '%d', '%d', '%d')",
			guild_db, g->guild_id, esc_name, esc_master, g->guild_lv, g->max_member, g->average_lv, g->member[0].char_id) )
		{
			Sql_ShowDebug(sql_handle);
			return 0; //Failed to create guild.
		}
	}
#endif //TXT_SQL_CONVERT

	// If we need an update on an existing guild or more update on the new guild
	if (((flag & GS_BASIC_MASK) && !new_guild) || ((flag & (GS_BASIC_MASK & ~GS_BASIC)) && new_guild))
	{
		StringBuf buf;
		bool add_comma = false;

		StringBuf_Init(&buf);
		StringBuf_Printf(&buf, "UPDATE `%s` SET ", guild_db);

		if (flag & GS_EMBLEM)
		{
			char emblem_data[sizeof(g->emblem_data)*2+1];
			char* pData = emblem_data;

			strcat(t_info, " emblem");
			// Convert emblem_data to hex
			//TODO: why not use binary directly? [ultramage]
			for(i=0; i<g->emblem_len; i++){
				*pData++ = dataToHex[(g->emblem_data[i] >> 4) & 0x0F];
				*pData++ = dataToHex[g->emblem_data[i] & 0x0F];
			}
			*pData = 0;
			StringBuf_Printf(&buf, "`emblem_len`=%d, `emblem_id`=%d, `emblem_data`='%s'", g->emblem_len, g->emblem_id, emblem_data);
			add_comma = true;
		}
		if (flag & GS_BASIC) 
		{
			strcat(t_info, " basic");
			if( add_comma )
				StringBuf_AppendStr(&buf, ", ");
			else
				add_comma = true;
			StringBuf_Printf(&buf, "`name`='%s', `master`='%s', `char_id`=%d", esc_name, esc_master, g->member[0].char_id);
		}
		if (flag & GS_CONNECT)
		{
			strcat(t_info, " connect");
			if( add_comma )
				StringBuf_AppendStr(&buf, ", ");
			else
				add_comma = true;
			StringBuf_Printf(&buf, "`connect_member`=%d, `average_lv`=%d", g->connect_member, g->average_lv);
		}
		if (flag & GS_MES)
		{
			char esc_mes1[sizeof(g->mes1)*2+1];
			char esc_mes2[sizeof(g->mes2)*2+1];

			strcat(t_info, " mes");
			if( add_comma )
				StringBuf_AppendStr(&buf, ", ");
			else
				add_comma = true;
			Sql_EscapeStringLen(sql_handle, esc_mes1, g->mes1, strnlen(g->mes1, sizeof(g->mes1)));
			Sql_EscapeStringLen(sql_handle, esc_mes2, g->mes2, strnlen(g->mes2, sizeof(g->mes2)));
			StringBuf_Printf(&buf, "`mes1`='%s', `mes2`='%s'", esc_mes1, esc_mes2);
		}
		if (flag & GS_LEVEL)
		{
			strcat(t_info, " level");
			if( add_comma )
				StringBuf_AppendStr(&buf, ", ");
			else
				add_comma = true;
			StringBuf_Printf(&buf, "`guild_lv`=%d, `skill_point`=%d, `exp`=%u, `next_exp`=%u, `max_member`=%d", g->guild_lv, g->skill_point, g->exp, g->next_exp, g->max_member);
		}
		StringBuf_Printf(&buf, " WHERE `guild_id`=%d", g->guild_id);
		if( SQL_ERROR == Sql_Query(sql_handle, "%s", StringBuf_Value(&buf)) )
			Sql_ShowDebug(sql_handle);
		StringBuf_Destroy(&buf);
	}

	if (flag&GS_MEMBER)
	{
		struct guild_member *m;

		strcat(t_info, " members");
		// Update only needed players
		for(i=0;i<g->max_member;i++){
			m = &g->member[i];
#ifndef TXT_SQL_CONVERT
			if (!m->modified)
				continue;
#endif
			if(m->account_id) {
				//Since nothing references guild member table as foreign keys, it's safe to use REPLACE INTO
				Sql_EscapeStringLen(sql_handle, esc_name, m->name, strnlen(m->name, NAME_LENGTH));
				if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`account_id`,`char_id`,`hair`,`hair_color`,`gender`,`class`,`lv`,`exp`,`exp_payper`,`online`,`position`,`name`) "
					"VALUES ('%d','%d','%d','%d','%d','%d','%d','%d','%u','%d','%d','%d','%s')",
					guild_member_db, g->guild_id, m->account_id, m->char_id,
					m->hair, m->hair_color, m->gender,
					m->class_, m->lv, m->exp, m->exp_payper, m->online, m->position, esc_name) )
					Sql_ShowDebug(sql_handle);
				if (m->modified & GS_MEMBER_NEW)
				{
					if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `guild_id` = '%d' WHERE `char_id` = '%d'",
						char_db, g->guild_id, m->char_id) )
						Sql_ShowDebug(sql_handle);
				}
				m->modified = GS_MEMBER_UNMODIFIED;
			}
		}
	}

	if (flag&GS_POSITION){
		strcat(t_info, " positions");
		//printf("- Insert guild %d to guild_position\n",g->guild_id);
		for(i=0;i<MAX_GUILDPOSITION;i++){
			struct guild_position *p = &g->position[i];
#ifndef TXT_SQL_CONVERT
			if (!p->modified)
				continue;
#endif
			Sql_EscapeStringLen(sql_handle, esc_name, p->name, strnlen(p->name, NAME_LENGTH));
			if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`position`,`name`,`mode`,`exp_mode`) VALUES ('%d','%d','%s','%d','%d')",
				guild_position_db, g->guild_id, i, esc_name, p->mode, p->exp_mode) )
				Sql_ShowDebug(sql_handle);
			p->modified = GS_POSITION_UNMODIFIED;
		}
	}

	if (flag&GS_ALLIANCE)
	{
		// Delete current alliances 
		// NOTE: no need to do it on both sides since both guilds in memory had 
		// their info changed, not to mention this would also mess up oppositions!
		// [Skotlex]
		//if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id`='%d' OR `alliance_id`='%d'", guild_alliance_db, g->guild_id, g->guild_id) )
		if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id`='%d'", guild_alliance_db, g->guild_id) )
		{
			Sql_ShowDebug(sql_handle);
		}
		else
		{
			//printf("- Insert guild %d to guild_alliance\n",g->guild_id);
			for(i=0;i<MAX_GUILDALLIANCE;i++)
			{
				struct guild_alliance *a=&g->alliance[i];
				if(a->guild_id>0)
				{
					Sql_EscapeStringLen(sql_handle, esc_name, a->name, strnlen(a->name, NAME_LENGTH));
					if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`opposition`,`alliance_id`,`name`) "
						"VALUES ('%d','%d','%d','%s')",
						guild_alliance_db, g->guild_id, a->opposition, a->guild_id, esc_name) )
						Sql_ShowDebug(sql_handle);
				}
			}
		}
	}

	if (flag&GS_EXPULSION){
		strcat(t_info, " expulsions");
		//printf("- Insert guild %d to guild_expulsion\n",g->guild_id);
		for(i=0;i<MAX_GUILDEXPULSION;i++){
			struct guild_expulsion *e=&g->expulsion[i];
			if(e->account_id>0){
				char esc_mes[sizeof(e->mes)*2+1];

				Sql_EscapeStringLen(sql_handle, esc_name, e->name, strnlen(e->name, NAME_LENGTH));
				Sql_EscapeStringLen(sql_handle, esc_mes, e->mes, strnlen(e->mes, sizeof(e->mes)));
				if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`account_id`,`name`,`mes`) "
					"VALUES ('%d','%d','%s','%s')", guild_expulsion_db, g->guild_id, e->account_id, esc_name, esc_mes) )
					Sql_ShowDebug(sql_handle);
			}
		}
	}

	if (flag&GS_SKILL){
		strcat(t_info, " skills");
		//printf("- Insert guild %d to guild_skill\n",g->guild_id);
		for(i=0;i<MAX_GUILDSKILL;i++){
			if (g->skill[i].id>0 && g->skill[i].lv>0){
				if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`id`,`lv`) VALUES ('%d','%d','%d')",
					guild_skill_db, g->guild_id, g->skill[i].id, g->skill[i].lv) )
					Sql_ShowDebug(sql_handle);
			}
		}
	}

	if (save_log)
		ShowInfo("Saved guild (%d - %s):%s\n",g->guild_id,g->name,t_info);
	return 1;
}

// Read guild from sql
struct guild * inter_guild_fromsql(int guild_id)
{
	struct guild *g;
	char* data;
	size_t len;
	char* p;
	int i;

	if( guild_id <= 0 )
		return NULL;

	g = (struct guild*)idb_get(guild_db_, guild_id);
	if( g )
		return g;

	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `name`,`master`,`guild_lv`,`connect_member`,`max_member`,`average_lv`,`exp`,`next_exp`,`skill_point`,`mes1`,`mes2`,`emblem_len`,`emblem_id`,`emblem_data` "
		"FROM `%s` WHERE `guild_id`='%d'", guild_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		return NULL;
	}

	if( SQL_SUCCESS != Sql_NextRow(sql_handle) )
		return NULL;// Guild does not exists.

	CREATE(g, struct guild, 1);

	g->guild_id = guild_id;
	Sql_GetData(sql_handle,  0, &data, &len); memcpy(g->name, data, min(len, NAME_LENGTH));
	Sql_GetData(sql_handle,  1, &data, &len); memcpy(g->master, data, min(len, NAME_LENGTH));
	Sql_GetData(sql_handle,  2, &data, NULL); g->guild_lv = atoi(data);
	Sql_GetData(sql_handle,  3, &data, NULL); g->connect_member = atoi(data);
	Sql_GetData(sql_handle,  4, &data, NULL); g->max_member = atoi(data);
	if( g->max_member > MAX_GUILD )
	{	// Fix reduction of MAX_GUILD [PoW]
		ShowWarning("Guild %d:%s specifies higher capacity (%d) than MAX_GUILD (%d)\n", guild_id, g->name, g->max_member, MAX_GUILD);
		g->max_member = MAX_GUILD;
	}
	Sql_GetData(sql_handle,  5, &data, NULL); g->average_lv = atoi(data);
	Sql_GetData(sql_handle,  6, &data, NULL); g->exp = (unsigned int)strtoul(data, NULL, 10);
	Sql_GetData(sql_handle,  7, &data, NULL); g->next_exp = (unsigned int)strtoul(data, NULL, 10);
	Sql_GetData(sql_handle,  8, &data, NULL); g->skill_point = atoi(data);
	Sql_GetData(sql_handle,  9, &data, &len); memcpy(g->mes1, data, min(len, sizeof(g->mes1)));
	Sql_GetData(sql_handle, 10, &data, &len); memcpy(g->mes2, data, min(len, sizeof(g->mes2)));
	Sql_GetData(sql_handle, 11, &data, &len); g->emblem_len = atoi(data);
	Sql_GetData(sql_handle, 12, &data, &len); g->emblem_id = atoi(data);
	Sql_GetData(sql_handle, 13, &data, &len);
	// convert emblem data from hexadecimal to binary
	//TODO: why not store it in the db as binary directly? [ultramage]
	for( i = 0, p = g->emblem_data; i < g->emblem_len; ++i, ++p )
	{
		if( *data >= '0' && *data <= '9' )
			*p = *data - '0';
		else if( *data >= 'a' && *data <= 'f' )
			*p = *data - 'a' + 10;
		else if( *data >= 'A' && *data <= 'F' )
			*p = *data - 'A' + 10;
		*p <<= 4;
		++data;

		if( *data >= '0' && *data <= '9' )
			*p |= *data - '0';
		else if( *data >= 'a' && *data <= 'f' )
			*p |= *data - 'a' + 10;
		else if( *data >= 'A' && *data <= 'F' )
			*p |= *data - 'A' + 10;
		++data;
	}

	// load guild member info
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id`,`char_id`,`hair`,`hair_color`,`gender`,`class`,`lv`,`exp`,`exp_payper`,`online`,`position`,`name` "
		"FROM `%s` WHERE `guild_id`='%d' ORDER BY `position`", guild_member_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}
	for( i = 0; i < g->max_member && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		struct guild_member* m = &g->member[i];

		Sql_GetData(sql_handle,  0, &data, NULL); m->account_id = atoi(data);
		Sql_GetData(sql_handle,  1, &data, NULL); m->char_id = atoi(data);
		Sql_GetData(sql_handle,  2, &data, NULL); m->hair = atoi(data);
		Sql_GetData(sql_handle,  3, &data, NULL); m->hair_color = atoi(data);
		Sql_GetData(sql_handle,  4, &data, NULL); m->gender = atoi(data);
		Sql_GetData(sql_handle,  5, &data, NULL); m->class_ = atoi(data);
		Sql_GetData(sql_handle,  6, &data, NULL); m->lv = atoi(data);
		Sql_GetData(sql_handle,  7, &data, NULL); m->exp = (unsigned int)strtoul(data, NULL, 10);
		Sql_GetData(sql_handle,  8, &data, NULL); m->exp_payper = (unsigned int)atoi(data);
		Sql_GetData(sql_handle,  9, &data, NULL); m->online = atoi(data);
		Sql_GetData(sql_handle, 10, &data, NULL); m->position = atoi(data);
		if( m->position >= MAX_GUILDPOSITION ) // Fix reduction of MAX_GUILDPOSITION [PoW]
			m->position = MAX_GUILDPOSITION - 1;
		Sql_GetData(sql_handle, 11, &data, &len); memcpy(m->name, data, min(len, NAME_LENGTH));
		m->modified = GS_MEMBER_UNMODIFIED;
	}

	//printf("- Read guild_position %d from sql \n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `position`,`name`,`mode`,`exp_mode` FROM `%s` WHERE `guild_id`='%d'", guild_position_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}
	while( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		int position;
		struct guild_position* p;

		Sql_GetData(sql_handle, 0, &data, NULL); position = atoi(data);
		if( position < 0 || position >= MAX_GUILDPOSITION )
			continue;// invalid position
		p = &g->position[position];
		Sql_GetData(sql_handle, 1, &data, &len); memcpy(p->name, data, min(len, NAME_LENGTH));
		Sql_GetData(sql_handle, 2, &data, NULL); p->mode = atoi(data);
		Sql_GetData(sql_handle, 3, &data, NULL); p->exp_mode = atoi(data);
		p->modified = GS_POSITION_UNMODIFIED;
	}

	//printf("- Read guild_alliance %d from sql \n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `opposition`,`alliance_id`,`name` FROM `%s` WHERE `guild_id`='%d'", guild_alliance_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}
	for( i = 0; i < MAX_GUILDALLIANCE && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		struct guild_alliance* a = &g->alliance[i];

		Sql_GetData(sql_handle, 0, &data, NULL); a->opposition = atoi(data);
		Sql_GetData(sql_handle, 1, &data, NULL); a->guild_id = atoi(data);
		Sql_GetData(sql_handle, 2, &data, &len); memcpy(a->name, data, min(len, NAME_LENGTH));
	}

	//printf("- Read guild_expulsion %d from sql \n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id`,`name`,`mes` FROM `%s` WHERE `guild_id`='%d'", guild_expulsion_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}
	for( i = 0; i < MAX_GUILDEXPULSION && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		struct guild_expulsion *e = &g->expulsion[i];

		Sql_GetData(sql_handle, 0, &data, NULL); e->account_id = atoi(data);
		Sql_GetData(sql_handle, 1, &data, &len); memcpy(e->name, data, min(len, NAME_LENGTH));
		Sql_GetData(sql_handle, 2, &data, &len); memcpy(e->mes, data, min(len, sizeof(e->mes)));
	}

	//printf("- Read guild_skill %d from sql \n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `id`,`lv` FROM `%s` WHERE `guild_id`='%d' ORDER BY `id`", guild_skill_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}

	for(i = 0; i < MAX_GUILDSKILL; i++)
	{	//Skill IDs must always be initialized. [Skotlex]
		g->skill[i].id = i + GD_SKILLBASE;
	}

	while( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		int id;
		Sql_GetData(sql_handle, 0, &data, NULL); id = atoi(data) - GD_SKILLBASE;
		if( id < 0 && id >= MAX_GUILDSKILL )
			continue;// invalid guild skill
		Sql_GetData(sql_handle, 1, &data, NULL); g->skill[id].lv = atoi(data);
	}
	Sql_FreeResult(sql_handle);

	idb_put(guild_db_, guild_id, g); //Add to cache
	g->save_flag |= GS_REMOVE; //But set it to be removed, in case it is not needed for long.
	
	if (save_log)
		ShowInfo("Guild loaded (%d - %s)\n", guild_id, g->name);

	return g;
}

int inter_guildcastle_tosql(struct guild_castle *gc)
{
	// `guild_castle` (`castle_id`, `guild_id`, `economy`, `defense`, `triggerE`, `triggerD`, `nextTime`, `payTime`, `createTime`, `visibleC`, `visibleG0`, `visibleG1`, `visibleG2`, `visibleG3`, `visibleG4`, `visibleG5`, `visibleG6`, `visibleG7`)

	if (gc==NULL) return 0;
	#ifdef GUILD_DEBUG
ShowDebug("Save guild_castle (%d)\n", gc->castle_id);
	#endif

//	sql_query("DELETE FROM `%s` WHERE `castle_id`='%d'",guild_castle_db, gc->castle_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` "
		"(`castle_id`, `guild_id`, `economy`, `defense`, `triggerE`, `triggerD`, `nextTime`, `payTime`, `createTime`,"
		"`visibleC`, `visibleG0`, `visibleG1`, `visibleG2`, `visibleG3`, `visibleG4`, `visibleG5`, `visibleG6`, `visibleG7`)"
		"VALUES ('%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d','%d')",
		guild_castle_db, gc->castle_id, gc->guild_id,  gc->economy, gc->defense, gc->triggerE, gc->triggerD, gc->nextTime, gc->payTime, gc->createTime, gc->visibleC,
		gc->guardian[0].visible, gc->guardian[1].visible, gc->guardian[2].visible, gc->guardian[3].visible, gc->guardian[4].visible, gc->guardian[5].visible, gc->guardian[6].visible, gc->guardian[7].visible) )
		Sql_ShowDebug(sql_handle);

#ifndef TXT_SQL_CONVERT
	memcpy(&castles[gc->castle_id],gc,sizeof(struct guild_castle));
#endif //TXT_SQL_CONVERT
	return 0;
}

// Read guild_castle from sql
int inter_guildcastle_fromsql(int castle_id,struct guild_castle *gc)
{
	static int castles_init=0;
	char* data;

	if (gc==NULL) return 0;
	if (castle_id==-1) return 0;

	if(!castles_init)
	{
		int i;
		for(i=0;i<MAX_GUILDCASTLE;i++)
			castles[i].castle_id=-1;
		castles_init = 1;
	}

	if(castles[castle_id].castle_id == castle_id)
	{
		memcpy(gc,&castles[castle_id],sizeof(struct guild_castle));
		return 1;
	}

	memset(gc,0,sizeof(struct guild_castle));
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `castle_id`, `guild_id`, `economy`, `defense`, `triggerE`, `triggerD`, `nextTime`, `payTime`, `createTime`, "
		"`visibleC`, `visibleG0`, `visibleG1`, `visibleG2`, `visibleG3`, `visibleG4`, `visibleG5`, `visibleG6`, `visibleG7`"
		" FROM `%s` WHERE `castle_id`='%d'", guild_castle_db, castle_id) )
	{
		Sql_ShowDebug(sql_handle);
		return 0;
	}
	// ARU: This needs to be set even if there are no SQL results
	gc->castle_id=castle_id;
	if( SQL_SUCCESS != Sql_NextRow(sql_handle) )
	{
		Sql_FreeResult(sql_handle);
		return 1; //Assume empty castle.
	}

	Sql_GetData(sql_handle,  1, &data, NULL); gc->guild_id =  atoi(data);
	Sql_GetData(sql_handle,  2, &data, NULL); gc->economy = atoi(data);
	Sql_GetData(sql_handle,  3, &data, NULL); gc->defense = atoi(data);
	Sql_GetData(sql_handle,  4, &data, NULL); gc->triggerE = atoi(data);
	Sql_GetData(sql_handle,  5, &data, NULL); gc->triggerD = atoi(data);
	Sql_GetData(sql_handle,  6, &data, NULL); gc->nextTime = atoi(data);
	Sql_GetData(sql_handle,  7, &data, NULL); gc->payTime = atoi(data);
	Sql_GetData(sql_handle,  8, &data, NULL); gc->createTime = atoi(data);
	Sql_GetData(sql_handle,  9, &data, NULL); gc->visibleC = atoi(data);
	Sql_GetData(sql_handle, 10, &data, NULL); gc->guardian[0].visible = atoi(data);
	Sql_GetData(sql_handle, 11, &data, NULL); gc->guardian[1].visible = atoi(data);
	Sql_GetData(sql_handle, 12, &data, NULL); gc->guardian[2].visible = atoi(data);
	Sql_GetData(sql_handle, 13, &data, NULL); gc->guardian[3].visible = atoi(data);
	Sql_GetData(sql_handle, 14, &data, NULL); gc->guardian[4].visible = atoi(data);
	Sql_GetData(sql_handle, 15, &data, NULL); gc->guardian[5].visible = atoi(data);
	Sql_GetData(sql_handle, 16, &data, NULL); gc->guardian[6].visible = atoi(data);
	Sql_GetData(sql_handle, 17, &data, NULL); gc->guardian[7].visible = atoi(data);

	Sql_FreeResult(sql_handle);
	memcpy(&castles[castle_id],gc,sizeof(struct guild_castle));

	if( save_log )
		ShowInfo("Loaded Castle %d (guild %d)\n", castle_id, gc->guild_id);

	return 1;
}


// Read exp_guild.txt
int inter_guild_ReadEXP(void)
{
	int i;
	FILE *fp;
	char line[1024];
	for (i=0;i<100;i++) guild_exp[i]=0;

	sprintf(line, "%s/exp_guild.txt", db_path);
	fp=fopen(line,"r");
	if(fp==NULL){
		ShowError("can't read %s\n", line);
		return 1;
	}
	i=0;
	while(fgets(line, sizeof(line), fp) && i < 100)
	{
		if(line[0]=='/' && line[1]=='/')
			continue;
		guild_exp[i]=(unsigned int)atof(line);
		i++;
	}
	fclose(fp);

	return 0;
}


int inter_guild_CharOnline(int char_id, int guild_id)
{
   struct guild *g;
   int i;
   
	if (guild_id == -1) {
		//Get guild_id from the database
		if( SQL_ERROR == Sql_Query(sql_handle, "SELECT guild_id FROM `%s` WHERE char_id='%d'", char_db, char_id) )
		{
			Sql_ShowDebug(sql_handle);
			return 0;
		}

		if( SQL_SUCCESS == Sql_NextRow(sql_handle) )
		{
			char* data;

			Sql_GetData(sql_handle, 0, &data, NULL);
			guild_id = atoi(data);
		}
		else
		{
			guild_id = 0;
		}
		Sql_FreeResult(sql_handle);
	}
	if (guild_id == 0)
		return 0; //No guild...
	
	g = inter_guild_fromsql(guild_id);
	if(!g) {
		ShowError("Character %d's guild %d not found!\n", char_id, guild_id);
		return 0;
	}

	//Member has logged in before saving, tell saver not to delete
	if(g->save_flag & GS_REMOVE)
		g->save_flag &= ~GS_REMOVE;

	//Set member online
	ARR_FIND( 0, g->max_member, i, g->member[i].char_id == char_id );
	if( i < g->max_member )
	{
		g->member[i].online = 1;
		g->member[i].modified = GS_MEMBER_MODIFIED;
	}

	return 1;
}

int inter_guild_CharOffline(int char_id, int guild_id)
{
	struct guild *g=NULL;
	int online_count, i;

	if (guild_id == -1)
	{
		//Get guild_id from the database
		if( SQL_ERROR == Sql_Query(sql_handle, "SELECT guild_id FROM `%s` WHERE char_id='%d'", char_db, char_id) )
		{
			Sql_ShowDebug(sql_handle);
			return 0;
		}

		if( SQL_SUCCESS == Sql_NextRow(sql_handle) )
		{
			char* data;

			Sql_GetData(sql_handle, 0, &data, NULL);
			guild_id = atoi(data);
		}
		else
		{
			guild_id = 0;
		}
		Sql_FreeResult(sql_handle);
	}
	if (guild_id == 0)
		return 0; //No guild...
	
	//Character has a guild, set character offline and check if they were the only member online
	g = inter_guild_fromsql(guild_id);
	if (g == NULL) //Guild not found?
		return 0;

	//Set member offline
	ARR_FIND( 0, g->max_member, i, g->member[i].char_id == char_id );
	if( i < g->max_member )
	{
		g->member[i].online = 0;
		g->member[i].modified = GS_MEMBER_MODIFIED;
	}

	online_count = 0;
	for( i = 0; i < g->max_member; i++ )
		if( g->member[i].online )
			online_count++;

	// Remove guild from memory if no players online
	if( online_count == 0 )
		g->save_flag |= GS_REMOVE;

	return 1;
}

// Initialize guild sql
int inter_guild_init(void)
{
	//Initialize the guild cache
	guild_db_= idb_alloc(DB_OPT_RELEASE_DATA);

   //Read exp file
	inter_guild_ReadEXP();
   
	add_timer_func_list(guild_save_timer, "guild_save_timer");
	add_timer(gettick() + 10000, guild_save_timer, 0, 0);
	return 0;
}

static int guild_db_final(DBKey key, void *data, va_list ap)
{
	struct guild *g = (struct guild*)data;
	if (g->save_flag&GS_MASK) {
		inter_guild_tosql(g, g->save_flag&GS_MASK);
		return 1;
	}
	return 0;
}

void inter_guild_final(void)
{
	guild_db_->destroy(guild_db_, guild_db_final);
	return;
}

// Get guild_id by its name. Returns 0 if not found, -1 on error.
int search_guildname(char *str)
{
	int guild_id;
	char esc_name[NAME_LENGTH*2+1];
	
	Sql_EscapeStringLen(sql_handle, esc_name, str, safestrnlen(str, NAME_LENGTH));
	//Lookup guilds with the same name
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT guild_id FROM `%s` WHERE name='%s'", guild_db, esc_name) )
	{
		Sql_ShowDebug(sql_handle);
		return -1;
	}

	if( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		char* data;

		Sql_GetData(sql_handle, 0, &data, NULL);
		guild_id = atoi(data);
	}
	else
	{
		guild_id = 0;
	}
	Sql_FreeResult(sql_handle);
	return guild_id;
}

// Check if guild is empty
static bool guild_check_empty(struct guild *g)
{
	int i;
	ARR_FIND( 0, g->max_member, i, g->member[i].account_id > 0 );
	if( i < g->max_member)
		return false; // not empty

	//Let the calling function handle the guild removal in case they need
	//to do something else with it before freeing the data. [Skotlex]
	return true;
}

unsigned int guild_nextexp(int level)
{
	if (level == 0)
		return 1;
	if (level < 100 && level > 0) // Change by hack
		return guild_exp[level-1];

	return 0;
}

int guild_checkskill(struct guild *g,int id)
{
	int idx = id - GD_SKILLBASE;

	if(idx < 0 || idx >= MAX_GUILDSKILL)
		return 0;

	return g->skill[idx].lv;
}

int guild_calcinfo(struct guild *g)
{
	int i,c;
	unsigned int nextexp;
	struct guild before = *g; // Save guild current values

	if(g->guild_lv<=0)
		g->guild_lv = 1;
	nextexp = guild_nextexp(g->guild_lv);

	// Consume guild exp and increase guild level
	while(g->exp >= nextexp && nextexp > 0){	//fixed guild exp overflow [Kevin]
		g->exp-=nextexp;
		g->guild_lv++;
		g->skill_point++;
		nextexp = guild_nextexp(g->guild_lv);
	}

	// Save next exp step
	g->next_exp = nextexp;

	// Set the max number of members, Guild Extention skill - currently adds 6 to max per skill lv.
	g->max_member = 16 + guild_checkskill(g, GD_EXTENSION) * 6; 
	if(g->max_member > MAX_GUILD)
	{	
		ShowError("Guild %d:%s has capacity for too many guild members (%d), max supported is %d\n", g->guild_id, g->name, g->max_member, MAX_GUILD);
		g->max_member = MAX_GUILD;
	}
	
	// Compute the guild average level level
	g->average_lv=0;
	g->connect_member=0;
	for(i=c=0;i<g->max_member;i++)
	{
		if(g->member[i].account_id>0)
		{
			if (g->member[i].lv >= 0)
			{
				g->average_lv+=g->member[i].lv;
				c++;
			}
			else
			{
				ShowWarning("Guild %d:%s, member %d:%s has an invalid level %d\n", g->guild_id, g->name, g->member[i].char_id, g->member[i].name, g->member[i].lv);
			}

			if(g->member[i].online)
				g->connect_member++;
		}
	}
	if(c)
		g->average_lv /= c;

	// Check if guild stats has change
	if(g->max_member != before.max_member || g->guild_lv != before.guild_lv || g->skill_point != before.skill_point	)
	{
		g->save_flag |= GS_LEVEL;
		mapif_guild_info(-1,g);
		return 1;
	}

	return 0;
}

//-------------------------------------------------------------------
// Packet sent to map server

int mapif_guild_created(int fd,int account_id,struct guild *g)
{
	WFIFOHEAD(fd, 10);
	WFIFOW(fd,0)=0x3830;
	WFIFOL(fd,2)=account_id;
	if(g != NULL)
	{
		WFIFOL(fd,6)=g->guild_id;
		ShowInfo("int_guild: Guild created (%d - %s)\n",g->guild_id,g->name);
	} else
		WFIFOL(fd,6)=0;

	WFIFOSET(fd,10);
	return 0;
}

// Guild not found
int mapif_guild_noinfo(int fd,int guild_id)
{
	unsigned char buf[12];
	WBUFW(buf,0)=0x3831;
	WBUFW(buf,2)=8;
	WBUFL(buf,4)=guild_id;
	ShowWarning("int_guild: info not found %d\n",guild_id);
	if(fd<0)
		mapif_sendall(buf,8);
	else
		mapif_send(fd,buf,8);
	return 0;
}

// Send guild info
int mapif_guild_info(int fd,struct guild *g)
{
	unsigned char buf[8+sizeof(struct guild)];
	WBUFW(buf,0)=0x3831;
	WBUFW(buf,2)=4+sizeof(struct guild);
	memcpy(buf+4,g,sizeof(struct guild));
	if(fd<0)
		mapif_sendall(buf,WBUFW(buf,2));
	else
		mapif_send(fd,buf,WBUFW(buf,2));
	return 0;
}

// ACK member add
int mapif_guild_memberadded(int fd,int guild_id,int account_id,int char_id,int flag)
{
	WFIFOHEAD(fd, 15);
	WFIFOW(fd,0)=0x3832;
	WFIFOL(fd,2)=guild_id;
	WFIFOL(fd,6)=account_id;
	WFIFOL(fd,10)=char_id;
	WFIFOB(fd,14)=flag;
	WFIFOSET(fd,15);
	return 0;
}

// ACK member leave
int mapif_guild_leaved(int guild_id,int account_id,int char_id,int flag, const char *name, const char *mes)
{
	unsigned char buf[55+NAME_LENGTH];
	WBUFW(buf, 0)=0x3834;
	WBUFL(buf, 2)=guild_id;
	WBUFL(buf, 6)=account_id;
	WBUFL(buf,10)=char_id;
	WBUFB(buf,14)=flag;
	memcpy(WBUFP(buf,15),mes,40);
	memcpy(WBUFP(buf,55),name,NAME_LENGTH);
	mapif_sendall(buf,55+NAME_LENGTH);
	ShowInfo("int_guild: guild leaved (%d - %d: %s - %s)\n",guild_id,account_id,name,mes);
	return 0;
}

// Send short member's info
int mapif_guild_memberinfoshort(struct guild *g,int idx)
{
	unsigned char buf[19];
	WBUFW(buf, 0)=0x3835;
	WBUFL(buf, 2)=g->guild_id;
	WBUFL(buf, 6)=g->member[idx].account_id;
	WBUFL(buf,10)=g->member[idx].char_id;
	WBUFB(buf,14)=(unsigned char)g->member[idx].online;
	WBUFW(buf,15)=g->member[idx].lv;
	WBUFW(buf,17)=g->member[idx].class_;
	mapif_sendall(buf,19);
	return 0;
}

// Send guild broken
int mapif_guild_broken(int guild_id,int flag)
{
	unsigned char buf[7];
	WBUFW(buf,0)=0x3836;
	WBUFL(buf,2)=guild_id;
	WBUFB(buf,6)=flag;
	mapif_sendall(buf,7);
	ShowInfo("int_guild: Guild broken (%d)\n",guild_id);
	return 0;
}

// Send guild message
int mapif_guild_message(int guild_id,int account_id,char *mes,int len, int sfd)
{
	unsigned char buf[512];
	if (len > 500)
		len = 500;
	WBUFW(buf,0)=0x3837;
	WBUFW(buf,2)=len+12;
	WBUFL(buf,4)=guild_id;
	WBUFL(buf,8)=account_id;
	memcpy(WBUFP(buf,12),mes,len);
	mapif_sendallwos(sfd, buf,len+12);
	return 0;
}

// Send basic info
int mapif_guild_basicinfochanged(int guild_id,int type,const void *data,int len)
{
	unsigned char buf[2048];
	if (len > 2038)
		len = 2038;
	WBUFW(buf, 0)=0x3839;
	WBUFW(buf, 2)=len+10;
	WBUFL(buf, 4)=guild_id;
	WBUFW(buf, 8)=type;
	memcpy(WBUFP(buf,10),data,len);
	mapif_sendall(buf,len+10);
	return 0;
}

// Send member info
int mapif_guild_memberinfochanged(int guild_id,int account_id,int char_id, int type,const void *data,int len)
{
	unsigned char buf[2048];
	if (len > 2030)
		len = 2030;
	WBUFW(buf, 0)=0x383a;
	WBUFW(buf, 2)=len+18;
	WBUFL(buf, 4)=guild_id;
	WBUFL(buf, 8)=account_id;
	WBUFL(buf,12)=char_id;
	WBUFW(buf,16)=type;
	memcpy(WBUFP(buf,18),data,len);
	mapif_sendall(buf,len+18);
	return 0;
}

// ACK guild skill up
int mapif_guild_skillupack(int guild_id,int skill_num,int account_id)
{
	unsigned char buf[14];
	WBUFW(buf, 0)=0x383c;
	WBUFL(buf, 2)=guild_id;
	WBUFL(buf, 6)=skill_num;
	WBUFL(buf,10)=account_id;
	mapif_sendall(buf,14);
	return 0;
}

// ACK guild alliance
int mapif_guild_alliance(int guild_id1,int guild_id2,int account_id1,int account_id2,int flag,const char *name1,const char *name2)
{
	unsigned char buf[19+2*NAME_LENGTH];
	WBUFW(buf, 0)=0x383d;
	WBUFL(buf, 2)=guild_id1;
	WBUFL(buf, 6)=guild_id2;
	WBUFL(buf,10)=account_id1;
	WBUFL(buf,14)=account_id2;
	WBUFB(buf,18)=flag;
	memcpy(WBUFP(buf,19),name1,NAME_LENGTH);
	memcpy(WBUFP(buf,19+NAME_LENGTH),name2,NAME_LENGTH);
	mapif_sendall(buf,19+2*NAME_LENGTH);
	return 0;
}

// Send a guild position desc
int mapif_guild_position(struct guild *g,int idx)
{
	unsigned char buf[12 + sizeof(struct guild_position)];
	WBUFW(buf,0)=0x383b;
	WBUFW(buf,2)=sizeof(struct guild_position)+12;
	WBUFL(buf,4)=g->guild_id;
	WBUFL(buf,8)=idx;
	memcpy(WBUFP(buf,12),&g->position[idx],sizeof(struct guild_position));
	mapif_sendall(buf,WBUFW(buf,2));
	return 0;
}

// Send the guild notice
int mapif_guild_notice(struct guild *g)
{
	unsigned char buf[256];
	WBUFW(buf,0)=0x383e;
	WBUFL(buf,2)=g->guild_id;
	memcpy(WBUFP(buf,6),g->mes1,60);
	memcpy(WBUFP(buf,66),g->mes2,120);
	mapif_sendall(buf,186);
	return 0;
}

// Send emblem data
int mapif_guild_emblem(struct guild *g)
{
	unsigned char buf[12 + sizeof(g->emblem_data)];
	WBUFW(buf,0)=0x383f;
	WBUFW(buf,2)=g->emblem_len+12;
	WBUFL(buf,4)=g->guild_id;
	WBUFL(buf,8)=g->emblem_id;
	memcpy(WBUFP(buf,12),g->emblem_data,g->emblem_len);
	mapif_sendall(buf,WBUFW(buf,2));
	return 0;
}

int mapif_guild_master_changed(struct guild *g, int aid, int cid)
{
	unsigned char buf[14];
	WBUFW(buf,0)=0x3843;
	WBUFL(buf,2)=g->guild_id;
	WBUFL(buf,6)=aid;
	WBUFL(buf,10)=cid;
	mapif_sendall(buf,14);
	return 0;
}

int mapif_guild_castle_dataload(int castle_id,int index,int value)
{
	unsigned char buf[9];
	WBUFW(buf, 0)=0x3840;
	WBUFW(buf, 2)=castle_id;
	WBUFB(buf, 4)=index;
	WBUFL(buf, 5)=value;
	mapif_sendall(buf,9);
	return 0;
}

int mapif_guild_castle_datasave(int castle_id,int index,int value)
{
	unsigned char buf[9];
	WBUFW(buf, 0)=0x3841;
	WBUFW(buf, 2)=castle_id;
	WBUFB(buf, 4)=index;
	WBUFL(buf, 5)=value;
	mapif_sendall(buf,9);
	return 0;
}

int mapif_guild_castle_alldataload(int fd)
{
	struct guild_castle s_gc;
	struct guild_castle* gc = &s_gc;
	int i;
	int len;
	char* data;

	WFIFOHEAD(fd, 4 + MAX_GUILDCASTLE*sizeof(struct guild_castle));
	WFIFOW(fd, 0) = 0x3842;
	if( SQL_ERROR == Sql_Query(sql_handle,
		"SELECT `castle_id`, `guild_id`, `economy`, `defense`, `triggerE`, `triggerD`, `nextTime`, `payTime`, `createTime`,"
		" `visibleC`, `visibleG0`, `visibleG1`, `visibleG2`, `visibleG3`, `visibleG4`, `visibleG5`, `visibleG6`, `visibleG7`"
		" FROM `%s` ORDER BY `castle_id`", guild_castle_db) )
		Sql_ShowDebug(sql_handle);
	for( i = 0, len = 4; i < MAX_GUILDCASTLE && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		memset(gc, 0, sizeof(struct guild_castle));

		Sql_GetData(sql_handle,  0, &data, NULL); gc->castle_id = atoi(data);
		Sql_GetData(sql_handle,  1, &data, NULL); gc->guild_id = atoi(data);
		Sql_GetData(sql_handle,  2, &data, NULL); gc->economy = atoi(data);
		Sql_GetData(sql_handle,  3, &data, NULL); gc->defense = atoi(data);
		Sql_GetData(sql_handle,  4, &data, NULL); gc->triggerE = atoi(data);
		Sql_GetData(sql_handle,  5, &data, NULL); gc->triggerD = atoi(data);
		Sql_GetData(sql_handle,  6, &data, NULL); gc->nextTime = atoi(data);
		Sql_GetData(sql_handle,  7, &data, NULL); gc->payTime = atoi(data);
		Sql_GetData(sql_handle,  8, &data, NULL); gc->createTime = atoi(data);
		Sql_GetData(sql_handle,  9, &data, NULL); gc->visibleC = atoi(data);
		Sql_GetData(sql_handle, 10, &data, NULL); gc->guardian[0].visible = atoi(data);
		Sql_GetData(sql_handle, 11, &data, NULL); gc->guardian[1].visible = atoi(data);
		Sql_GetData(sql_handle, 12, &data, NULL); gc->guardian[2].visible = atoi(data);
		Sql_GetData(sql_handle, 13, &data, NULL); gc->guardian[3].visible = atoi(data);
		Sql_GetData(sql_handle, 14, &data, NULL); gc->guardian[4].visible = atoi(data);
		Sql_GetData(sql_handle, 15, &data, NULL); gc->guardian[5].visible = atoi(data);
		Sql_GetData(sql_handle, 16, &data, NULL); gc->guardian[6].visible = atoi(data);
		Sql_GetData(sql_handle, 17, &data, NULL); gc->guardian[7].visible = atoi(data);

		memcpy(WFIFOP(fd, len), gc, sizeof(struct guild_castle));
		len += sizeof(struct guild_castle);
	}
	Sql_FreeResult(sql_handle);
	WFIFOW(fd, 2) = len;
	WFIFOSET(fd, len);
	
	return 0;
}