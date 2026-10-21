#include "scepter.h"

static int	ReadObj(ObjRec *Rec, int Room, int Type);
static void	WriteObj(ObjRec *Rec, int Room, int Type);
static int	FindRec(FILE *fp, size_t RecSize, int Room, int Type, int *Free);
static void	PutRec(FILE *fp, size_t RecSize, void *Rec, int Room, int Type);
static void	DeleteRec(FILE *fp, size_t RecSize, int Room, int Type);
static int	ReadMon(MonRec *Rec, int Room, int Type);
static void	DeleteObjType(RmCodeType Rm, int Type);
static void	ReadMonInv(RmCodeType Rm, MonsterPoint Monster);
static void	WriteMonInv(RmCodeType Rm, MonsterPoint Monster);

/* dbobj record types: 0 is a room's objects, 1..8 the contents of its
 * chests, and MonInvType + N the inventory of its Nth monster (from 0). */
#define	MonInvType	9

/*
 * Read the objects of room *Rm* (Type 0) or the contents of its Nth chest
 * (Type N).  The Nth chest in the room's list, empty or not, keeps its
 * contents in record N, as WriteUObj() and mkgame number them (#43, #56).
 * Chests inside chests have no record (#44).
 */
void ReadUObj(RmCodeType Rm, ObjectPoint *Object, int Type)
{
	ObjRec		Rec;
	ObjectPoint	Pt, *Tail = Object;
	int			N, ChestNo = 0;

	*Object = NULL;
	if (!ReadObj(&Rec, Rm, Type))
		return;

	for (N = 0; N < Rec.Num && N < SavedItems; N++)
	{
		if (!(Pt = malloc(sizeof(ObjectType))))
			Abort(" Out of memory loading room objects.");
		*Pt = Rec.Obj[N];
		Pt->Next = NULL;
		if (Pt->ObClass == Chest)
		{
			Pt->Object.Chest.ObjectTail = NULL;	/* stale pointer from disk */
			if (Type == 0)
				ReadUObj(Rm, &Pt->Object.Chest.ObjectTail, ++ChestNo);
			else
				Pt->Object.Chest.NumInside = 0;
		}
		*Tail = Pt;
		Tail = &Pt->Next;
	}
}

/*
 * Save the objects of room *Rm* (Type 0) or the contents of its Nth chest
 * (Type N), numbering chests as ReadUObj() does.  The record of a chest
 * that is now empty, or no longer in the room, is deleted, so its old
 * contents can't come back (#42).
 */
void WriteUObj(RmCodeType Rm, ObjectPoint Object, int Type)
{
	ObjectPoint	Pt;
	ObjRec		Rec;
	int			N, ChestNo = 0;

	memset(&Rec, 0, sizeof(Rec));
	for (Pt = Object, N = 0; Pt && N < SavedItems; Pt = Pt->Next, N++)
	{
		Rec.Obj[N] = *Pt;
		if (Pt->ObClass == Chest && Type == 0)
		{
			if (Pt->Object.Chest.ObjectTail)
				WriteUObj(Rm, Pt->Object.Chest.ObjectTail, ++ChestNo);
			else
				DeleteObjType(Rm, ++ChestNo);
		}
	}
	if (Type == 0)
		while (ChestNo < SavedItems)
			DeleteObjType(Rm, ++ChestNo);

	Rec.Room = Rm;
	Rec.DataType = Type;
	Rec.Num = N;
	WriteObj(&Rec, Rm, Type);
}

void DeleteUObj(RmCodeType Rm)
{
	int		Type;

	for (Type = 0; Type <= 8; Type++)
		DeleteObjType(Rm, Type);
}

static void DeleteObjType(RmCodeType Rm, int Type)
{
	DeleteRec(EDBObj, sizeof(ObjRec), Rm, Type);
}

/* Load the inventory of each monster in the list read for room *Rm*. */
static void ReadMonInv(RmCodeType Rm, MonsterPoint Monster)
{
	int		N;

	for (N = 0; Monster && N < SavedItems; Monster = Monster->Next, N++)
	{
		ObjRec		Rec;
		ObjectPoint	Pt, *Tail = &Monster->ObjectTail;
		int			I;

		if (!ReadObj(&Rec, Rm, MonInvType + N))
			continue;
		for (I = 0; I < Rec.Num && I < SavedItems; I++)
		{
			if (!(Pt = malloc(sizeof(ObjectType))))
				Abort(" Out of memory loading monster inventory.");
			*Pt = Rec.Obj[I];
			Pt->Next = NULL;
			if (Pt->ObClass == Chest)
			{
				Pt->Object.Chest.ObjectTail = NULL;	/* contents aren't saved */
				Pt->Object.Chest.NumInside = 0;
			}
			*Tail = Pt;
			Tail = &Pt->Next;
		}
	}
}

/* Save the inventory of each monster in room *Rm* (first SavedItems). */
static void WriteMonInv(RmCodeType Rm, MonsterPoint Monster)
{
	int		N;

	for (N = 0; N < SavedItems; N++)
	{
		if (Monster && Monster->ObjectTail)
		{
			ObjRec		Rec;
			ObjectPoint	Pt;
			int			I;

			memset(&Rec, 0, sizeof(Rec));
			for (Pt = Monster->ObjectTail, I = 0; Pt && I < SavedItems;
				 Pt = Pt->Next, I++)
				Rec.Obj[I] = *Pt;
			Rec.Room = Rm;
			Rec.DataType = MonInvType + N;
			Rec.Num = I;
			WriteObj(&Rec, Rm, MonInvType + N);
		}
		else
			DeleteObjType(Rm, MonInvType + N);

		if (Monster)
			Monster = Monster->Next;
	}
}

static int ReadObj(ObjRec *Rec, int Room, int Type)
{
	int		Slot;

	if ((Slot = FindRec(EDBObj, sizeof(ObjRec), Room, Type, NULL)) < 0)
		return 0;
	SeekRec(EDBObj, (long)Slot * sizeof(ObjRec));
	return fread((char *)Rec, sizeof(ObjRec), 1, EDBObj) == 1;
}

static void WriteObj(ObjRec *Rec, int Room, int Type)
{
	PutRec(EDBObj, sizeof(ObjRec), Rec, Room, Type);
}

/*
 * dbobj and dbmon are hash tables of DBSlots records, probed linearly from
 * HashObj().  Every record starts with a RecHead.  A Room of 0 marks a slot
 * never used, which ends a probe; a deleted record gets Room Deleted, which
 * doesn't, so records stored past it stay reachable (#64).
 */
#define	DBSlots		1009
#define	Deleted		(-1)

typedef struct
{
	RmCodeType	Room;
	char		DataType;
	char		Num;
} RecHead;

static int HashObj(int Room, int Type)
{
	return (int)(((long)Room * 73 + Type) % DBSlots);
}

/*
 * The slot holding (Room, Type) in table fp, or -1.  If Free isn't NULL,
 * *Free is set to the first deleted or unused slot on the way, where a new
 * record would go, or -1 if there is none.
 */
static int FindRec(FILE *fp, size_t RecSize, int Room, int Type, int *Free)
{
	RecHead	Head;
	int		Slot, N;

	if (Free)
		*Free = -1;
	Slot = HashObj(Room, Type);
	for (N = 0; N < DBSlots; N++, Slot = (Slot + 1) % DBSlots)
	{
		SeekRec(fp, (long)Slot * RecSize);
		if (fread((char *)&Head, sizeof(Head), 1, fp) != 1)
			break;
		if (Head.Room == 0 || Head.Room == Deleted)
		{
			if (Free && *Free < 0)
				*Free = Slot;
			if (Head.Room == 0)
				break;
		}
		else
		if (Head.Room == Room && Head.DataType == Type)
			return Slot;
	}
	return -1;
}

/* Store Rec as the record for (Room, Type), replacing any old one */
static void PutRec(FILE *fp, size_t RecSize, void *Rec, int Room, int Type)
{
	int		Slot, Free;

	if ((Slot = FindRec(fp, RecSize, Room, Type, &Free)) < 0
	&&	(Slot = Free) < 0)
		Abort(fp == EDBObj ? " Object DB full on write!"
						   : " Monster DB full on write!");
	SeekRec(fp, (long)Slot * RecSize);
	fwrite((char *)Rec, RecSize, 1, fp);
	fflush(fp);
}

static void DeleteRec(FILE *fp, size_t RecSize, int Room, int Type)
{
	RmCodeType	Mark = Deleted;
	int			Slot;

	if ((Slot = FindRec(fp, RecSize, Room, Type, NULL)) < 0)
		return;
	SeekRec(fp, (long)Slot * RecSize);
	fwrite((char *)&Mark, sizeof(Mark), 1, fp);
	fflush(fp);
}

void ReadUMon(RmCodeType Rm, MonsterPoint *Monster)
{
	MonRec	MRec;

	if (ReadMon(&MRec, Rm, 0))
	{
		MonsterPoint	Pt;
		int				N;

		Pt = malloc(sizeof(MonsterType));
		memset(Pt, 0, sizeof(*Pt));

		*Monster = Pt;

		*Pt = MRec.Mon[0];
		Pt->Next = NULL;
		Pt->DefPlayer = NULL;
		Pt->DefMon = NULL;
		Pt->CharmPlayer = NULL;
		Pt->ObjectTail = NULL;

		for (N = 1; N < MRec.Num; N++)
		{
			Pt->Next = malloc(sizeof(MonsterType));
			Pt = Pt->Next;
			memset(Pt, 0, sizeof(*Pt));

			*Pt = MRec.Mon[N];
			Pt->Next = NULL;
			Pt->DefPlayer = NULL;
			Pt->DefMon = NULL;
			Pt->CharmPlayer = NULL;
			Pt->ObjectTail = NULL;
		}
		ReadMonInv(Rm, *Monster);
	}
	else
		*Monster = NULL;
}

static int ReadMon(MonRec *Rec, int Room, int Type)
{
	int		Slot;

	if ((Slot = FindRec(EDBMon, sizeof(MonRec), Room, Type, NULL)) < 0)
		return 0;
	SeekRec(EDBMon, (long)Slot * sizeof(MonRec));
	return fread((char *)Rec, sizeof(MonRec), 1, EDBMon) == 1;
}

void WriteUMon(RmCodeType Rm, MonsterPoint Monster)
{
	MonsterPoint	Pt;
	MonRec			MRec;
	int				N;

	WriteMonInv(Rm, Monster);
	for (Pt = Monster, N = 0; Pt && N < SavedItems; Pt = Pt->Next, N++)
		MRec.Mon[N] = *Pt;

	MRec.Room = Rm;
	MRec.DataType = 0;
	MRec.Num = N;
	PutRec(EDBMon, sizeof(MonRec), &MRec, Rm, 0);
}

void DeleteMon(RmCodeType Rm)
{
	WriteMonInv(Rm, NULL);		/* deletes every monster's inventory */
	DeleteRec(EDBMon, sizeof(MonRec), Rm, 0);
}

/* Read template *Index* from the MList file.  Pointer fields hold
 * whatever the writer had in memory, so they are cleared here. */
int ReadMList(int Index, MonsterPoint Mon)
{
	int		Found;

	SeekRec(MList, (long)Index * sizeof(MonsterType));
	Found = fread((char *)Mon, sizeof(MonsterType), 1, MList) == 1;
	if (!Found)
		memset(Mon, 0, sizeof(*Mon));
	Mon->Next = NULL;
	Mon->DefMon = NULL;
	Mon->DefPlayer = NULL;
	Mon->CharmPlayer = NULL;
	Mon->ObjectTail = NULL;
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

/* Read template *Index* from the OList file, clearing pointer fields. */
int ReadOList(int Index, ObjectPoint Obj)
{
	int		Found;

	SeekRec(OList, (long)Index * sizeof(ObjectType));
	Found = fread((char *)Obj, sizeof(ObjectType), 1, OList) == 1;
	if (!Found)
		memset(Obj, 0, sizeof(*Obj));
	Obj->Next = NULL;
	if (Obj->ObClass == Chest)
		Obj->Object.Chest.ObjectTail = NULL;
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
