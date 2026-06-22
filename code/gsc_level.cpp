#include "gsc_level.hpp"

extern dvar_t *sv_maxclients;

#if COMPILE_LEVEL == 1

static bool helpers_is_active_player(gentity_t *ent)
{
	gclient_t *client = ent->client;
	if ( !ent->r.inuse || client == NULL )
		return false;
	if ( client->sess.connected != CON_CONNECTED )
		return false;
	if ( client->sess.sessionState != STATE_PLAYING )
		return false;
	if ( ent->health <= 0 )
		return false;
	return true;
}

void gsc_level_getmovers()
{
	gentity_t *ent = g_entities;
	int i;

	stackPushArray();
	for ( i = 0; i < level.num_entities; i++, ent++ )
	{
		if ( ent->s.eType == ET_SCRIPTMOVER )
		{
			stackPushEntity(ent);
			stackPushArrayLast();
		}
	}
}

void gsc_level_getnumberofstaticmodels()
{
	stackPushInt(cm.numStaticModels);
}

void gsc_level_getstaticmodelname()
{
	int index;

	if ( !stackGetParams("i", &index) )
	{
		stackError("gsc_level_getstaticmodelname() argument is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	if ( index < 0 || index >= (int)cm.numStaticModels )
	{
		stackError("gsc_level_getstaticmodelname() index is out of range");
		stackPushUndefined();
		return;
	}

	stackPushString(cm.staticModelList[index].xmodel->name);
}

void gsc_level_getstaticmodelorigin()
{
	int index;

	if ( !stackGetParams("i", &index) )
	{
		stackError("gsc_level_getstaticmodelorigin() argument is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	if ( index < 0 || index >= (int)cm.numStaticModels )
	{
		stackError("gsc_level_getstaticmodelorigin() index is out of range");
		stackPushUndefined();
		return;
	}

	stackPushVector(cm.staticModelList[index].origin);
}

void gsc_level_getentitycount()
{
	int inUseOnly;
	gentity_t *ent;
	int i;
	int entities = 0;

	if ( !stackGetParams("i", &inUseOnly) )
	{
		inUseOnly = 0;
	}

	if ( inUseOnly )
	{
		ent = g_entities;
		for ( i = 0; i < level.num_entities; i++, ent++ )
		{
			if ( ent->r.inuse != 0 )
			{
				entities++;
			}
		}
		stackPushInt(entities);
	}
	else
	{
		stackPushInt(level.num_entities);
	}
}

void gsc_level_getsavepersist()
{
	stackPushBool(level.savepersist);
}

void gsc_level_setsavepersist()
{
	int save;

	if ( !stackGetParams("i",  &save) )
	{
		stackError("gsc_utils_setsavepersist() argument is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	level.savepersist = save;

	stackPushBool(qtrue);
}

void gsc_level_setnorthyaw()
{
	float fYaw;

	if ( !stackGetParams("f", &fYaw) )
	{
		stackError("gsc_level_setnorthyaw() one or more arguments is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	int len = snprintf(NULL, 0, "%g", fYaw);
	if ( len <= 0 || len >= MAX_STRINGLENGTH )
	{
		stackError("gsc_level_setnorthyaw() one or more arguments is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	char *szYaw = (char *)Z_MallocInternal(len + 1);
	snprintf(szYaw, len + 1, "%g", fYaw);
	SV_SetConfigstring(11, szYaw);
	Z_FreeInternal(szYaw);

	stackPushBool(qtrue);
}

void gsc_level_getclosestplayerbyvieworigininrange()
{
	int args = Scr_GetNumParam();

	if ( args < 1 || Scr_GetType(0) != VAR_VECTOR )
	{
		stackError("gsc_level_getclosestplayerbyvieworigininrange() requires origin (vector)");
		stackPushUndefined();
		return;
	}

	vec3_t origin;
	Scr_GetVector(0, origin);

	float maxDistSq = 0.0f;
	bool hasMaxDist = false;
	if ( args > 1 && Scr_GetType(1) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(1) != VAR_FLOAT && Scr_GetType(1) != VAR_INTEGER )
		{
			stackError("gsc_level_getclosestplayerbyvieworigininrange() maxDistSq must be a number");
			stackPushUndefined();
			return;
		}
		maxDistSq = Scr_GetFloat(1);
		if ( maxDistSq < 0.0f )
		{
			stackError("gsc_level_getclosestplayerbyvieworigininrange() maxDistSq must be >= 0");
			stackPushUndefined();
			return;
		}
		hasMaxDist = true;
	}

	int filterTeam = -1;
	if ( args > 2 && Scr_GetType(2) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(2) != VAR_INTEGER )
		{
			stackError("gsc_level_getclosestplayerbyvieworigininrange() filterTeam must be an int");
			stackPushUndefined();
			return;
		}
		filterTeam = Scr_GetInt(2);
	}

	int traceContentMask = 0;
	bool hasTraceCheck = false;
	if ( args > 3 && Scr_GetType(3) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(3) != VAR_INTEGER )
		{
			stackError("gsc_level_getclosestplayerbyvieworigininrange() traceContentMask must be an int");
			stackPushUndefined();
			return;
		}
		traceContentMask = Scr_GetInt(3);
		hasTraceCheck = true;
	}

	int maxClients = sv_maxclients->current.integer;
	gentity_t *best = NULL;
	float bestDistSq = hasMaxDist ? maxDistSq : 0.0f;
	bool found = false;

	for ( int i = 0; i < maxClients; i++ )
	{
		gentity_t *ent = &g_entities[i];
		if ( !helpers_is_active_player(ent) )
			continue;
		if ( filterTeam >= 0 && ent->client->sess.cs.team != filterTeam )
			continue;

		vec3_t viewOrigin;
		G_GetPlayerViewOrigin(ent, viewOrigin);

		float dx = viewOrigin[0] - origin[0];
		float dy = viewOrigin[1] - origin[1];
		float dz = viewOrigin[2] - origin[2];
		float d2 = dx * dx + dy * dy + dz * dz;
		if ( hasMaxDist && d2 > maxDistSq )
			continue;

		if ( hasTraceCheck && !G_LocationalTracePassed(origin, viewOrigin, ent->s.number, traceContentMask) )
			continue;

		if ( !found || d2 < bestDistSq )
		{
			best = ent;
			bestDistSq = d2;
			found = true;
		}
	}

	if ( found )
		stackPushEntity(best);
	else
		stackPushUndefined();
}

void gsc_level_getclosestplayerinrange()
{
	int args = Scr_GetNumParam();

	if ( args < 1 || Scr_GetType(0) != VAR_VECTOR )
	{
		stackError("gsc_level_getclosestplayerinrange() requires origin (vector)");
		stackPushUndefined();
		return;
	}

	vec3_t origin;
	Scr_GetVector(0, origin);

	float maxDistSq = 0.0f;
	bool hasMaxDist = false;
	if ( args > 1 && Scr_GetType(1) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(1) != VAR_FLOAT && Scr_GetType(1) != VAR_INTEGER )
		{
			stackError("gsc_level_getclosestplayerinrange() maxDistSq must be a number");
			stackPushUndefined();
			return;
		}
		maxDistSq = Scr_GetFloat(1);
		if ( maxDistSq < 0.0f )
		{
			stackError("gsc_level_getclosestplayerinrange() maxDistSq must be >= 0");
			stackPushUndefined();
			return;
		}
		hasMaxDist = true;
	}

	int filterTeam = -1;
	if ( args > 2 && Scr_GetType(2) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(2) != VAR_INTEGER )
		{
			stackError("gsc_level_getclosestplayerinrange() filterTeam must be an int");
			stackPushUndefined();
			return;
		}
		filterTeam = Scr_GetInt(2);
	}

	int contentMask = 0;
	bool hasTraceCheck = false;
	if ( args > 3 && Scr_GetType(3) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(3) != VAR_INTEGER )
		{
			stackError("gsc_level_getclosestplayerinrange() contentMask must be an int");
			stackPushUndefined();
			return;
		}
		contentMask = Scr_GetInt(3);
		hasTraceCheck = true;
	}

	int maxClients = sv_maxclients->current.integer;
	gentity_t *best = NULL;
	float bestDistSq = hasMaxDist ? maxDistSq : 0.0f;
	bool found = false;

	for ( int i = 0; i < maxClients; i++ )
	{
		gentity_t *ent = &g_entities[i];
		if ( !helpers_is_active_player(ent) )
			continue;
		if ( filterTeam >= 0 && ent->client->sess.cs.team != filterTeam )
			continue;

		float dx = ent->r.currentOrigin[0] - origin[0];
		float dy = ent->r.currentOrigin[1] - origin[1];
		float dz = ent->r.currentOrigin[2] - origin[2];
		float d2 = dx * dx + dy * dy + dz * dz;
		if ( hasMaxDist && d2 > maxDistSq )
			continue;

		if ( hasTraceCheck && !G_LocationalTracePassed(origin, ent->r.currentOrigin, ent->s.number, contentMask) )
			continue;

		if ( !found || d2 < bestDistSq )
		{
			best = ent;
			bestDistSq = d2;
			found = true;
		}
	}

	if ( found )
		stackPushEntity(best);
	else
		stackPushUndefined();
}

void gsc_level_getplayersbyvieworigininrange()
{
	int args = Scr_GetNumParam();

	if ( args < 1 || Scr_GetType(0) != VAR_VECTOR )
	{
		stackError("gsc_level_getplayersbyvieworigininrange() requires origin (vector)");
		stackPushUndefined();
		return;
	}

	vec3_t origin;
	Scr_GetVector(0, origin);

	float maxDistSq = 0.0f;
	bool hasMaxDist = false;
	if ( args > 1 && Scr_GetType(1) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(1) != VAR_FLOAT && Scr_GetType(1) != VAR_INTEGER )
		{
			stackError("gsc_level_getplayersbyvieworigininrange() maxDistSq must be a number");
			stackPushUndefined();
			return;
		}
		maxDistSq = Scr_GetFloat(1);
		if ( maxDistSq < 0.0f )
		{
			stackError("gsc_level_getplayersbyvieworigininrange() maxDistSq must be >= 0");
			stackPushUndefined();
			return;
		}
		hasMaxDist = true;
	}

	int filterTeam = -1;
	if ( args > 2 && Scr_GetType(2) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(2) != VAR_INTEGER )
		{
			stackError("gsc_level_getplayersbyvieworigininrange() filterTeam must be an int");
			stackPushUndefined();
			return;
		}
		filterTeam = Scr_GetInt(2);
	}

	int traceContentMask = 0;
	bool hasTraceCheck = false;
	if ( args > 3 && Scr_GetType(3) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(3) != VAR_INTEGER )
		{
			stackError("gsc_level_getplayersbyvieworigininrange() traceContentMask must be an int");
			stackPushUndefined();
			return;
		}
		traceContentMask = Scr_GetInt(3);
		hasTraceCheck = true;
	}

	int maxClients = sv_maxclients->current.integer;

	stackPushArray();
	for ( int i = 0; i < maxClients; i++ )
	{
		gentity_t *ent = &g_entities[i];
		if ( !helpers_is_active_player(ent) )
			continue;
		if ( filterTeam >= 0 && ent->client->sess.cs.team != filterTeam )
			continue;

		vec3_t viewOrigin;
		G_GetPlayerViewOrigin(ent, viewOrigin);

		float dx = viewOrigin[0] - origin[0];
		float dy = viewOrigin[1] - origin[1];
		float dz = viewOrigin[2] - origin[2];
		float d2 = dx * dx + dy * dy + dz * dz;
		if ( hasMaxDist && d2 > maxDistSq )
			continue;

		if ( hasTraceCheck && !G_LocationalTracePassed(origin, viewOrigin, ent->s.number, traceContentMask) )
			continue;

		stackPushEntity(ent);
		stackPushArrayLast();
	}
}

void gsc_level_getplayersinrange()
{
	int args = Scr_GetNumParam();

	if ( args < 1 || Scr_GetType(0) != VAR_VECTOR )
	{
		stackError("gsc_level_getplayersinrange() requires origin (vector)");
		stackPushUndefined();
		return;
	}

	vec3_t origin;
	Scr_GetVector(0, origin);

	float maxDistSq = 0.0f;
	bool hasMaxDist = false;
	if ( args > 1 && Scr_GetType(1) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(1) != VAR_FLOAT && Scr_GetType(1) != VAR_INTEGER )
		{
			stackError("gsc_level_getplayersinrange() maxDistSq must be a number");
			stackPushUndefined();
			return;
		}
		maxDistSq = Scr_GetFloat(1);
		if ( maxDistSq < 0.0f )
		{
			stackError("gsc_level_getplayersinrange() maxDistSq must be >= 0");
			stackPushUndefined();
			return;
		}
		hasMaxDist = true;
	}

	int filterTeam = -1;
	if ( args > 2 && Scr_GetType(2) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(2) != VAR_INTEGER )
		{
			stackError("gsc_level_getplayersinrange() filterTeam must be an int");
			stackPushUndefined();
			return;
		}
		filterTeam = Scr_GetInt(2);
	}

	int contentMask = 0;
	bool hasTraceCheck = false;
	if ( args > 3 && Scr_GetType(3) != VAR_UNDEFINED )
	{
		if ( Scr_GetType(3) != VAR_INTEGER )
		{
			stackError("gsc_level_getplayersinrange() contentMask must be an int");
			stackPushUndefined();
			return;
		}
		contentMask = Scr_GetInt(3);
		hasTraceCheck = true;
	}

	int maxClients = sv_maxclients->current.integer;

	stackPushArray();
	for ( int i = 0; i < maxClients; i++ )
	{
		gentity_t *ent = &g_entities[i];
		if ( !helpers_is_active_player(ent) )
			continue;
		if ( filterTeam >= 0 && ent->client->sess.cs.team != filterTeam )
			continue;

		float dx = ent->r.currentOrigin[0] - origin[0];
		float dy = ent->r.currentOrigin[1] - origin[1];
		float dz = ent->r.currentOrigin[2] - origin[2];
		float d2 = dx * dx + dy * dy + dz * dz;
		if ( hasMaxDist && d2 > maxDistSq )
			continue;

		if ( hasTraceCheck && !G_LocationalTracePassed(origin, ent->r.currentOrigin, ent->s.number, contentMask) )
			continue;

		stackPushEntity(ent);
		stackPushArrayLast();
	}
}

void gsc_level_getpvs()
{
	vec3_t o1, o2;

	if ( !stackGetParams("vv", o1, o2) )
	{
		stackError("gsc_level_getpvs() bad args (expected two origin vectors)");
		stackPushUndefined();
		return;
	}

	int cluster1 = CM_LeafCluster(CM_PointLeafnum(o1));
	int cluster2 = CM_LeafCluster(CM_PointLeafnum(o2));

	if ( cluster1 < 0 || cluster2 < 0 || cluster2 >= cm.numClusters )
	{
		stackPushInt(1);
		return;
	}

	byte *pvs = CM_ClusterPVS(cluster1);
	int visible = ( pvs[cluster2 >> 3] & ( 1 << ( cluster2 & 7 ) ) ) != 0;
	stackPushInt(visible ? 1 : 0);
}

#endif