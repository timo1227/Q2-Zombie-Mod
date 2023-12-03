/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "g_local.h"

game_locals_t	game;
level_locals_t	level;
game_import_t	gi;
game_export_t	globals;
spawn_temp_t	st;

int	sm_meat_index;
int	snd_fry;
int meansOfDeath;

edict_t		*g_edicts;

cvar_t	*deathmatch;
cvar_t	*coop;
cvar_t	*dmflags;
cvar_t	*skill;
cvar_t	*fraglimit;
cvar_t	*timelimit;
cvar_t	*password;
cvar_t	*spectator_password;
cvar_t	*maxclients;
cvar_t	*maxspectators;
cvar_t	*maxentities;
cvar_t	*g_select_empty;
cvar_t	*dedicated;

cvar_t	*filterban;

cvar_t	*sv_maxvelocity;
cvar_t	*sv_gravity;

cvar_t	*sv_rollspeed;
cvar_t	*sv_rollangle;
cvar_t	*gun_x;
cvar_t	*gun_y;
cvar_t	*gun_z;

cvar_t	*run_pitch;
cvar_t	*run_roll;
cvar_t	*bob_up;
cvar_t	*bob_pitch;
cvar_t	*bob_roll;

cvar_t	*sv_cheats;

cvar_t	*flood_msgs;
cvar_t	*flood_persecond;
cvar_t	*flood_waitdelay;

cvar_t	*sv_maplist;

void SpawnEntities (char *mapname, char *entities, char *spawnpoint);
void ClientThink (edict_t *ent, usercmd_t *cmd);
qboolean ClientConnect (edict_t *ent, char *userinfo);
void ClientUserinfoChanged (edict_t *ent, char *userinfo);
void ClientDisconnect (edict_t *ent);
void ClientBegin (edict_t *ent);
void ClientCommand (edict_t *ent);
void RunEntity (edict_t *ent);
void WriteGame (char *filename, qboolean autosave);
void ReadGame (char *filename);
void WriteLevel (char *filename);
void ReadLevel (char *filename);
void InitGame (void);
void G_RunFrame (void);

static int currentWave = 0;
static int zombiesToSpawn = 0;
int zombiesAlive = 0; 
int roundZombies = 0; // Total number of zombies to be spawned in the current round
static qboolean waveActive = false;

static float nextWaveTime = 0;
static float waveSystemInitTime = 0; // Time at which the wave system will be initialized

// Initialize wave system at the start of the game or level
static qboolean isWaveSystemInitialized = false;

//===================================================================


void ShutdownGame (void)
{
	gi.dprintf ("==== ShutdownGame ====\n");

	gi.FreeTags (TAG_LEVEL);
	gi.FreeTags (TAG_GAME);
}


/*
=================
GetGameAPI

Returns a pointer to the structure with all entry points
and global variables
=================
*/
game_export_t *GetGameAPI (game_import_t *import)
{
	gi = *import;

	globals.apiversion = GAME_API_VERSION;
	globals.Init = InitGame;
	globals.Shutdown = ShutdownGame;
	globals.SpawnEntities = SpawnEntities;

	globals.WriteGame = WriteGame;
	globals.ReadGame = ReadGame;
	globals.WriteLevel = WriteLevel;
	globals.ReadLevel = ReadLevel;

	globals.ClientThink = ClientThink;
	globals.ClientConnect = ClientConnect;
	globals.ClientUserinfoChanged = ClientUserinfoChanged;
	globals.ClientDisconnect = ClientDisconnect;
	globals.ClientBegin = ClientBegin;
	globals.ClientCommand = ClientCommand;

	globals.RunFrame = G_RunFrame;

	globals.ServerCommand = ServerCommand;

	globals.edict_size = sizeof(edict_t);

	return &globals;
}

#ifndef GAME_HARD_LINKED
// this is only here so the functions in q_shared.c and q_shwin.c can link
void Sys_Error (char *error, ...)
{
	va_list		argptr;
	char		text[1024];

	va_start (argptr, error);
	vsprintf (text, error, argptr);
	va_end (argptr);

	gi.error (ERR_FATAL, "%s", text);
}

void Com_Printf (char *msg, ...)
{
	va_list		argptr;
	char		text[1024];

	va_start (argptr, msg);
	vsprintf (text, msg, argptr);
	va_end (argptr);

	gi.dprintf ("%s", text);
}

#endif

//======================================================================


/*
=================
ClientEndServerFrames
=================
*/
void ClientEndServerFrames (void)
{
	int		i;
	edict_t	*ent;

	// calc the player views now that all pushing
	// and damage has been added
	for (i=0 ; i<maxclients->value ; i++)
	{
		ent = g_edicts + 1 + i;
		if (!ent->inuse || !ent->client)
			continue;
		ClientEndServerFrame (ent);
	}

}

/*
=================
CreateTargetChangeLevel

Returns the created target changelevel
=================
*/
edict_t *CreateTargetChangeLevel(char *map)
{
	edict_t *ent;

	ent = G_Spawn ();
	ent->classname = "target_changelevel";
	Com_sprintf(level.nextmap, sizeof(level.nextmap), "%s", map);
	ent->map = level.nextmap;
	return ent;
}

/*
=================
EndDMLevel

The timelimit or fraglimit has been exceeded
=================
*/
void EndDMLevel (void)
{
	edict_t		*ent;
	char *s, *t, *f;
	static const char *seps = " ,\n\r";

	// stay on same level flag
	if ((int)dmflags->value & DF_SAME_LEVEL)
	{
		BeginIntermission (CreateTargetChangeLevel (level.mapname) );
		return;
	}

	// see if it's in the map list
	if (*sv_maplist->string) {
		s = strdup(sv_maplist->string);
		f = NULL;
		t = strtok(s, seps);
		while (t != NULL) {
			if (Q_stricmp(t, level.mapname) == 0) {
				// it's in the list, go to the next one
				t = strtok(NULL, seps);
				if (t == NULL) { // end of list, go to first one
					if (f == NULL) // there isn't a first one, same level
						BeginIntermission (CreateTargetChangeLevel (level.mapname) );
					else
						BeginIntermission (CreateTargetChangeLevel (f) );
				} else
					BeginIntermission (CreateTargetChangeLevel (t) );
				free(s);
				return;
			}
			if (!f)
				f = t;
			t = strtok(NULL, seps);
		}
		free(s);
	}

	if (level.nextmap[0]) // go to a specific map
		BeginIntermission (CreateTargetChangeLevel (level.nextmap) );
	else {	// search for a changelevel
		ent = G_Find (NULL, FOFS(classname), "target_changelevel");
		if (!ent)
		{	// the map designer didn't include a changelevel,
			// so create a fake ent that goes back to the same level
			BeginIntermission (CreateTargetChangeLevel (level.mapname) );
			return;
		}
		BeginIntermission (ent);
	}
}

/*
=================
CheckDMRules
=================
*/
void CheckDMRules (void)
{
	int			i;
	gclient_t	*cl;

	if (level.intermissiontime)
		return;

	if (!deathmatch->value)
		return;

	if (timelimit->value)
	{
		if (level.time >= timelimit->value*60)
		{
			gi.bprintf (PRINT_HIGH, "Timelimit hit.\n");
			EndDMLevel ();
			return;
		}
	}

	if (fraglimit->value)
	{
		for (i=0 ; i<maxclients->value ; i++)
		{
			cl = game.clients + i;
			if (!g_edicts[i+1].inuse)
				continue;

			if (cl->resp.score >= fraglimit->value)
			{
				gi.bprintf (PRINT_HIGH, "Fraglimit hit.\n");
				EndDMLevel ();
				return;
			}
		}
	}
}


/*
=============
ExitLevel
=============
*/
void ExitLevel (void)
{
	int		i;
	edict_t	*ent;
	char	command [256];

	Com_sprintf (command, sizeof(command), "gamemap \"%s\"\n", level.changemap);
	gi.AddCommandString (command);
	level.changemap = NULL;
	level.exitintermission = 0;
	level.intermissiontime = 0;
	ClientEndServerFrames ();

	// clear some things before going to next level
	for (i=0 ; i<maxclients->value ; i++)
	{
		ent = g_edicts + 1 + i;
		if (!ent->inuse)
			continue;
		if (ent->health > ent->client->pers.max_health)
			ent->health = ent->client->pers.max_health;
	}

}

/*
=============
IsValidSpawnLocation
=============
*/
qboolean IsValidSpawnLocation(vec3_t pos) {
	trace_t tr;
	vec3_t mins, maxs;
	vec3_t end;

	// Set the bounding box dimensions for a typical zombie
	VectorSet(mins, -16, -16, -24);
	VectorSet(maxs, 16, 16, 32);

	// Set the end point for the trace to be the same as the start point
	VectorCopy(pos, end);

	// Adjust end point vertically to check from slightly above the position
	end[2] += 10;

	// Perform the trace
	tr = gi.trace(pos, mins, maxs, end, NULL, MASK_SOLID);

	// Check if the trace hit anything
	if (tr.fraction < 1.0)
		return false; // The location is obstructed

	// Additional checks can go here, like checking for water or other specific conditions

	return true; // The location is clear
}

/*
=============
FindAllPlayers
=============
*/
int FindAllPlayers(edict_t** playerList, int maxPlayers) {
	int numPlayers = 0;
	for (int i = 0; i < maxclients->value && numPlayers < maxPlayers; i++) {
		edict_t* ent = g_edicts + 1 + i;
		if (ent->inuse && ent->client) {
			playerList[numPlayers++] = ent;
		}
	}
	return numPlayers;
}

/*
=============
CalculateZombiesToSpawn
=============
*/
int CalculateZombiesToSpawn(int wave) {
	return 10 + (wave - 1) * 5; // Example formula, adjust as needed
}

/*
=============
CalculateSpawnPosition
=============
*/
void CalculateSpawnPosition(edict_t* player, float radius, vec3_t spawnPos) {
	// Define a minimum distance from the player to avoid spawning zombies too close
	float minDistance = 100.0; // Adjust this value as needed

	do {
		// Random angle and distance within the radius
		float angle = random() * 2 * M_PI;
		float distance = minDistance + random() * (radius - minDistance);

		spawnPos[0] = player->s.origin[0] + cos(angle) * distance;
		spawnPos[1] = player->s.origin[1] + sin(angle) * distance;
		spawnPos[2] = player->s.origin[2]; // Same vertical position as the player

		// Check if spawnPos is a valid location (not inside walls, etc.)
	} while (!IsValidSpawnLocation(spawnPos));
}

/*
=============
SpawnWaveZombies
=============
*/
void SpawnWaveZombies() {
	edict_t* players[MAX_CLIENTS];
	int numPlayers = FindAllPlayers(players, MAX_CLIENTS);

	if (numPlayers <= 0) return; // No players found

	vec3_t spawnPos;
	for (int i = 0; i < zombiesToSpawn; i++) {
		if (zombiesAlive >= MAX_ZOMBIES) break; // Limit the number of zombies

		edict_t* targetPlayer = players[i % numPlayers]; // Distribute zombies among players
		CalculateSpawnPosition(targetPlayer, 500.0, spawnPos); // Calculate spawn position

		edict_t* zombie = G_Spawn(); // Function to spawn an entity
		VectorCopy(spawnPos, zombie->s.origin); // Set spawn position

		SP_monster_soldier(zombie, currentWave);
		zombiesAlive++;  // Increment zombiesAlive
	}
}

/*
=============
ManageWaveProgression
=============
*/
void ManageWaveProgression() {
	if (waveActive && level.time > nextWaveTime) {
		if (zombiesAlive <= 0 && roundZombies <= 0) {
			// Advance to the next wave
			currentWave++;
			zombiesToSpawn = CalculateZombiesToSpawn(currentWave);
			roundZombies = zombiesToSpawn;
			SpawnWaveZombies();

			// Send message to all connected clients about the new wave
			for (int i = 0; i < maxclients->value; i++) {
				edict_t* client = g_edicts + 1 + i;
				if (!client->inuse)
					continue;

				gi.cprintf(client, PRINT_HIGH, "Round: %d\n", zombiesAlive);
			}

			// Set the cooldown for the next wave
			nextWaveTime = level.time + WAVE_COOLDOWN_TIME;
		}
		else if (roundZombies > 0 && zombiesAlive < MAX_ZOMBIES) {
			// There are still zombies to spawn in the current round
			SpawnWaveZombies();
		}
	}
}

/*
=============
InitializeWaveSystem
=============
*/
void InitializeWaveSystem() {
	currentWave = 1;
	gi.dprintf("currentWave: %d\n", currentWave);

	zombiesToSpawn = CalculateZombiesToSpawn(currentWave);
	roundZombies = zombiesToSpawn;
	gi.dprintf("zombiesToSpawn: %d\n", zombiesToSpawn);

	zombiesAlive = 0;
	waveActive = true;

	SpawnWaveZombies();
}

/*
================
G_RunFrame

Advances the world by 0.1 seconds
================
*/
void G_RunFrame (void)
{
	int		i;
	edict_t	*ent;

	// Set the time to initialize the wave system if not already set
	if (waveSystemInitTime == 0) {
		waveSystemInitTime = level.time + WAVE_INIT_DELAY;
	}

	// Initialize the wave system after the delay
	if (!isWaveSystemInitialized && level.time > waveSystemInitTime) {
		InitializeWaveSystem();
		gi.dprintf("Wave System Initialized\n");
		isWaveSystemInitialized = true;
	}

	level.framenum++;
	level.time = level.framenum*FRAMETIME;

	// choose a client for monsters to target this frame
	AI_SetSightClient ();

	// exit intermissions

	if (level.exitintermission)
	{
		ExitLevel ();
		return;
	}

	// Wave management
	ManageWaveProgression();

	//
	// treat each object in turn
	// even the world gets a chance to think
	//
	ent = &g_edicts[0];
	for (i=0 ; i<globals.num_edicts ; i++, ent++)
	{
		if (!ent->inuse)
			continue;

		level.current_entity = ent;

		VectorCopy (ent->s.origin, ent->s.old_origin);

		// if the ground entity moved, make sure we are still on it
		if ((ent->groundentity) && (ent->groundentity->linkcount != ent->groundentity_linkcount))
		{
			ent->groundentity = NULL;
			if ( !(ent->flags & (FL_SWIM|FL_FLY)) && (ent->svflags & SVF_MONSTER) )
			{
				M_CheckGround (ent);
			}
		}

		if (i > 0 && i <= maxclients->value)
		{
			ClientBeginServerFrame (ent);
			continue;
		}

		G_RunEntity (ent);
	}

	// see if it is time to end a deathmatch
	CheckDMRules ();

	// build the playerstate_t structures for all players
	ClientEndServerFrames ();
}

