#include "scepter.h"

/*
 * Room contents: objects, the contents of chests among them, monsters and
 * their inventories, stored by dbstore.c as rows of the game database.  A
 * save replaces all of a room's objects or monsters in one transaction.
 * At most SavedItems are kept in each list; chests inside chests, and the
 * contents of a monster's chests, aren't saved (#44).
 */

/* Read the objects of room *Rm*, and what their chests hold */
void ReadUObj(RmCodeType Rm, ObjectPoint *Object)
{
	if (!GetRoomObjs(Rm, Object))
		Abort(" Cannot read a room's objects.");
}

/* Save the objects of room *Rm*, replacing what was saved before (#42) */
void WriteUObj(RmCodeType Rm, ObjectPoint Object)
{
	if (!PutRoomObjs(Rm, Object))
		Abort(" Cannot save a room's objects.");
}

void DeleteUObj(RmCodeType Rm)
{
	WriteUObj(Rm, NULL);
}

/* Read the monsters of room *Rm*, with their inventories */
void ReadUMon(RmCodeType Rm, MonsterPoint *Monster)
{
	if (!GetRoomMons(Rm, Monster))
		Abort(" Cannot read a room's monsters.");
}

void WriteUMon(RmCodeType Rm, MonsterPoint Monster)
{
	if (!PutRoomMons(Rm, Monster))
		Abort(" Cannot save a room's monsters.");
}

void DeleteMon(RmCodeType Rm)
{
	WriteUMon(Rm, NULL);
}

/* Read MList template *Index*; FALSE, and a zeroed *Mon*, if there is none */
int ReadMList(int Index, MonsterPoint Mon)
{
	int		Found;

	if (!GetMonTemplate(Index, Mon, &Found))
		Abort(" Cannot read a monster template.");
	return Found;
}

/* A new monster from MList template *Index*, or NULL if there is no
 * such template (logged) or no memory */
MonsterPoint NewMList(int Index)
{
	MonsterPoint	Mon;
	char			Msg[64];

	if (!(Mon = malloc(sizeof(MonsterType))))
		return NULL;
	if (!ReadMList(Index, Mon))
	{
		free(Mon);
		snprintf(Msg, sizeof(Msg), " MList has no template %d.", Index);
		ErrLog(Msg);
		return NULL;
	}
	return Mon;
}

/* Read OList template *Index*; FALSE, and a zeroed *Obj*, if there is none */
int ReadOList(int Index, ObjectPoint Obj)
{
	int		Found;

	if (!GetObjTemplate(Index, Obj, &Found))
		Abort(" Cannot read an object template.");
	return Found;
}

/* A new object from OList template *Index*, or NULL if there is no
 * such template (logged) or no memory */
ObjectPoint NewOList(int Index)
{
	ObjectPoint	Obj;
	char		Msg[64];

	if (!(Obj = malloc(sizeof(ObjectType))))
		return NULL;
	if (!ReadOList(Index, Obj))
	{
		free(Obj);
		snprintf(Msg, sizeof(Msg), " OList has no template %d.", Index);
		ErrLog(Msg);
		return NULL;
	}
	return Obj;
}
