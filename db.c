#include "scepter.h"

static int	ReadObj(ObjRec *Rec, int Room, int Type);
static void	WriteObj(ObjRec *Rec, int Room, int Type);
static int	HashObj(int Room, int Type);
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
	ObjRec	Tmp;
	int		RecNum, First;

	First = RecNum = HashObj(Rm, Type);
	SeekRec(EDBObj, (long)RecNum * sizeof(ObjRec));
	while (fread((char *)&Tmp, sizeof(ObjRec), 1, EDBObj))
	{
		if (Tmp.Room == Rm && Tmp.DataType == Type)
		{
			Tmp.Room = 0;
			fseek(EDBObj, (long) -sizeof(ObjRec), 1);
			fwrite((char *)&Tmp, sizeof(ObjRec), 1, EDBObj);
			fflush(EDBObj);
			break;
		}

		if (!Tmp.Room)
			break;

		if (++RecNum >= 1009)
		{
			SeekRec(EDBObj, 0L);
			RecNum = 0;
		}

		if (RecNum == First)
			Abort(" Object DB full on delete!");
	}
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
	int		RecNum, First;

	First = RecNum = HashObj(Room, Type);
	SeekRec(EDBObj, (long)RecNum * sizeof(ObjRec));
	while (fread((char *)Rec, sizeof(ObjRec), 1, EDBObj))
	{
		if (!Rec->Room)
			return 0;
		if (Rec->Room == Room && Rec->DataType == Type)
			return 1;

		if (++RecNum >= 1009)
		{
			SeekRec(EDBObj, 0L);
			RecNum = 0;
		}

		if (RecNum == First)
			Abort(" Object DB full on read!");
	}

	return 0;
}

static void WriteObj(ObjRec *Rec, int Room, int Type)
{
	ObjRec	Tmp;
	int		RecNum, First;

	First = RecNum = HashObj(Room, Type);
	SeekRec(EDBObj, (long)RecNum * sizeof(ObjRec));
	while (fread((char *)&Tmp, sizeof(ObjRec), 1, EDBObj))
	{
		if (Tmp.Room == Room && Tmp.DataType == Type || !Tmp.Room)
		{
			fseek(EDBObj, (long) -sizeof(ObjRec), 1);
			fwrite((char *)Rec, sizeof(ObjRec), 1, EDBObj);
			fflush(EDBObj);
			break;
		}
		
		if (++RecNum >= 1009)
		{
			SeekRec(EDBObj, 0L);
			RecNum = 0;
		}

		if (RecNum == First)
			Abort(" Object DB full on write!");
	}
}

static int HashObj(int Room, int Type)
{
	return (int)(((long)Room * 73 + Type) % 1009);
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
	int		RecNum, First;

	First = RecNum = HashObj(Room, Type);
	SeekRec(EDBMon, (long)RecNum * sizeof(MonRec));
	while (fread((char *)Rec, sizeof(MonRec), 1, EDBMon))
	{
		if (!Rec->Room)
			return 0;
		if (Rec->Room == Room && Rec->DataType == Type)
			return 1;

		if (++RecNum >= 1009)
		{
			SeekRec(EDBMon, 0L);
			RecNum = 0;
		}

		if (RecNum == First)
			Abort(" Monster DB full on read!");
	}

	return 0;
}

void WriteUMon(RmCodeType Rm, MonsterPoint Monster)
{
	MonsterPoint	Pt;
	MonRec			MRec, Tmp;
	int				N, RecNum, First;

	WriteMonInv(Rm, Monster);
	for (Pt = Monster, N = 0; Pt && N < SavedItems; Pt = Pt->Next, N++)
		MRec.Mon[N] = *Pt;

	MRec.Room = Rm;
	MRec.DataType = 0;
	MRec.Num = N;

	First = RecNum = HashObj(Rm, 0);
	SeekRec(EDBMon, (long)RecNum * sizeof(MonRec));
	while (fread((char *)&Tmp, sizeof(MonRec), 1, EDBMon))
	{
		if (Tmp.Room == 0 || Tmp.Room == Rm && Tmp.DataType == 0)
		{
			fseek(EDBMon, (long) -sizeof(MonRec), 1);
			fwrite((char *)&MRec, sizeof(MonRec), 1, EDBMon);
			fflush(EDBMon);
			return;
		}
		
		if (++RecNum >= 1009)
		{
			SeekRec(EDBMon, 0L);
			RecNum = 0;
		}

		if (RecNum == First)
			Abort(" Monster DB full on write!");
	}
}

void DeleteMon(RmCodeType Rm)
{
	MonRec	MRec;
	int		RecNum, First;

	WriteMonInv(Rm, NULL);		/* deletes every monster's inventory */
	First = RecNum = HashObj(Rm, 0);
	SeekRec(EDBMon, (long)RecNum * sizeof(MonRec));
	while (fread((char *)&MRec, sizeof(MonRec), 1, EDBMon))
	{
		if (MRec.Room == Rm && MRec.DataType == 0)
		{
			MRec.Room = 0;
			fseek(EDBMon, (long) -sizeof(MonRec), 1);
			fwrite((char *)&MRec, sizeof(MonRec), 1, EDBMon);
			fflush(EDBMon);
			return;
		}
		
		if (!MRec.Room)
			return;

		if (++RecNum >= 1009)
		{
			SeekRec(EDBMon, 0L);
			RecNum = 0;
		}

		if (RecNum == First)
		 	Abort(" Monster DB full on delete!");
	}
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
