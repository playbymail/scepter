/*
 * Special rooms: shops, training halls and the start/revive/jail rooms.
 *
 * The QNX version fixed these by room number.  They now come from the
 * world (see docs/world-format.md); mkgame stores them in the game
 * database: the start, revive and jail rooms in settings, and each room's
 * role, greeting, stock, staggers and trained classes in room_specials,
 * room_stock, room_staggers and room_trains (dbstore.c).
 */
#include "scepter.h"

#define	MaxSpecial	100
#define	MaxStock	20
#define	MaxStagger	20

typedef struct
{
	RmCodeType	Room;
	RoleType	Role;
	char		*Greeting;
	short		NStock, Stock[MaxStock];
	short		NStagger, Stagger[MaxStagger];
	unsigned	Trains;				/* bit (1 << ChType) */
} SpecialType;

static SpecialType	Special[MaxSpecial];
static int			NSpecial;

int		StartRoom = 1;
int		ReviveRoom = 1;
int		JailRoom = 1;

static SpecialType *Find(int WRm)
{
	int		i;

	for (i = 0; i < NSpecial; i++)
		if (Special[i].Room == WRm)
			return &Special[i];
	return NULL;
}

static SpecialType *Add(int WRm)
{
	SpecialType	*p = Find(WRm);

	if (!p)
	{
		if (NSpecial == MaxSpecial)
			Abort(" Too many special rooms.");
		p = &Special[NSpecial++];
		p->Room = (RmCodeType)WRm;
	}
	return p;
}

/* One row of the special rooms (GetSpecials), checked as it is added */
static int AddSpecial(int Kind, int Rm, int Value, RoleType Role,
					  const char *Greeting)
{
	SpecialType	*Sp;

	if (Rm < 1 || Rm > NumRooms)
		Abort(" Bad room in the special rooms.");
	Sp = Add(Rm);
	switch (Kind)
	{
		case 'r':
			Sp->Role = Role;
			if (Greeting)
			{
				if (!(Sp->Greeting = malloc(strlen(Greeting) + 1)))
					Abort(" Out of memory reading the special rooms.");
				strcpy(Sp->Greeting, Greeting);
			}
			break;

		case 's':
			if (Value < 1 || Value > RanObjLen)
				Abort(" Bad stock in the special rooms.");
			if (Sp->NStock < MaxStock)
				Sp->Stock[Sp->NStock++] = (short)Value;
			break;

		case 'g':
			if (Value < 1 || Value > NumRooms)
				Abort(" Bad stagger room in the special rooms.");
			if (Sp->NStagger < MaxStagger)
				Sp->Stagger[Sp->NStagger++] = (short)Value;
			break;

		case 't':
			Sp->Trains |= 1u << Value;
			break;
	}
	return TRUE;
}

void LoadSpecial(void)
{
	SettingsType	Set;

	if (!GetSettings(&Set))
		Abort(" Cannot read the game's settings.");
	if (Set.StartRoom < 1 || Set.StartRoom > NumRooms
	||	Set.ReviveRoom < 1 || Set.ReviveRoom > NumRooms
	||	Set.JailRoom < 1 || Set.JailRoom > NumRooms)
		Abort(" Bad start, revive or jail room.");
	StartRoom = Set.StartRoom;
	ReviveRoom = Set.ReviveRoom;
	JailRoom = Set.JailRoom;
	if (!GetSpecials(AddSpecial))
		Abort(" Cannot read the special rooms.");
}

RoleType RoomRole(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p ? p->Role : RoleNone;
}

char *RoomGreeting(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p ? p->Greeting : NULL;
}

/* Object template number of item *Item* (1-based) for sale in *WRm*. */
int StockItem(int WRm, int Item)
{
	SpecialType	*p = Find(WRm);

	return p && Item >= 1 && Item <= p->NStock ? p->Stock[Item - 1] : 0;
}

int StockCount(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p ? p->NStock : 0;
}

/* Where a drunk staggers to from bar *WRm*. */
int StaggerRoom(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p && p->NStagger ? p->Stagger[Rnd(p->NStagger) - 1] : StartRoom;
}

/* Room *Item* (1-based) of the rooms a drunk staggers to from *WRm*. */
int StaggerItem(int WRm, int Item)
{
	SpecialType	*p = Find(WRm);

	return p && Item >= 1 && Item <= p->NStagger ? p->Stagger[Item - 1] : 0;
}

int StaggerCount(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p ? p->NStagger : 0;
}

int TrainsClass(int WRm, ChType Class)
{
	SpecialType	*p = Find(WRm);

	return p && (p->Trains & (1u << Class));
}
