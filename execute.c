#include "scepter.h"

#define	fVOID	0	/* func(); */
#define	fUSER	1	/* func(User); */
#define	fWORD	2	/* func(Word); */
#define	fNUM	3	/* func(Word, Num); */
#define	fROOM	4	/* func(Word, Num, User->RmCode); */

static struct
{
	int		cmd, type;
	union
	{
		void	(*v)(void);
		void	(*u)(UserPoint);
		void	(*w)(char *);
		void	(*n)(char *, int);
		void	(*r)(char *, int, RmCodeType);
	} func;
} cmdtable[] =
{
	{  8, fROOM,	{ .r = DropObject } },
	{ 11, fROOM,	{ .r = Attack } },
	{ 20, fVOID,	{ .v = NumUsers } },
	{ 22, fUSER,	{ .u = PlayerDisplay } },
	{ 28, fVOID,	{ .v = Help } },
	{ 29, fNUM,	{ .n = ReturnObj } },
	{ 33, fWORD,	{ .w = GetStatus } },
	{ 35, fWORD,	{ .w = Users } },
	{ 40, fVOID,	{ .v = SaveChar } },
	{ 41, fVOID,	{ .v = Inventory } },
	{ 42, fROOM,	{ .r = Attack } },
	{ 43, fROOM,	{ .r = Attack } },
	{ 44, fNUM,	{ .n = EnterSpell } },
	{ 45, fVOID,	{ .v = Appeal } },
	{ 48, fVOID,	{ .v = Catalog } },
	{ 49, fNUM,	{ .n = Pawn } },
	{ 55, fWORD,	{ .w = FollowPlayer } },
	{ 56, fWORD,	{ .w = Lose } },
	{ 57, fNUM,	{ .n = Repair } },
	{ 58, fNUM,	{ .n = ChangeName } },
	{ 59, fNUM,	{ .n = ChangePW } },
	{ 60, fNUM,	{ .n = Offer } },
	{ 61, fVOID,	{ .v = Accept } },
	{ 62, fNUM,	{ .n = Turn } },
	{ 63, fROOM,	{ .r = GetObject } },
	{ 65, fNUM,	{ .n = OpenClose } },
	{ 66, fNUM,	{ .n = OpenClose } },
	{ 67, fNUM,	{ .n = OpenClose } },
	{ 68, fNUM,	{ .n = OpenClose } },
	{ 69, fNUM,	{ .n = OpenClose } },
	{ 70, fNUM,	{ .n = OpenClose } },
	{ 71, fVOID,	{ .v = Track } },
	{ 72, fROOM,	{ .r = Attack } },
	{ 73, fNUM,	{ .n = Hide } },
	{ 74, fVOID,	{ .v = Search } },
	{ 75, fVOID,	{ .v = Train } },
	{ 77, fWORD,	{ .w = TagPlayer } },
	{ 78, fVOID,	{ .v = Run } },
	{ 79, fUSER,	{ .u = PrintExp } },
	{ 81, fNUM,	{ .n = Parley } },
	{ 84, fNUM,	{ .n = StealObj } },
	{ 85, fROOM,	{ .r = Attack } },
	{ 86, fROOM,	{ .r = Attack } },
	{ 87, fVOID,	{ .v = Who } },
	{ 88, fUSER,	{ .u = Condition } },
	{  0 }
};

void ExecuteCommand(void)
{
	int		Num, ILoop;
	Alfa	Cmd, Word;

	GetWord(Cmd, &Num);
	CapAlfa(Cmd);
	if (*Cmd)
		CmdCode = BinaryMatch(Cmd, CmdListLen, CmdList);
	else
		CmdCode = -1;	/* empty C/R */
	if (*Cmd == '*' && !User->SSJ)
		CmdCode = -2;	/* illegal access */

	if (CmdCode > 0)
	{
		strcpy(User->LastCmd, CmdList[CmdCode]);
		CmdCode = CmdNum[CmdCode];
		if (CmdCode != 1  && CmdCode != 2  && CmdCode != 3  && CmdCode != 4
		&&	CmdCode != 5  && CmdCode != 6  && CmdCode != 7  && CmdCode != 16
		&&	CmdCode != 17 && CmdCode != 18 && CmdCode != 19 && CmdCode != 20
		&&	CmdCode != 22 && CmdCode != 25 && CmdCode != 26 && CmdCode != 27
		&&	CmdCode != 28 && CmdCode != 30 && CmdCode != 34 && CmdCode != 39
		&&	CmdCode != 40 && CmdCode != 41 && CmdCode != 45 && CmdCode != 47
		&&	CmdCode != 48 && CmdCode != 51 && CmdCode != 52 && CmdCode != 61
		&&	CmdCode != 71 && CmdCode != 74 && CmdCode != 75 && CmdCode != 76
		&&	CmdCode != 78 && CmdCode != 79 && CmdCode != 83 && CmdCode != 87)
			GetWord(Word, &Num);
	}

	if (User->Assoc		/* these are all *commands... */
	&& (CmdCode == 18 || CmdCode == 19 || CmdCode == 27
	||	CmdCode == 51 || CmdCode == 54))
		CmdCode = -3;

	for (ILoop = 0; cmdtable[ILoop].cmd; ILoop++)
		if (cmdtable[ILoop].cmd == CmdCode)
		{
			switch (cmdtable[ILoop].type)
			{
				case fVOID:
					(*cmdtable[ILoop].func.v)();
					break;
				case fUSER:
					(*cmdtable[ILoop].func.u)(User);
					break;
				case fWORD:
					(*cmdtable[ILoop].func.w)(Word);
					break;
				case fNUM:
					(*cmdtable[ILoop].func.n)(Word, Num);
					break;
				case fROOM:
					(*cmdtable[ILoop].func.r)(Word, Num, User->RmCode);
					break;
			}
			return;
		}

	switch (CmdCode)
	{
		case -3:
			QOut(Term, "0Sorry, associate DMs may not do that.");
			break;

		case -2:
			QOut(Term, "0Sorry... That's for DM use only!");
			break;

		case -1:
			break;

		case 0:
			QOut(Term, "0I don't understand.");
			break;

		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
		case 9:
			FolCount = 0;
			GoDirection(Word, Num);
			break;

		case 10:
			if (Num = UseObject(Word, Num))
				Spell(Num);
			break;

		case 12:
		case 13:
		case 14:
		case 36:
		case 37:
		case 38:
		case 82:
		case 89:
			if (CmdCode == 82 && !User->Master)
				QOut(Term, "0Only master DMs can logoff players.");
			else
				Talk(Word);
			break;

		case 16:
			User->Entry = XEdit;
			break;

		case 17:
		case 39:
			User->Data = CmdCode;
			User->Entry = XSure;
			break;

		case 18:
			TaskClosed = 'C';
			QOut(Term, "0Scepter closed.");
			break;

		case 19:
			TaskClosed = 'O';
			QOut(Term, "0Scepter opened.");
			break;

		case 51:
			TaskClosed = 'T';
			QOut(Term, "0Scepter open for testing only.");
			break;

		case 26:
			sprintf(B1, "0%sA random monster check occurs every %d seconds.",
					ctime(&RealTime), MSpeed);
			QOut(Term, B1);
			break;

		case 27:
		{
			/* The QNX version also dumped 50 words starting at address 0
			 * (the real-mode interrupt vectors); that faults on a
			 * protected-memory OS, so only the slot table is shown. */
			QOut(Term, "0Active  SlotTbl");
			for (ILoop = 0; ILoop <= MaxUsers; ILoop++)
			{
				sprintf(B1, "0%3d     %3d", Active[ILoop], SlotTbl[ILoop]);
				QOut(Term, B1);
			}
			break;
		}

		case 30:
			User->Invisible = !User->Invisible;
			User->NonExistant = FALSE;
			sprintf(B1, "0You are now %svisible.", User->Invisible ? "in" : "");
			QOut(Term, B1);
			break;

		case 34:
			User->Echo = !User->Echo;
			sprintf(B1, "0Echo mode %s.", User->Echo ? "on" : "off");
			QOut(Term, B1);
			break;

		case 21:
			UpdateF();
			Abort(" Sce000 - DM took task down.");
			break;

		case 23:
			UpdateF();
			LastUpdate = RealTime;
			QOut(Term, "0Files updated.");
			break;

		case 24:
		case 31:
		case 64:
			if (!*Word)
				RoomDisplay(User->RmCode, FALSE);
			else
			if (Num = Look(Word, Num))
				Spell(Num);
			break;

		case 25:
			User->Brief = !User->Brief;
			sprintf(B1, "0Brief mode %s.", User->Brief ? "on" : "off");
			QOut(Term, B1);
			break;

		case 32:
			QOut(Term, "0When in doubt, panic!");
			break;

		case 46:
			Buy(Num, User->RmCode);
			break;

		case 47:
			User->Entry = XNotice;
			Loc++;
			break;

		case 50:
			if (Num < 1 || Num > 3)
				QOut(Term, "0Must be 1 to 3.");
			else
			{
				TLvl[Num - 1] = 0;
				QOut(Term, "0Death entry purged.");
			}
			break;

		case 52:
			User->Entry = XNews;
			Loc++;
			break;

		case 53:
			if (Num < 1 || Num > 5)
				QOut(Term, "0Must be 1 to 5.");
			else
			{
				*NewsBuf[Num - 1] = '\0';
				QOut(Term, "0News line deleted.");
			}
			break;

		case 54:
			MSpeed = Max(1, Min(10, Num));
			sprintf(B1, "0Monspeed set to %d.", MSpeed);
			QOut(Term, B1);
			break;

		case 76:
			User->MesBlock = !User->MesBlock;
			sprintf(B1, "0Send messages %sed.",
					User->MesBlock ? "block" : "receiv");
			QOut(Term, B1);
			break;

		case 80:
			QOut(Term, "0Command not implemented yet.");
			break;

		case 83:
			User->Invisible = User->NonExistant = !User->NonExistant;
			QOut(Term, User->NonExistant ?
					"0You no longer exist to the outside world."
				  : "0You now exist");
			break;
	}
}
