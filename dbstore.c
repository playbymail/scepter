/*
 * Game data as rows of the game database (gamedb.c, docs/data-files.md).
 *
 * Each struct the game saves has a pair of functions here: Bind* puts its
 * fields into a statement's parameters, and Col* reads them back from a
 * row.  Both refuse a value the schema can't hold or the struct can't take
 * (an enum out of range, a number too big for its field, a name too long),
 * naming the field, so invalid data never reaches the store or the game.
 * The Get* and Put* functions read and write whole records; each Put* is
 * one transaction, or part of the caller's if one is open.
 *
 * Every function returns FALSE after reporting a problem through DBReport.
 * This file uses no game state, so mkgame links it alone.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "scepter.h"

/* ---------- names of enum values, as stored ---------- */

static const char	*ArticleName[] = { "a", "an", "some", "the", "none", NULL };
static const char	*KindName[] =
{
	"portal", "treasure", "weapon", "armor", "shield", "coins", "scroll",
	"chest", "door", "device", "teleport", "keys", "card", "ring", "misc",
	"dummy", NULL
};
static const char	*SkillName[] = { "sharp", "thrust", "blunt", "pole", NULL };

/* ---------- reporting ---------- */

static void Bad(const char *Where, const char *Field, const char *Why, long Value)
{
	char	Msg[256];

	snprintf(Msg, sizeof(Msg), "%s: %s: %s (%ld)", Where, Field, Why, Value);
	DBReport(Msg);
}

static int Failed(const char *Where, const char *Doing)
{
	char	Msg[512];

	snprintf(Msg, sizeof(Msg), "%s: cannot %s: %s", Where, Doing,
			 sqlite3_errmsg(GameDB));
	DBReport(Msg);
	return FALSE;
}

/* ---------- binding parameters ---------- */

static int BindInt(sqlite3_stmt *St, int At, long Value)
{
	return sqlite3_bind_int64(St, At, Value) == SQLITE_OK;
}

static int BindFlag(sqlite3_stmt *St, int At, int Value, const char *Where,
					const char *Field)
{
	if (Value != 0 && Value != 1)
	{
		Bad(Where, Field, "must be 0 or 1", Value);
		return FALSE;
	}
	return BindInt(St, At, Value);
}

/* Text up to its NUL, or all Size bytes */
static int BindText(sqlite3_stmt *St, int At, const char *Text, size_t Size)
{
	return sqlite3_bind_text(St, At, Text, (int)strnlen(Text, Size),
							 SQLITE_TRANSIENT) == SQLITE_OK;
}

static int BindEnum(sqlite3_stmt *St, int At, int Value, const char **Names,
					const char *Where, const char *Field)
{
	int		N;

	for (N = 0; Names[N]; N++)
		;
	if (Value < 0 || Value >= N)
	{
		Bad(Where, Field, "no such value", Value);
		return FALSE;
	}
	return sqlite3_bind_text(St, At, Names[Value], -1, SQLITE_STATIC) == SQLITE_OK;
}

/* ---------- reading columns ---------- */

/* Column Col as a number from Lo to Hi; NULL or out of range is refused */
static int ColInt(sqlite3_stmt *St, int Col, long Lo, long Hi, long *Value,
				  const char *Where)
{
	sqlite3_int64	V;

	if (sqlite3_column_type(St, Col) != SQLITE_INTEGER)
	{
		Bad(Where, sqlite3_column_name(St, Col), "not a number", 0);
		return FALSE;
	}
	V = sqlite3_column_int64(St, Col);
	if (V < Lo || V > Hi)
	{
		Bad(Where, sqlite3_column_name(St, Col), "out of range", (long)V);
		return FALSE;
	}
	*Value = (long)V;
	return TRUE;
}

/* Column Col as text in a Size-byte field: NUL-terminated if it fits */
static int ColText(sqlite3_stmt *St, int Col, char *Text, size_t Size,
				   int NulNeeded, const char *Where)
{
	const unsigned char	*V = sqlite3_column_text(St, Col);
	size_t				Len = (size_t)sqlite3_column_bytes(St, Col);

	if (!V)
	{
		Bad(Where, sqlite3_column_name(St, Col), "missing", 0);
		return FALSE;
	}
	if (Len > Size - (NulNeeded ? 1 : 0))
	{
		Bad(Where, sqlite3_column_name(St, Col), "too long", (long)Len);
		return FALSE;
	}
	memset(Text, 0, Size);
	memcpy(Text, V, Len);
	return TRUE;
}

static int ColEnum(sqlite3_stmt *St, int Col, const char **Names, int *Value,
				   const char *Where)
{
	const char	*V = (const char *)sqlite3_column_text(St, Col);
	int			N;

	for (N = 0; V && Names[N]; N++)
		if (!strcmp(V, Names[N]))
		{
			*Value = N;
			return TRUE;
		}
	Bad(Where, sqlite3_column_name(St, Col), "no such value", 0);
	return FALSE;
}

/* Read column Col into a field of any integer type, checking its range */
#define	GET(Col, Field, Lo, Hi) \
	(ColInt(St, At + (Col), (Lo), (Hi), &V, Where) ? ((Field) = V, TRUE) : FALSE)
#define	GETFLAG(Col, Field)	GET(Col, Field, 0, 1)

/* ---------- objects ---------- */

/* Object columns, in the order of ObjCols (gamedb.h) */
enum
{
	OName, OArticle, OWeight, OPrice, ODescCode, ODescRec, OCarry, OMagic,
	OPermanent, OInvisible, OCursed, OKind, OToWhere, OMinHp, OMaxHp,
	OStrikes, OWeaPlus, OWeapType, OShPlus, OShHits, OArmPlus, OArmHits,
	OMultiplier, OSpell, OClosed, OTrap, OLocked, ONumInside, ODToWhere,
	ODClosed, ODSpring, ODToll, ODTrap, ODLocked, OUnLock, ONumUses, OMSpell,
	ONumCharges, OMLevel, OTToWhere, OTActiveRm, ORSpell, ORLevel, NObjCols
};

/*
 * Bind the object columns of O to parameters At to At + NObjCols - 1.  Only
 * the fields of O's own kind are stored; the others are NULL.
 */
int BindObj(sqlite3_stmt *St, int At, ObjectPoint O, const char *Where)
{
	union tObject	*U = &O->Object;
	int				Ok = TRUE, C;

	for (C = OToWhere; C < NObjCols; C++)
		Ok &= sqlite3_bind_null(St, At + C) == SQLITE_OK;
	Ok = Ok
	&&	BindText(St, At + OName, O->Name, sizeof(O->Name))
	&&	BindEnum(St, At + OArticle, O->Article, ArticleName, Where, "Article")
	&&	BindInt(St, At + OWeight, O->Weight)
	&&	BindInt(St, At + OPrice, O->Price)
	&&	BindInt(St, At + ODescCode, O->DescCode)
	&&	BindInt(St, At + ODescRec, O->DescRec)
	&&	BindInt(St, At + OCarry, O->Carry)
	&&	BindInt(St, At + OMagic, O->Magic)
	&&	BindInt(St, At + OPermanent, O->Permanent)
	&&	BindInt(St, At + OInvisible, O->Invisible)
	&&	BindInt(St, At + OCursed, O->Cursed)
	&&	BindEnum(St, At + OKind, O->ObClass, KindName, Where, "ObClass");
	if (!Ok)
		return FALSE;

	switch (O->ObClass)
	{
		case Portal:
			return BindInt(St, At + OToWhere, U->Portal.ToWhere);
		case Weap:
			return BindInt(St, At + OMinHp, U->Weap.MinHp)
				&& BindInt(St, At + OMaxHp, U->Weap.MaxHp)
				&& BindInt(St, At + OStrikes, U->Weap.Strikes)
				&& BindInt(St, At + OWeaPlus, U->Weap.WeaPlus)
				&& BindEnum(St, At + OWeapType, U->Weap.WeapType, SkillName,
							Where, "WeapType");
		case Shield:
			return BindInt(St, At + OShPlus, U->Shield.ShPlus)
				&& BindInt(St, At + OShHits, U->Shield.ShHits);
		case Armor:
			return BindInt(St, At + OArmPlus, U->Armor.ArmPlus)
				&& BindInt(St, At + OArmHits, U->Armor.ArmHits);
		case Coins:
			return BindInt(St, At + OMultiplier, U->Coins.Multiplier);
		case Scroll:
			return BindInt(St, At + OSpell, U->Scroll.Spell);
		case Chest:
			return BindFlag(St, At + OClosed, U->Chest.Closed, Where, "Closed")
				&& BindInt(St, At + OTrap, U->Chest.Trap)
				&& BindInt(St, At + OLocked, U->Chest.Locked)
				&& BindInt(St, At + ONumInside, U->Chest.NumInside);
		case Door:
			return BindInt(St, At + ODToWhere, U->Door.DToWhere)
				&& BindInt(St, At + ODClosed, U->Door.DClosed)
				&& BindInt(St, At + ODSpring, U->Door.DSpring)
				&& BindInt(St, At + ODToll, U->Door.DToll)
				&& BindInt(St, At + ODTrap, U->Door.DTrap)
				&& BindInt(St, At + ODLocked, U->Door.DLocked);
		case Keys:
			return BindInt(St, At + OUnLock, U->Keys.UnLock)
				&& BindInt(St, At + ONumUses, U->Keys.NumUses);
		case MagDevice:
			return BindInt(St, At + OMSpell, U->MagDevice.MSpell)
				&& BindInt(St, At + ONumCharges, U->MagDevice.NumCharges)
				&& BindInt(St, At + OMLevel, U->MagDevice.MLevel);
		case Teleport:
			return BindInt(St, At + OTToWhere, U->Teleport.TToWhere)
				&& BindInt(St, At + OTActiveRm, U->Teleport.TActiveRm);
		case Ring:
			return BindInt(St, At + ORSpell, U->Ring.RSpell)
				&& BindInt(St, At + ORLevel, U->Ring.RLevel);
		default:
			return TRUE;
	}
}

/* Read O from the object columns At to At + NObjCols - 1 of the row */
int ColObj(sqlite3_stmt *St, int At, ObjectPoint O, const char *Where)
{
	union tObject	*U = &O->Object;
	long			V;
	int				Article, Kind, Skill;

	memset(O, 0, sizeof(*O));
	if (!ColText(St, At + OName, O->Name, sizeof(O->Name), TRUE, Where)
	||	!ColEnum(St, At + OArticle, ArticleName, &Article, Where)
	||	!ColEnum(St, At + OKind, KindName, &Kind, Where)
	||	!GET(OWeight, O->Weight, SHRT_MIN, SHRT_MAX)
	||	!GET(OPrice, O->Price, SHRT_MIN, SHRT_MAX)
	||	!GET(ODescCode, O->DescCode, 0, UCHAR_MAX)
	||	!GET(ODescRec, O->DescRec, 0, UCHAR_MAX)
	||	!GETFLAG(OCarry, O->Carry)
	||	!GETFLAG(OMagic, O->Magic)
	||	!GETFLAG(OPermanent, O->Permanent)
	||	!GETFLAG(OInvisible, O->Invisible)
	||	!GETFLAG(OCursed, O->Cursed))
		return FALSE;
	O->Article = (ArticleType)Article;
	O->ObClass = (ObClassType)Kind;

	switch (O->ObClass)
	{
		case Portal:
			return GET(OToWhere, U->Portal.ToWhere, SHRT_MIN, SHRT_MAX);
		case Weap:
			if (!ColEnum(St, At + OWeapType, SkillName, &Skill, Where))
				return FALSE;
			U->Weap.WeapType = (SkillType)Skill;
			return GET(OMinHp, U->Weap.MinHp, SHRT_MIN, SHRT_MAX)
				&& GET(OMaxHp, U->Weap.MaxHp, SHRT_MIN, SHRT_MAX)
				&& GET(OStrikes, U->Weap.Strikes, SHRT_MIN, SHRT_MAX)
				&& GET(OWeaPlus, U->Weap.WeaPlus, SHRT_MIN, SHRT_MAX);
		case Shield:
			return GET(OShPlus, U->Shield.ShPlus, SHRT_MIN, SHRT_MAX)
				&& GET(OShHits, U->Shield.ShHits, SHRT_MIN, SHRT_MAX);
		case Armor:
			return GET(OArmPlus, U->Armor.ArmPlus, SHRT_MIN, SHRT_MAX)
				&& GET(OArmHits, U->Armor.ArmHits, SHRT_MIN, SHRT_MAX);
		case Coins:
			return GET(OMultiplier, U->Coins.Multiplier, SHRT_MIN, SHRT_MAX);
		case Scroll:
			return GET(OSpell, U->Scroll.Spell, SHRT_MIN, SHRT_MAX);
		case Chest:
			return GETFLAG(OClosed, U->Chest.Closed)
				&& GET(OTrap, U->Chest.Trap, CHAR_MIN, CHAR_MAX)
				&& GET(OLocked, U->Chest.Locked, SHRT_MIN, SHRT_MAX)
				&& GET(ONumInside, U->Chest.NumInside, CHAR_MIN, CHAR_MAX);
		case Door:
			return GET(ODToWhere, U->Door.DToWhere, SHRT_MIN, SHRT_MAX)
				&& GETFLAG(ODClosed, U->Door.DClosed)
				&& GETFLAG(ODSpring, U->Door.DSpring)
				&& GETFLAG(ODToll, U->Door.DToll)
				&& GET(ODTrap, U->Door.DTrap, SHRT_MIN, SHRT_MAX)
				&& GET(ODLocked, U->Door.DLocked, SHRT_MIN, SHRT_MAX);
		case Keys:
			return GET(OUnLock, U->Keys.UnLock, SHRT_MIN, SHRT_MAX)
				&& GET(ONumUses, U->Keys.NumUses, SHRT_MIN, SHRT_MAX);
		case MagDevice:
			return GET(OMSpell, U->MagDevice.MSpell, SHRT_MIN, SHRT_MAX)
				&& GET(ONumCharges, U->MagDevice.NumCharges, SHRT_MIN, SHRT_MAX)
				&& GET(OMLevel, U->MagDevice.MLevel, CHAR_MIN, CHAR_MAX);
		case Teleport:
			return GET(OTToWhere, U->Teleport.TToWhere, SHRT_MIN, SHRT_MAX)
				&& GET(OTActiveRm, U->Teleport.TActiveRm, SHRT_MIN, SHRT_MAX);
		case Ring:
			return GET(ORSpell, U->Ring.RSpell, SHRT_MIN, SHRT_MAX)
				&& GET(ORLevel, U->Ring.RLevel, CHAR_MIN, CHAR_MAX);
		default:
			return TRUE;
	}
}

/* ---------- monsters ---------- */

/* Monster columns, in the order of MonCols (gamedb.h) */
enum
{
	MName, MArticle, MDescRec, MDescCode, MAtkSpeed, MAC, MHits, MMaxHits,
	MLvl, MNum, MMReact, MMParley, MWhichObj, MDefend, MBlock, MFollow,
	MGuard, MAtkLastAggr, MSlowReact, MFastReact, MInvisible, MRegenerate,
	MDrain, MPoison, MAntiMagic, MUndead, MMoralReact, MFlee, MAssistance,
	MMonSpells, MTop, MPermanent, MMagic, MSummonHelp, MImmoralReact,
	MUnique, MWatch, MNice, NMonCols
};

/* Bind the monster columns of M to parameters At to At + NMonCols - 1 */
int BindMon(sqlite3_stmt *St, int At, MonsterPoint M, const char *Where)
{
	return BindText(St, At + MName, M->Name, sizeof(M->Name))
		&& BindEnum(St, At + MArticle, M->Article, ArticleName, Where, "Article")
		&& BindInt(St, At + MDescRec, M->DescRec)
		&& BindInt(St, At + MDescCode, M->DescCode)
		&& BindInt(St, At + MAtkSpeed, M->AtkSpeed)
		&& BindInt(St, At + MAC, M->AC)
		&& BindInt(St, At + MHits, M->Hits)
		&& BindInt(St, At + MMaxHits, M->MaxHits)
		&& BindInt(St, At + MLvl, M->Lvl)
		&& BindInt(St, At + MNum, M->Num)
		&& BindInt(St, At + MMReact, M->MReact)
		&& BindInt(St, At + MMParley, M->MParley)
		&& BindInt(St, At + MWhichObj, M->WhichObj)
		&& BindInt(St, At + MDefend, M->Defend)
		&& BindInt(St, At + MBlock, M->Block)
		&& BindInt(St, At + MFollow, M->Follow)
		&& BindInt(St, At + MGuard, M->Guard)
		&& BindInt(St, At + MAtkLastAggr, M->AtkLastAggr)
		&& BindInt(St, At + MSlowReact, M->SlowReact)
		&& BindInt(St, At + MFastReact, M->FastReact)
		&& BindInt(St, At + MInvisible, M->Invisible)
		&& BindInt(St, At + MRegenerate, M->Regenerate)
		&& BindInt(St, At + MDrain, M->Drain)
		&& BindInt(St, At + MPoison, M->Poison)
		&& BindInt(St, At + MAntiMagic, M->AntiMagic)
		&& BindInt(St, At + MUndead, M->Undead)
		&& BindInt(St, At + MMoralReact, M->MoralReact)
		&& BindInt(St, At + MFlee, M->Flee)
		&& BindInt(St, At + MAssistance, M->Assistance)
		&& BindInt(St, At + MMonSpells, M->MonSpells)
		&& BindInt(St, At + MTop, M->Top)
		&& BindInt(St, At + MPermanent, M->Permanent)
		&& BindInt(St, At + MMagic, M->Magic)
		&& BindInt(St, At + MSummonHelp, M->SummonHelp)
		&& BindInt(St, At + MImmoralReact, M->ImmoralReact)
		&& BindInt(St, At + MUnique, M->Unique)
		&& BindInt(St, At + MWatch, M->Watch)
		&& BindInt(St, At + MNice, M->Nice);
}

/* Read M from the monster columns At to At + NMonCols - 1 of the row */
int ColMon(sqlite3_stmt *St, int At, MonsterPoint M, const char *Where)
{
	long	V;
	int		Article;

	memset(M, 0, sizeof(*M));
	if (!ColText(St, At + MName, M->Name, sizeof(M->Name), TRUE, Where)
	||	!ColEnum(St, At + MArticle, ArticleName, &Article, Where))
		return FALSE;
	M->Article = (ArticleType)Article;
	return GET(MDescRec, M->DescRec, 0, UCHAR_MAX)
		&& GET(MDescCode, M->DescCode, 0, UCHAR_MAX)
		&& GET(MAtkSpeed, M->AtkSpeed, SCHAR_MIN, SCHAR_MAX)
		&& GET(MAC, M->AC, SCHAR_MIN, SCHAR_MAX)
		&& GET(MHits, M->Hits, SHRT_MIN, SHRT_MAX)
		&& GET(MMaxHits, M->MaxHits, SHRT_MIN, SHRT_MAX)
		&& GET(MLvl, M->Lvl, CHAR_MIN, CHAR_MAX)
		&& GET(MNum, M->Num, CHAR_MIN, CHAR_MAX)
		&& GET(MMReact, M->MReact, CHAR_MIN, CHAR_MAX)
		&& GET(MMParley, M->MParley, CHAR_MIN, CHAR_MAX)
		&& GET(MWhichObj, M->WhichObj, 0, UCHAR_MAX)
		&& GETFLAG(MDefend, M->Defend)
		&& GETFLAG(MBlock, M->Block)
		&& GETFLAG(MFollow, M->Follow)
		&& GETFLAG(MGuard, M->Guard)
		&& GETFLAG(MAtkLastAggr, M->AtkLastAggr)
		&& GETFLAG(MSlowReact, M->SlowReact)
		&& GETFLAG(MFastReact, M->FastReact)
		&& GETFLAG(MInvisible, M->Invisible)
		&& GETFLAG(MRegenerate, M->Regenerate)
		&& GETFLAG(MDrain, M->Drain)
		&& GETFLAG(MPoison, M->Poison)
		&& GETFLAG(MAntiMagic, M->AntiMagic)
		&& GETFLAG(MUndead, M->Undead)
		&& GETFLAG(MMoralReact, M->MoralReact)
		&& GETFLAG(MFlee, M->Flee)
		&& GETFLAG(MAssistance, M->Assistance)
		&& GETFLAG(MMonSpells, M->MonSpells)
		&& GETFLAG(MTop, M->Top)
		&& GETFLAG(MPermanent, M->Permanent)
		&& GETFLAG(MMagic, M->Magic)
		&& GETFLAG(MSummonHelp, M->SummonHelp)
		&& GETFLAG(MImmoralReact, M->ImmoralReact)
		&& GETFLAG(MUnique, M->Unique)
		&& GETFLAG(MWatch, M->Watch)
		&& GETFLAG(MNice, M->Nice);
}

/* Run a statement that returns no rows */
static int Done(sqlite3_stmt *St, const char *Where, const char *Doing)
{
	return sqlite3_step(St) == SQLITE_DONE || Failed(Where, Doing);
}

/* ---------- templates ---------- */

/* Monster template N into M; *Found is FALSE, and M zero, if there is none */
int GetMonTemplate(int N, MonsterPoint M, int *Found)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Rc;

	snprintf(Where, sizeof(Where), "monster template %d", N);
	*Found = FALSE;
	memset(M, 0, sizeof(*M));
	if (!DBPrepare(&St, "SELECT " MonCols " FROM monster_templates WHERE number = ?")
	||	!BindInt(St, 1, N))
		return Failed(Where, "read");
	if ((Rc = sqlite3_step(St)) == SQLITE_ROW)
		*Found = Rc = ColMon(St, 0, M, Where);
	else
	if (!(Rc = Rc == SQLITE_DONE))
		Failed(Where, "read");
	sqlite3_reset(St);			/* ends the read */
	return Rc;
}

int PutMonTemplate(int N, MonsterPoint M)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Own;

	snprintf(Where, sizeof(Where), "monster template %d", N);
	if (!DBStart(&Own))
		return FALSE;
	return DBFinish(Own, DBPrepare(&St, "INSERT OR REPLACE INTO monster_templates"
								 " (number, " MonCols ") VALUES (?, " MonVals ")")
				  && BindInt(St, 1, N) && BindMon(St, 2, M, Where)
				  && Done(St, Where, "save"));
}

/* Object template N into O; *Found is FALSE, and O zero, if there is none */
int GetObjTemplate(int N, ObjectPoint O, int *Found)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Rc;

	snprintf(Where, sizeof(Where), "object template %d", N);
	*Found = FALSE;
	memset(O, 0, sizeof(*O));
	if (!DBPrepare(&St, "SELECT " ObjCols " FROM object_templates WHERE number = ?")
	||	!BindInt(St, 1, N))
		return Failed(Where, "read");
	if ((Rc = sqlite3_step(St)) == SQLITE_ROW)
		*Found = Rc = ColObj(St, 0, O, Where);
	else
	if (!(Rc = Rc == SQLITE_DONE))
		Failed(Where, "read");
	sqlite3_reset(St);			/* ends the read */
	return Rc;
}

int PutObjTemplate(int N, ObjectPoint O)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Own;

	snprintf(Where, sizeof(Where), "object template %d", N);
	if (!DBStart(&Own))
		return FALSE;
	return DBFinish(Own, DBPrepare(&St, "INSERT OR REPLACE INTO object_templates"
								 " (number, " ObjCols ") VALUES (?, " ObjVals ")")
				  && BindInt(St, 1, N) && BindObj(St, 2, O, Where)
				  && Done(St, Where, "save"));
}

/* ---------- descriptions ---------- */

/*
 * Description record Rec into Lines (StrPerSeg lines of 80 bytes, each
 * NUL-terminated unless it is 80 bytes long).  *Found is FALSE, and Lines
 * zero, if there is no such record.
 */
int GetDesc(int Rec, char Lines[StrPerSeg][80], int *Found)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	long				Line;
	int					Rc;

	snprintf(Where, sizeof(Where), "description record %d", Rec);
	*Found = FALSE;
	memset(Lines, 0, StrPerSeg * 80);
	if (!DBPrepare(&St, "SELECT line, text FROM descriptions WHERE record = ?")
	||	!BindInt(St, 1, Rec))
		return Failed(Where, "read");
	while ((Rc = sqlite3_step(St)) == SQLITE_ROW)
	{
		if (!ColInt(St, 0, 1, StrPerSeg, &Line, Where)
		||	!ColText(St, 1, Lines[Line - 1], 80, FALSE, Where))
		{
			sqlite3_reset(St);
			return FALSE;
		}
		*Found = TRUE;
	}
	if (Rc != SQLITE_DONE)
		Failed(Where, "read");
	sqlite3_reset(St);			/* ends the read */
	return Rc == SQLITE_DONE;
}

/* Store all StrPerSeg lines of description record Rec */
int PutDesc(int Rec, char Lines[StrPerSeg][80])
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Own, Line, Ok = TRUE;

	snprintf(Where, sizeof(Where), "description record %d", Rec);
	if (!DBStart(&Own))
		return FALSE;
	for (Line = 0; Ok && Line < StrPerSeg; Line++)
		Ok = DBPrepare(&St, "INSERT OR REPLACE INTO descriptions (record, line, text)"
					   " VALUES (?, ?, ?)")
		  && BindInt(St, 1, Rec) && BindInt(St, 2, Line + 1)
		  && BindText(St, 3, Lines[Line], 80)
		  && Done(St, Where, "save");
	return DBFinish(Own, Ok);
}

/* ---------- rooms ---------- */

/* Room Rm into R, its pointers NULL; *Found is FALSE, and R zero, if none */
int GetRoom(int Rm, RoomType *R, int *Found)
{
	static sqlite3_stmt	*St;
	char				Where[32];
	long				V;
	int					At = 0, Rc;

	snprintf(Where, sizeof(Where), "room %d", Rm);
	*Found = FALSE;
	memset(R, 0, sizeof(*R));
	if (!DBPrepare(&St, "SELECT north, south, east, west, up, down, out, desc_rec,"
				   " desc_code, which_encounter, encounter_time, last_dir,"
				   " notify_dm, safe, anti_magic, dark FROM rooms WHERE room = ?")
	||	!BindInt(St, 1, Rm))
		return Failed(Where, "read");
	if ((Rc = sqlite3_step(St)) == SQLITE_ROW)
		*Found = Rc = GET(0, R->Adjoin[0], SHRT_MIN, SHRT_MAX)
			&& GET(1, R->Adjoin[1], SHRT_MIN, SHRT_MAX)
			&& GET(2, R->Adjoin[2], SHRT_MIN, SHRT_MAX)
			&& GET(3, R->Adjoin[3], SHRT_MIN, SHRT_MAX)
			&& GET(4, R->Adjoin[4], SHRT_MIN, SHRT_MAX)
			&& GET(5, R->Adjoin[5], SHRT_MIN, SHRT_MAX)
			&& GET(6, R->Out, SHRT_MIN, SHRT_MAX)
			&& GET(7, R->DescRec, 0, UCHAR_MAX)
			&& GET(8, R->DescCode, 0, UCHAR_MAX)
			&& GET(9, R->WhichEncounter, 0, UCHAR_MAX)
			&& GET(10, R->EncounterTime, 0, UCHAR_MAX)
			&& GET(11, R->LastDir, 0, UCHAR_MAX)
			&& GETFLAG(12, R->NotifyDM)
			&& GETFLAG(13, R->Safe)
			&& GETFLAG(14, R->AntiMagic)
			&& GETFLAG(15, R->Dark);
	else
	if (!(Rc = Rc == SQLITE_DONE))
		Failed(Where, "read");
	sqlite3_reset(St);			/* ends the read */
	return Rc;
}

/* Store room Rm's own fields; its contents are saved separately */
int PutRoom(int Rm, RoomType *R)
{
	static sqlite3_stmt	*St;
	char				Where[32];
	int					Own, I, Ok;

	snprintf(Where, sizeof(Where), "room %d", Rm);
	if (!DBStart(&Own))
		return FALSE;
	Ok = DBPrepare(&St, "INSERT OR REPLACE INTO rooms (room, north, south, east,"
				   " west, up, down, out, desc_rec, desc_code, which_encounter,"
				   " encounter_time, last_dir, notify_dm, safe, anti_magic, dark)"
				   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
	  && BindInt(St, 1, Rm);
	for (I = 0; Ok && I < 6; I++)
		Ok = BindInt(St, 2 + I, R->Adjoin[I]);
	return DBFinish(Own, Ok
		&& BindInt(St, 8, R->Out)
		&& BindInt(St, 9, R->DescRec)
		&& BindInt(St, 10, R->DescCode)
		&& BindInt(St, 11, R->WhichEncounter)
		&& BindInt(St, 12, R->EncounterTime)
		&& BindInt(St, 13, R->LastDir)
		&& BindInt(St, 14, R->NotifyDM)
		&& BindInt(St, 15, R->Safe)
		&& BindInt(St, 16, R->AntiMagic)
		&& BindInt(St, 17, R->Dark)
		&& Done(St, Where, "save"));
}

/*
 * The number of segments of rooms: the rooms must be 0 to N * 10 - 1, with
 * none missing, or the table is refused.
 */
int CountSegs(int *NSegs)
{
	static sqlite3_stmt	*St;
	sqlite3_int64		Count, Max;
	int					Rc;

	if (!DBPrepare(&St, "SELECT count(*), coalesce(max(room), -1) FROM rooms"))
		return Failed(DBName ": rooms", "count");
	if ((Rc = sqlite3_step(St)) != SQLITE_ROW)
	{
		sqlite3_reset(St);
		return Failed(DBName ": rooms", "count");
	}
	Count = sqlite3_column_int64(St, 0);
	Max = sqlite3_column_int64(St, 1);
	sqlite3_reset(St);
	if (!Count || Count != Max + 1 || Count % 10)
	{
		Bad(DBName ": rooms", "room numbers", "must run from 0 in whole segments"
			" of 10", (long)Count);
		return FALSE;
	}
	*NSegs = (int)(Count / 10);
	return TRUE;
}

/* ---------- room contents ---------- */

/* Free a list read here; chests take their contents with them */
static void FreeObjs(ObjectPoint O)
{
	while (O)
	{
		ObjectPoint	Next = O->Next;

		if (O->ObClass == Chest)
			FreeObjs(O->Object.Chest.ObjectTail);
		free(O);
		O = Next;
	}
}

static void FreeMons(MonsterPoint M)
{
	while (M)
	{
		MonsterPoint	Next = M->Next;

		FreeObjs(M->ObjectTail);
		free(M);
		M = Next;
	}
}

/*
 * Read the objects St returns (its parameters bound), the object columns
 * starting at column At, into *List: at most SavedItems, as they were
 * saved.  Unless Keep, a chest comes back closed to its contents, which
 * aren't saved at that depth (#44).
 */
static int ReadObjs(sqlite3_stmt *St, int At, ObjectPoint *List, int Keep,
					const char *Where)
{
	ObjectPoint	*Tail = List;
	int			N, Rc;

	*List = NULL;
	for (N = 0; N < SavedItems && (Rc = sqlite3_step(St)) == SQLITE_ROW; N++)
	{
		ObjectPoint	O;

		if (!(O = malloc(sizeof(ObjectType))))
		{
			Bad(Where, "objects", "out of memory", 0);
			Rc = SQLITE_NOMEM;
			break;
		}
		if (!ColObj(St, At, O, Where))
		{
			free(O);
			Rc = SQLITE_MISMATCH;
			break;
		}
		if (O->ObClass == Chest && !Keep)
			O->Object.Chest.NumInside = 0;
		*Tail = O;
		Tail = &O->Next;
	}
	if (N == SavedItems)
		Rc = SQLITE_DONE;		/* any more aren't the game's */
	if (Rc != SQLITE_DONE)
	{
		if (Rc != SQLITE_NOMEM && Rc != SQLITE_MISMATCH)
			Failed(Where, "read");
		sqlite3_reset(St);
		FreeObjs(*List);
		*List = NULL;
		return FALSE;
	}
	sqlite3_reset(St);
	return TRUE;
}

/*
 * The objects of room Rm, with the contents of each chest among them, into
 * *List (NULL if there are none).
 */
int GetRoomObjs(int Rm, ObjectPoint *List)
{
	static sqlite3_stmt	*St, *In;
	char				Where[32];
	ObjectPoint			O;
	int					Slot;

	snprintf(Where, sizeof(Where), "room %d's objects", Rm);
	if (!DBPrepare(&St, "SELECT " ObjCols " FROM room_objects WHERE room = ?"
				   " ORDER BY slot")
	||	!BindInt(St, 1, Rm))
		return Failed(Where, "read");
	if (!ReadObjs(St, 0, List, TRUE, Where))
		return FALSE;
	for (O = *List, Slot = 1; O; O = O->Next, Slot++)
		if (O->ObClass == Chest
		&&	!(DBPrepare(&In, "SELECT " ObjCols " FROM chest_objects"
						" WHERE room = ? AND chest = ? ORDER BY slot")
			  && BindInt(In, 1, Rm) && BindInt(In, 2, Slot)
			  && ReadObjs(In, 0, &O->Object.Chest.ObjectTail, FALSE, Where)))
		{
			FreeObjs(*List);
			*List = NULL;
			return FALSE;
		}
	return TRUE;
}

/*
 * Replace room Rm's objects with the first SavedItems of List, and each
 * chest among them with the first SavedItems of its contents.
 */
int PutRoomObjs(int Rm, ObjectPoint List)
{
	static sqlite3_stmt	*Del, *St, *In;
	char				Where[32];
	ObjectPoint			O, C;
	int					Own, Ok, Slot, N;

	snprintf(Where, sizeof(Where), "room %d's objects", Rm);
	if (!DBStart(&Own))
		return FALSE;
	Ok = DBPrepare(&Del, "DELETE FROM room_objects WHERE room = ?")
	  && BindInt(Del, 1, Rm) && Done(Del, Where, "save");
	for (O = List, Slot = 1; Ok && O && Slot <= SavedItems; O = O->Next, Slot++)
	{
		Ok = DBPrepare(&St, "INSERT INTO room_objects (room, slot, " ObjCols ")"
					   " VALUES (?, ?, " ObjVals ")")
		  && BindInt(St, 1, Rm) && BindInt(St, 2, Slot)
		  && BindObj(St, 3, O, Where) && Done(St, Where, "save");
		if (O->ObClass != Chest)
			continue;
		for (C = O->Object.Chest.ObjectTail, N = 1; Ok && C && N <= SavedItems;
			 C = C->Next, N++)
			Ok = DBPrepare(&In, "INSERT INTO chest_objects (room, chest, slot, "
						   ObjCols ") VALUES (?, ?, ?, " ObjVals ")")
			  && BindInt(In, 1, Rm) && BindInt(In, 2, Slot) && BindInt(In, 3, N)
			  && BindObj(In, 4, C, Where) && Done(In, Where, "save");
	}
	return DBFinish(Own, Ok);
}

/*
 * The monsters of room Rm, each with its inventory, into *List (NULL if
 * there are none).  Their pointers are NULL.
 */
int GetRoomMons(int Rm, MonsterPoint *List)
{
	static sqlite3_stmt	*St, *Inv;
	char				Where[32];
	MonsterPoint		*Tail = List, M;
	int					N, Rc = SQLITE_DONE;

	snprintf(Where, sizeof(Where), "room %d's monsters", Rm);
	*List = NULL;
	if (!DBPrepare(&St, "SELECT " MonCols " FROM room_monsters WHERE room = ?"
				   " ORDER BY slot")
	||	!BindInt(St, 1, Rm))
		return Failed(Where, "read");
	for (N = 0; N < SavedItems && (Rc = sqlite3_step(St)) == SQLITE_ROW; N++)
	{
		if (!(M = malloc(sizeof(MonsterType))))
		{
			Bad(Where, "monsters", "out of memory", 0);
			break;
		}
		if (!ColMon(St, 0, M, Where))
		{
			free(M);
			break;
		}
		*Tail = M;
		Tail = &M->Next;
	}
	if (N == SavedItems)
		Rc = SQLITE_DONE;
	sqlite3_reset(St);
	if (Rc != SQLITE_DONE && Rc != SQLITE_ROW)
		Failed(Where, "read");
	if (Rc != SQLITE_DONE)
	{
		FreeMons(*List);
		*List = NULL;
		return FALSE;
	}

	for (M = *List, N = 1; M; M = M->Next, N++)
		if (!DBPrepare(&Inv, "SELECT " ObjCols " FROM monster_objects"
					   " WHERE room = ? AND monster = ? ORDER BY slot")
		||	!BindInt(Inv, 1, Rm) || !BindInt(Inv, 2, N)
		||	!ReadObjs(Inv, 0, &M->ObjectTail, FALSE, Where))
		{
			FreeMons(*List);
			*List = NULL;
			return FALSE;
		}
	return TRUE;
}

/*
 * Replace room Rm's monsters with the first SavedItems of List, each with
 * the first SavedItems of its inventory.
 */
int PutRoomMons(int Rm, MonsterPoint List)
{
	static sqlite3_stmt	*Del, *St, *Inv;
	char				Where[32];
	MonsterPoint		M;
	ObjectPoint			O;
	int					Own, Ok, Slot, N;

	snprintf(Where, sizeof(Where), "room %d's monsters", Rm);
	if (!DBStart(&Own))
		return FALSE;
	Ok = DBPrepare(&Del, "DELETE FROM room_monsters WHERE room = ?")
	  && BindInt(Del, 1, Rm) && Done(Del, Where, "save");
	for (M = List, Slot = 1; Ok && M && Slot <= SavedItems; M = M->Next, Slot++)
	{
		Ok = DBPrepare(&St, "INSERT INTO room_monsters (room, slot, " MonCols ")"
					   " VALUES (?, ?, " MonVals ")")
		  && BindInt(St, 1, Rm) && BindInt(St, 2, Slot)
		  && BindMon(St, 3, M, Where) && Done(St, Where, "save");
		for (O = M->ObjectTail, N = 1; Ok && O && N <= SavedItems; O = O->Next, N++)
			Ok = DBPrepare(&Inv, "INSERT INTO monster_objects (room, monster, slot, "
						   ObjCols ") VALUES (?, ?, ?, " ObjVals ")")
			  && BindInt(Inv, 1, Rm) && BindInt(Inv, 2, Slot) && BindInt(Inv, 3, N)
			  && BindObj(Inv, 4, O, Where) && Done(Inv, Where, "save");
	}
	return DBFinish(Own, Ok);
}

/* ---------- players ---------- */

static const char	*ClassName[] =
{
	"fighter", "thief", "magic-user", "dm", "cleric", "paladin", "ranger",
	"barbarian", NULL
};
static const char	*SexName[] = { "male", "female", NULL };
static const char	*WornName[] =
{
	"weapon", "armor", "shield", "ring-left", "ring-right", NULL
};

/* The saved fields of a player, in this order (players) */
#define	PlayerCols	"name, lvl, class, hits, max_hits, fatigue, max_fatigue, magic," \
	" max_magic, experience, room, str, int, dex, pty, con, last_access, money," \
	" sex, s_sharp, s_thrust, s_blunt, s_long, assoc, spells, ssj, master," \
	" play_tester, a_guild, t_guild, evil, invisible, non_existant, skill_new," \
	" spell_new, poisoned, hidden, it, suck, brief, echo, mes_block, dead," \
	" plague, pw_hash"
#define	PlayerSets	"name = ?2, lvl = ?3, class = ?4, hits = ?5, max_hits = ?6," \
	" fatigue = ?7, max_fatigue = ?8, magic = ?9, max_magic = ?10," \
	" experience = ?11, room = ?12, str = ?13, int = ?14, dex = ?15, pty = ?16," \
	" con = ?17, last_access = ?18, money = ?19, sex = ?20, s_sharp = ?21," \
	" s_thrust = ?22, s_blunt = ?23, s_long = ?24, assoc = ?25, spells = ?26," \
	" ssj = ?27, master = ?28, play_tester = ?29, a_guild = ?30, t_guild = ?31," \
	" evil = ?32, invisible = ?33, non_existant = ?34, skill_new = ?35," \
	" spell_new = ?36, poisoned = ?37, hidden = ?38, it = ?39, suck = ?40," \
	" brief = ?41, echo = ?42, mes_block = ?43, dead = ?44, plague = ?45," \
	" pw_hash = ?46"

/* Bind the saved fields of P, named Name, to parameters At to At + 44 */
static int BindPlayer(sqlite3_stmt *St, int At, UserPoint P, const char *Name,
					  const char *Where)
{
	return BindText(St, At, Name, sizeof(Alfa))
		&& BindInt(St, At + 1, P->Lvl)
		&& BindEnum(St, At + 2, P->Class, ClassName, Where, "Class")
		&& BindInt(St, At + 3, P->Hits)
		&& BindInt(St, At + 4, P->MaxHits)
		&& BindInt(St, At + 5, P->Fatigue)
		&& BindInt(St, At + 6, P->MaxFatigue)
		&& BindInt(St, At + 7, P->Magic)
		&& BindInt(St, At + 8, P->MaxMagic)
		&& BindInt(St, At + 9, P->Experience)
		&& BindInt(St, At + 10, P->RmCode)
		&& BindInt(St, At + 11, P->Str)
		&& BindInt(St, At + 12, P->Int)
		&& BindInt(St, At + 13, P->Dex)
		&& BindInt(St, At + 14, P->Pty)
		&& BindInt(St, At + 15, P->Con)
		&& BindInt(St, At + 16, P->LastAccess)
		&& BindInt(St, At + 17, P->Money)
		&& BindEnum(St, At + 18, P->Sex, SexName, Where, "Sex")
		&& BindInt(St, At + 19, P->SSharp)
		&& BindInt(St, At + 20, P->SThrust)
		&& BindInt(St, At + 21, P->SBlunt)
		&& BindInt(St, At + 22, P->SLong)
		&& BindInt(St, At + 23, P->Assoc)
		&& BindInt(St, At + 24, P->Spells)
		&& BindInt(St, At + 25, P->SSJ)
		&& BindInt(St, At + 26, P->Master)
		&& BindInt(St, At + 27, P->PlayTester)
		&& BindInt(St, At + 28, P->AGuild)
		&& BindInt(St, At + 29, P->TGuild)
		&& BindInt(St, At + 30, P->Evil)
		&& BindInt(St, At + 31, P->Invisible)
		&& BindInt(St, At + 32, P->NonExistant)
		&& BindInt(St, At + 33, P->SkillNew)
		&& BindInt(St, At + 34, P->SpellNew)
		&& BindInt(St, At + 35, P->Poisoned)
		&& BindInt(St, At + 36, P->Hidden)
		&& BindInt(St, At + 37, P->It)
		&& BindInt(St, At + 38, P->Suck)
		&& BindInt(St, At + 39, P->Brief)
		&& BindInt(St, At + 40, P->Echo)
		&& BindInt(St, At + 41, P->MesBlock)
		&& BindInt(St, At + 42, P->Dead)
		&& BindInt(St, At + 43, P->Plague)
		&& BindText(St, At + 44, P->PWHash, sizeof(P->PWHash));
}

/*
 * Read the saved fields of a player from columns At to At + 44 into P,
 * leaving its other fields alone.  The old hash PW isn't stored (#151).
 */
static int ColPlayer(sqlite3_stmt *St, int At, UserPoint P, const char *Where)
{
	long	V;
	int		Class, Sex;

	if (!ColText(St, At, P->Name, sizeof(P->Name), TRUE, Where)
	||	!ColEnum(St, At + 2, ClassName, &Class, Where)
	||	!ColEnum(St, At + 18, SexName, &Sex, Where)
	||	!ColText(St, At + 44, P->PWHash, sizeof(P->PWHash), TRUE, Where))
		return FALSE;
	P->Class = (ChType)Class;
	P->Sex = (SexType)Sex;
	P->PW = 0;
	return GET(1, P->Lvl, SHRT_MIN, SHRT_MAX)
		&& GET(3, P->Hits, SHRT_MIN, SHRT_MAX)
		&& GET(4, P->MaxHits, SHRT_MIN, SHRT_MAX)
		&& GET(5, P->Fatigue, SHRT_MIN, SHRT_MAX)
		&& GET(6, P->MaxFatigue, SHRT_MIN, SHRT_MAX)
		&& GET(7, P->Magic, SHRT_MIN, SHRT_MAX)
		&& GET(8, P->MaxMagic, SHRT_MIN, SHRT_MAX)
		&& GET(9, P->Experience, LONG_MIN, LONG_MAX)
		&& GET(10, P->RmCode, SHRT_MIN, SHRT_MAX)
		&& GET(11, P->Str, SHRT_MIN, SHRT_MAX)
		&& GET(12, P->Int, SHRT_MIN, SHRT_MAX)
		&& GET(13, P->Dex, SHRT_MIN, SHRT_MAX)
		&& GET(14, P->Pty, SHRT_MIN, SHRT_MAX)
		&& GET(15, P->Con, SHRT_MIN, SHRT_MAX)
		&& GET(16, P->LastAccess, SHRT_MIN, SHRT_MAX)
		&& GET(17, P->Money, LONG_MIN, LONG_MAX)
		&& GET(19, P->SSharp, SHRT_MIN, SHRT_MAX)
		&& GET(20, P->SThrust, SHRT_MIN, SHRT_MAX)
		&& GET(21, P->SBlunt, SHRT_MIN, SHRT_MAX)
		&& GET(22, P->SLong, SHRT_MIN, SHRT_MAX)
		&& GET(23, P->Assoc, SHRT_MIN, SHRT_MAX)
		&& GET(24, P->Spells, LONG_MIN, LONG_MAX)
		&& GETFLAG(25, P->SSJ)
		&& GETFLAG(26, P->Master)
		&& GETFLAG(27, P->PlayTester)
		&& GETFLAG(28, P->AGuild)
		&& GETFLAG(29, P->TGuild)
		&& GETFLAG(30, P->Evil)
		&& GETFLAG(31, P->Invisible)
		&& GETFLAG(32, P->NonExistant)
		&& GETFLAG(33, P->SkillNew)
		&& GETFLAG(34, P->SpellNew)
		&& GETFLAG(35, P->Poisoned)
		&& GETFLAG(36, P->Hidden)
		&& GETFLAG(37, P->It)
		&& GETFLAG(38, P->Suck)
		&& GETFLAG(39, P->Brief)
		&& GETFLAG(40, P->Echo)
		&& GETFLAG(41, P->MesBlock)
		&& GETFLAG(42, P->Dead)
		&& GETFLAG(43, P->Plague);
}

/* The id of the saved player Name (in capitals), or 0 if there is none */
static int PlayerId(const char *Name, sqlite3_int64 *Id)
{
	static sqlite3_stmt	*St;
	int					Rc;

	*Id = 0;
	if (!DBPrepare(&St, "SELECT id FROM players WHERE name = ?")
	||	!BindText(St, 1, Name, sizeof(Alfa)))
		return Failed(Name, "look up the player");
	if ((Rc = sqlite3_step(St)) == SQLITE_ROW)
		*Id = sqlite3_column_int64(St, 0);
	sqlite3_reset(St);
	return Rc == SQLITE_ROW || Rc == SQLITE_DONE || Failed(Name, "look up the player");
}

/* Is there a saved player Name (in capitals)?  -1 on an error */
int HavePlayer(const char *Name)
{
	sqlite3_int64	Id;

	return PlayerId(Name, &Id) ? Id != 0 : -1;
}

/*
 * Read saved player Name (in capitals) into P: its saved fields, and its
 * objects, in the order they were saved, as P->ObjectTail, with the
 * USWeap, USArm, USShield, USRingL and USRingR it wears.  *Found is FALSE,
 * and P untouched, if there is no such player.
 */
int GetPlayer(const char *Name, UserPoint P, int *Found)
{
	static sqlite3_stmt	*St, *Objs;
	char				Where[48];
	ObjectPoint			O, *Tail;
	int					Rc, N;

	snprintf(Where, sizeof(Where), "player %s", Name);
	*Found = FALSE;
	if (!DBPrepare(&St, "SELECT id, " PlayerCols " FROM players WHERE name = ?")
	||	!BindText(St, 1, Name, sizeof(Alfa)))
		return Failed(Where, "read");
	if ((Rc = sqlite3_step(St)) == SQLITE_DONE)
	{
		sqlite3_reset(St);
		return TRUE;
	}
	if (Rc != SQLITE_ROW)
	{
		sqlite3_reset(St);
		return Failed(Where, "read");
	}
	if (!ColPlayer(St, 1, P, Where)
	||	!DBPrepare(&Objs, "SELECT worn, " ObjCols " FROM player_objects"
				   " WHERE player = ? ORDER BY slot")
	||	!BindInt(Objs, 1, (long)sqlite3_column_int64(St, 0)))
	{
		sqlite3_reset(St);
		return FALSE;
	}
	sqlite3_reset(St);

	P->ObjectTail = NULL;
	P->USWeap = P->USArm = P->USShield = P->USRingL = P->USRingR = NULL;
	Tail = &P->ObjectTail;
	for (N = 0; N < MaxObjs && (Rc = sqlite3_step(Objs)) == SQLITE_ROW; N++)
	{
		int		Worn = -1;

		if (!(O = malloc(sizeof(ObjectType))))
		{
			Bad(Where, "objects", "out of memory", 0);
			break;
		}
		if (!ColObj(Objs, 1, O, Where)
		||	sqlite3_column_type(Objs, 0) != SQLITE_NULL
		&&	!ColEnum(Objs, 0, WornName, &Worn, Where))
		{
			free(O);
			break;
		}
		if (O->ObClass == Chest)		/* a carried chest keeps nothing */
			O->Object.Chest.NumInside = 0;
		switch (Worn)
		{
			case 0:	P->USWeap = O;		break;
			case 1:	P->USArm = O;		break;
			case 2:	P->USShield = O;	break;
			case 3:	P->USRingL = O;		break;
			case 4:	P->USRingR = O;		break;
		}
		*Tail = O;
		Tail = &O->Next;
	}
	if (N == MaxObjs)
		Rc = SQLITE_DONE;
	sqlite3_reset(Objs);
	if (Rc != SQLITE_DONE)
	{
		if (Rc != SQLITE_ROW)
			Failed(Where, "read");
		FreeObjs(P->ObjectTail);
		P->ObjectTail = NULL;
		P->USWeap = P->USArm = P->USShield = P->USRingL = P->USRingR = NULL;
		return FALSE;
	}
	*Found = TRUE;
	return TRUE;
}

/*
 * Save player P as Name (in capitals), with its first MaxObjs objects
 * and what it wears among them, in one transaction.  A player saved for
 * the first time gets the next id, which no one else will ever have.
 */
int PutPlayer(const char *Name, UserPoint P)
{
	static sqlite3_stmt	*Ins, *Upd, *Del, *Objs;
	char				Where[48];
	sqlite3_int64		Id;
	ObjectPoint			O;
	int					Own, Ok, N;

	snprintf(Where, sizeof(Where), "player %s", Name);
	if (!DBStart(&Own))
		return FALSE;
	Ok = PlayerId(Name, &Id);
	if (Ok && Id)
		Ok = DBPrepare(&Upd, "UPDATE players SET " PlayerSets " WHERE id = ?1")
		  && BindInt(Upd, 1, (long)Id) && BindPlayer(Upd, 2, P, Name, Where)
		  && Done(Upd, Where, "save")
		  && DBPrepare(&Del, "DELETE FROM player_objects WHERE player = ?")
		  && BindInt(Del, 1, (long)Id) && Done(Del, Where, "save");
	else
	if (Ok)
	{
		Ok = DBPrepare(&Ins, "INSERT INTO players (" PlayerCols ") VALUES (?, ?,"
					   " ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
					   " ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
					   " ?, ?, ?, ?, ?)")
		  && BindPlayer(Ins, 1, P, Name, Where) && Done(Ins, Where, "save");
		Id = sqlite3_last_insert_rowid(GameDB);
	}

	for (O = P->ObjectTail, N = 1; Ok && O && N <= MaxObjs; O = O->Next, N++)
	{
		int		Worn = O == P->USWeap ? 0 : O == P->USArm ? 1
					 : O == P->USShield ? 2 : O == P->USRingL ? 3
					 : O == P->USRingR ? 4 : -1;

		Ok = DBPrepare(&Objs, "INSERT INTO player_objects (player, slot, worn, "
					   ObjCols ") VALUES (?, ?, ?, " ObjVals ")")
		  && BindInt(Objs, 1, (long)Id) && BindInt(Objs, 2, N)
		  && (Worn < 0 || BindEnum(Objs, 3, Worn, WornName, Where, "worn"))
		  && BindObj(Objs, 4, O, Where) && Done(Objs, Where, "save");
	}
	return DBFinish(Own, Ok);
}

/* Delete saved player Name (in capitals) and its objects; *Found says if */
int DropPlayer(const char *Name, int *Found)
{
	static sqlite3_stmt	*St;
	int					Own;

	if (!DBStart(&Own))
		return FALSE;
	if (!DBPrepare(&St, "DELETE FROM players WHERE name = ?")
	||	!BindText(St, 1, Name, sizeof(Alfa))
	||	!Done(St, Name, "delete the player"))
		return DBFinish(Own, FALSE);
	*Found = sqlite3_changes(GameDB) > 0;
	return DBFinish(Own, TRUE);
}

/*
 * Rename saved player Old to New (both in capitals), keeping its id and so
 * its seniority.  *Result is 1 when renamed, 0 if there is no player Old,
 * and -1 if New is already taken, which is refused.
 */
int RenamePlayer(const char *Old, const char *New, int *Result)
{
	static sqlite3_stmt	*St;
	sqlite3_int64		Id, Taken;
	int					Own, Ok;

	if (!DBStart(&Own))
		return FALSE;
	*Result = 0;
	Ok = PlayerId(Old, &Id) && PlayerId(New, &Taken);
	if (Ok && Id && Taken && Taken != Id)
		*Result = -1;
	else
	if (Ok && Id)
	{
		Ok = DBPrepare(&St, "UPDATE players SET name = ? WHERE id = ?")
		  && BindText(St, 1, New, sizeof(Alfa)) && BindInt(St, 2, (long)Id)
		  && Done(St, Old, "rename the player");
		*Result = Ok;
	}
	return DBFinish(Own, Ok);
}

/*
 * The saved player after the one with id *After (0: the most senior) into
 * P's saved fields and *After; *Found is FALSE after the last.
 */
int NextPlayer(long *After, UserPoint P, int *Found)
{
	static sqlite3_stmt	*St;
	int					Rc;

	*Found = FALSE;
	if (!DBPrepare(&St, "SELECT id, " PlayerCols " FROM players WHERE id > ?"
				   " ORDER BY id LIMIT 1")
	||	!BindInt(St, 1, *After))
		return Failed("players", "list");
	if ((Rc = sqlite3_step(St)) == SQLITE_ROW)
	{
		*After = (long)sqlite3_column_int64(St, 0);
		Rc = ColPlayer(St, 1, P, "players") ? SQLITE_DONE : SQLITE_ERROR;
		*Found = Rc == SQLITE_DONE;
	}
	else
	if (Rc != SQLITE_DONE)
		Failed("players", "list");
	sqlite3_reset(St);
	return Rc == SQLITE_DONE;
}
