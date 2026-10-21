#include <crypt.h>

#include "common.h"
#include "scepter.h"

/*
 * Saved characters are rows of the game database (dbstore.c).  A name is
 * saved in capitals, as CapAlfa() folds it, so "bob" finds "BOB".  Every
 * failure of the store is fatal: it is logged, and the server stops
 * rather than lose a save.
 */

/* Name as it is saved: at most an Alfa, in capitals */
static void SavedName(const char *Name, char *Key)
{
	snprintf(Key, sizeof(Alfa), "%s", Name);
	CapAlfa(Key);
}

int PlayerOnFile(char *Name)
{
	Alfa	Key;
	int		Found;

	SavedName(Name, Key);
	if ((Found = HavePlayer(Key)) < 0)
		Abort(" Sce203 - Cannot read the players.");
	return Found;
}

/* Copy the name of the Nth (from 0, most senior first) saved player to Name */
int PlayerName(int N, char *Name)
{
	UserType	P;
	long		After = 0;
	int			Found;

	do
		if (!NextPlayer(&After, &P, &Found))
			Abort(" Sce203 - Cannot read the players.");
	while (Found && N-- > 0);
	if (Found)
		strcpy(Name, P.Name);
	return Found;
}

/*
 * Load saved player Name into *Player, ready to log in, with its objects
 * in the order they were saved (#155).  If there is none, *Player is
 * ProtoUser with an empty name.
 */
void ReadPlayer(UserPoint Player, char *Name)
{
	ObjectPoint	Pt;
	Alfa		Key;
	int			Found;

	SavedName(Name, Key);
	if (!GetPlayer(Key, Player, &Found))
		Abort(" Sce203 - Cannot read a player.");
	if (!Found)
	{
		*Player = ProtoUser;
		*Player->Name = '\0';
		return;
	}

	strcpy(Player->Name, Name);
	Player->Weight		= 0;
	Player->AC			= 10;
	Player->Next		= NULL;
	Player->Pending		= NULL;
	Player->Follow		= NULL;
	Player->DefPlayer	= NULL;
	Player->DefMon		= NULL;
	Player->LastInput	= RealTime;
	Player->LastAtk		= RealTime - 50;
	Player->LastHeal	= RealTime - 50;
	Player->HitAtTime	= RealTime - 50;
	Player->Drunk		= RealTime;
	Player->Entry		= XName;

	for (Pt = Player->ObjectTail; Pt; Pt = Pt->Next)
	{
		Player->Weight += Pt->Weight;
		if (Pt == Player->USArm)
			Player->AC = Max(-50, Min(50, Player->AC - Pt->Object.Armor.ArmPlus));
		else
		if (Pt == Player->USShield)
			Player->AC = Max(-50, Min(50, Player->AC - Pt->Object.Shield.ShPlus));
	}
}

void FreePlayer(UserPoint Player)
{
	while (Player->ObjectTail)
	{
		ObjectPoint	Pt;

		Pt = Player->ObjectTail;
		Player->ObjectTail = Player->ObjectTail->Next;
		ODestroy(Pt);
	}
	free(Player);
}

/* Save *Player and its first MaxObjs objects, in one transaction */
void WritePlayer(UserPoint Player)
{
	Alfa	Key;

	SavedName(Player->Name, Key);
	if (!PutPlayer(Key, Player))
		Abort(" Sce206 - Error updating person.");
}

void DeleteUsr(char *Nm)
{
	Alfa	Key;
	int		Found;

	SavedName(Nm, Key);
	if (!DropPlayer(Key, &Found))
		Abort(" Sce208 - Error while deleting player.");
	if (!Found)
		QOut(Term, " Sce208 - Error while deleting player.");
}

/*
 * Rename saved player OldNm to NewNm, which is capitalised in place; an
 * empty NewNm deletes the player.  A name another saved player has is
 * refused, and FALSE returned, so two characters never share one (#151).
 */
int ChangeUsrName(char *OldNm, char *NewNm)
{
	Alfa	Old;
	int		Result;

	SavedName(OldNm, Old);
	CapAlfa(NewNm);
	if (!*NewNm)
	{
		if (!DropPlayer(Old, &Result))
			Abort(" Sce208 - Error while changing name.");
	}
	else
	if (!RenamePlayer(Old, NewNm, &Result))
		Abort(" Sce208 - Error while changing name.");
	if (Result < 0)
	{
		QOut(Term, "0That name is already taken.");
		return FALSE;
	}
	if (!Result)
		QOut(Term, " Sce208 - Error while changing name.");
	return TRUE;
}

/* List the saved players, most senior first */
void Users(char *Word)
{
	UserType	P;
	long		After = 0;
	int			Found;

	UNUSED(Word);

	for (;;)
	{
		if (!NextPlayer(&After, &P, &Found))
			Abort(" Sce203 - Cannot read the players.");
		if (!Found)
			break;
		if (User->SSJ)
			snprintf(B1, sizeof(B1), "0%c%c%-10s %4d %02d/%02d",
					P.SSJ ? '*' : ' ', P.It ? '+' : ' ', P.Name, P.Lvl,
					P.LastAccess >> 7, P.LastAccess & 0x3f);
		else
			snprintf(B1, sizeof(B1), "0%s", P.Name);
		QOut(Term, B1);
	}
}

UserPoint FindPlayer(char *Word, UserPoint PlayerTail)
{
	UserPoint	Usr;
	NameTList	UserNameList;
	int			Index, Count = 0;

	CapAlfa(Word);
	for (Usr = PlayerTail; Usr && Count < MaxNames; Usr = Usr->Next)
	{
		Count++;
		strcpy(UserNameList[Count], Usr->Name);
		CapAlfa(UserNameList[Count]);
	}
	if (Count >= MaxNames)
		QOut(Term, "0 Sce31 - Player table overflow!");

	if ((Index = WordMatch(Word, Count, UserNameList)) <= 0)
		return NULL;

	Usr = PlayerTail;
	for (Count = 1; Count < Index; Count++)
		Usr = Usr->Next;
	return Usr;
}

UserPoint FindUser(char *Word)
{
	UserPoint	Usr;
	NameTList	UserNameList;
	int			Index, Count = 0;

	CapAlfa(Word);
	for (Usr = UserTail; Usr && Count < MaxNames; Usr = Usr->NextUser)
	{
		Count++;
		strcpy(UserNameList[Count], Usr->Name);
		CapAlfa(UserNameList[Count]);
	}
	if (Count >= MaxNames)
		QOut(Term, "0 Sce31 - User table overflow!");

	if ((Index = WordMatch(Word, Count, UserNameList)) <= 0)
		return NULL;

	Usr = UserTail;
	for (Count = 1; Count < Index; Count++)
		Usr = Usr->NextUser;
	return Usr;
}

UserPoint FindPid(int FPid)
{
	UserPoint	Usr;

	for (Usr = UserTail; Usr; Usr = Usr->NextUser)
		if (Usr->XPid == FPid)
			return Usr;
	return NULL;
}

/* Hashing passwords, with libxcrypt's bcrypt (#82) */
static struct crypt_data	PWData;

/* Store a bcrypt hash of PassWord in PWHash; FALSE if it could not be made. */
int SetPW(char *PWHash, char *PassWord)
{
	char	Setting[CRYPT_GENSALT_OUTPUT_SIZE];

	*PWHash = '\0';
	if (!crypt_gensalt_rn("$2b$", PWCost, NULL, 0, Setting, sizeof(Setting))
	||	!crypt_rn(PassWord, Setting, &PWData, sizeof(PWData))
	||	strlen(PWData.output) >= PWHashLen)
		return FALSE;
	strcpy(PWHash, PWData.output);
	return TRUE;
}

/* Is PassWord the one hashed in PWHash? */
int CheckPW(char *PWHash, char *PassWord)
{
	char	Setting[PWHashLen];
	int		ILoop, Diff;

	if (!*PWHash)
		return FALSE;
	snprintf(Setting, sizeof(Setting), "%.*s", PWHashLen - 1, PWHash);
	if (!crypt_rn(PassWord, Setting, &PWData, sizeof(PWData)))
		return FALSE;
	for (Diff = ILoop = 0; Setting[ILoop]; ILoop++)
		Diff |= Setting[ILoop] ^ PWData.output[ILoop];
	return !Diff && !PWData.output[ILoop];
}

/*
 * DM ranks (#139).  A character is a DM if SSJ is set: an associate if
 * Assoc is also set, otherwise a master if Master is.  An associate's
 * Assoc number only labels them ("Assoc DM 3"); any number restricts the
 * same commands.
 */
RankType RankOf(UserPoint P)
{
	if (!P->SSJ)
		return RNone;
	if (P->Assoc)
		return RAssoc;
	return P->Master ? RMaster : RDM;
}

/* Give P rank Rank; an associate keeps an Assoc number they have */
void SetRank(UserPoint P, RankType Rank)
{
	P->SSJ = Rank != RNone;
	P->Master = Rank == RMaster;
	if (Rank != RAssoc)
		P->Assoc = 0;
	else
	if (!P->Assoc)
		P->Assoc = 1;
}

/* Is a character named Name (in capitals) logged in and playing? */
static int Playing(const char *Name)
{
	UserPoint	Usr;
	Alfa		Key;

	for (Usr = UserTail; Usr; Usr = Usr->NextUser)
	{
		SavedName(Usr->Name, Key);
		if (Usr->Status == SNormal && !strcmp(Key, Name))
			return TRUE;
	}
	return FALSE;
}

/*
 * The master DMs other than the playing character Except and the character
 * named ExceptSaved (in capitals), playing or saved; either may be NULL.
 * A character who is playing counts as they are now, not as they were
 * last saved.
 */
int MasterCount(UserPoint Except, const char *ExceptSaved)
{
	UserPoint	Usr;
	UserType	P;
	Alfa		Key;
	long		After = 0;
	int			Found, Count = 0;

	for (Usr = UserTail; Usr; Usr = Usr->NextUser)
	{
		SavedName(Usr->Name, Key);
		if (Usr != Except && (!ExceptSaved || strcmp(Key, ExceptSaved))
		&&	Usr->Status == SNormal && RankOf(Usr) == RMaster)
			Count++;
	}
	for (;;)
	{
		if (!NextPlayer(&After, &P, &Found))
			Abort(" Sce203 - Cannot read the players.");
		if (!Found)
			return Count;
		if ((!ExceptSaved || strcmp(P.Name, ExceptSaved)) && !Playing(P.Name)
		&&	RankOf(&P) == RMaster)
			Count++;
	}
}

/*
 * Record that User changed character Name's rank from Old to New, each a
 * RankType or -1 for a character made or removed.
 */
void RankChanged(const char *Name, int Old, int New)
{
	RankChangeType	R;

	R.Time = RealTime;
	SavedName(Name, R.Player);
	snprintf(R.By, sizeof(R.By), "%s", User->Name);
	CapAlfa(R.By);
	R.Old = Old;
	R.New = New;
	R.Tool = FALSE;
	if (!PutRankChange(&R))
		Abort(" Cannot save a rank change.");
}

long Expr(int Lvl)
{
	if (Lvl <= 1)
		return 0L;
	if (Lvl > 24)
		return 99999999L;
	return 64L << Lvl;
}

void PrintExp(UserPoint Plyr)
{
	long	Exp;

	Exp = Expr(Plyr->Lvl + 1) - Expr(Plyr->Lvl) - Plyr->Experience;
	snprintf(B1, sizeof(B1), "0You need %ld more exp points to train for the next level.",
			Exp > 0 ? Exp : 0L);
	QOut(Term, B1);
	snprintf(B1, sizeof(B1), "0You have %ld shillings in cash.", Plyr->Money);
	QOut(Term, B1);
}

void Inventory(void)
{
	if (!User->ObjectTail)
		QOut(Term, "0You are carrying nothing at all.");
	else
	{
		ObjectPoint	Object;
		int			Count, ILoop;

		Count = 0;
		for (Object = User->ObjectTail; Object; Object = Object->Next)
			Count++;

		QOut(Term, "0You are carrying the following objects:");
		strcpy(B1, "0");
		ILoop = 0;
		for (Object = User->ObjectTail;
			Object && ILoop < Count;
			Object = Object->Next)
		{
			ILoop++;
			PrintObj(Object, FALSE, ILoop == 1, B2);
			strcat(B1, B2);
			Punctuate(ILoop, Count, B1);
		}
		strcat(B1, ".");
		QOut(Term, B1);
		QOut(Term, "0");
	}
}

void PrintDmg(UserPoint Player, int Damage, int Brf, char *Str)
{
	if (Brf)
	{
		if (!Player->Fatigue)
			sprintf(Str, "%d vit!", Damage);
		else
		if (Damage > Player->Fatigue)
			sprintf(Str, "%d fat, and %d vit!",
					Player->Fatigue, Damage - Player->Fatigue);
		else
			sprintf(Str, "%d fat!", Damage);
	}
	else
	{
		if (!Player->Fatigue)
			sprintf(Str, "%d vitality points!", Damage);
		else
		if (Damage > Player->Fatigue)
			sprintf(Str, "%d fatigue points, and %d vitality pts!",
					Player->Fatigue, Damage - Player->Fatigue);
		else
			sprintf(Str, "%d fatigue points!", Damage);
	}
}

int ReadyCheck(long LastAtk)
{
	if (RealTime + 1 < LastAtk)
	{
		snprintf(B1, sizeof(B1), "0Not ready!  Wait %ld more seconds.", LastAtk - RealTime);
		QOut(Term, B1);
	}
	else
	if (User->Drunk - RealTime >= 60)
		QOut(Term, "0You're too hammered to do anything!");
	else
		return TRUE;

	return FALSE;
}

void PromptUser(void)
{
	char	*p = NULL;

	switch ((int)User->Entry)
	{
		case XCmd:		QOut(Term, "1");							break;
		case XName:		p = "Enter your name";						break;
		case XEdit:		p = User->Brief ? "Ed" : "Edit command";	break;
		case XPassword:	p = "Enter your password";					break;
		case XNewPW:	p = "Enter access password";				break;
		case XOldPW:	p = "Enter your password";					break;
		case XChangePW:	p = "Enter new password";					break;
		case XAgainPW:	p = "Enter the new password again";		break;
		case XNewClass:	p = "Class";								break;
		case XStats:	p = "Stats";								break;
		case XNotice:	p = "Enter notice";							break;
		case XSex:	p = "Finally, is your character a male or female"; break;
		case XDead:
			QOut(Term, "0Congratulations!  Your death has made the newspapers!");
			QOut(Term, "0In one line, try to explain what killed you and how.");
			QOut(Term, "0Use words like \"he\" and \"she\" instead of \"I\".");
			QOut(Term, "0Example:  \"His weapon broke and before he knew it, the");
			QOut(Term, "0vampire drained his life away...\"");
			p = "";
			break;
		case XNews:		p = "Enter news";							break;
		case XSell:
		case XRepair:	p = "Yes/No";								break;
		case XWish:		p = "Enter wish";							break;
		case XSpell:	p = "Say chant";							break;
		case XSure:		p = "Are you sure";							break;
		case XParley:	p = "Would you like to have it, Effendi";	break;
		case XSkill:	p = "Skill";								break;
		case XDesc:		p = "Enter description";					break;
	}
	if (p)
	{
		snprintf(B1, sizeof(B1), "1%s? ", p);
		QOut(Term, B1);
	}
}

void Init(void)
{
	int		ILoop;

	for (ILoop = 0; ILoop < 6; ILoop++)
		ProtoRoom.Adjoin[ILoop] = 0;
	ProtoRoom.Out			= 0;
	ProtoRoom.DescCode		= 0;
	ProtoRoom.DescRec		= 0;
	ProtoRoom.LastDir		= 0;
	ProtoRoom.WhichEncounter= 0;
	ProtoRoom.EncounterTime	= 0;
	ProtoRoom.NotifyDM		= FALSE;
	ProtoRoom.Safe			= FALSE;
	ProtoRoom.AntiMagic		= FALSE;
	ProtoRoom.Dark			= FALSE;
	ProtoRoom.RmPlayerTail	= NULL;
	ProtoRoom.RmMonsterTail	= NULL;
	ProtoRoom.RmObjectTail	= NULL;

	strcpy(ProtoObject.Name, "thing,*");
	ProtoObject.Next		= NULL;
	ProtoObject.Article		= A;
	ProtoObject.Carry		= TRUE;
	ProtoObject.Weight		= 1;
	ProtoObject.Price		= 0;
	ProtoObject.Magic		= FALSE;
	ProtoObject.Permanent	= FALSE;
	ProtoObject.Invisible	= FALSE;
	ProtoObject.DescCode	= 0;
	ProtoObject.DescRec		= 0;
	ProtoObject.ObClass		= Misc;

	strcpy(ProtoMonster.Name, "something,*");
	ProtoMonster.Next		= NULL;
	ProtoMonster.DefPlayer	= NULL;
	ProtoMonster.Lvl		= 0;
	ProtoMonster.Hits		= 10;
	ProtoMonster.MaxHits	= 10;
	ProtoMonster.ObjectTail	= NULL;
	ProtoMonster.Top		= TRUE;
	ProtoMonster.Num		= 1;
	ProtoMonster.Permanent	= TRUE;
	ProtoMonster.Magic		= FALSE;
	ProtoMonster.Defend		= TRUE;
	ProtoMonster.AtkLastAggr= TRUE;
	ProtoMonster.WhichObj	= 0;
	ProtoMonster.MReact		= 0;
	ProtoMonster.MParley	= 0;

	ProtoUser.Name[0]		= '\0';
	ProtoUser.Weight		= 0;
	ProtoUser.ObjectTail	= NULL;
	ProtoUser.Lvl			= 1;
	ProtoUser.Class			= Fighter;
	ProtoUser.Hits			= 10;
	ProtoUser.MaxHits		= 10;
	ProtoUser.Magic			= 10;
	ProtoUser.MaxMagic		= 10;
	ProtoUser.Experience	= 0;
	ProtoUser.AGuild		= FALSE;
	ProtoUser.AC			= 10;
	ProtoUser.RmCode		= 1;
	ProtoUser.Str			= 11;
	ProtoUser.Int			= 11;
	ProtoUser.Dex			= 11;
	ProtoUser.Pty			= 11;
	ProtoUser.Con			= 11;
	ProtoUser.Evil			= FALSE;
	ProtoUser.LastAccess	= Today;
	ProtoUser.PW			= 0;
	ProtoUser.Money			= 200;
	ProtoUser.Invisible		= FALSE;
	ProtoUser.Fatigue		= 10;
	ProtoUser.NonExistant	= FALSE;
	ProtoUser.SkillNew		= FALSE;
	ProtoUser.Sex			= Male;
	ProtoUser.Dead			= FALSE;
	ProtoUser.MaxFatigue	= 10;
	ProtoUser.Poisoned		= FALSE;
	ProtoUser.PlayTester	= FALSE;
	ProtoUser.Echo			= FALSE;
	ProtoUser.SSharp		= 0;
	ProtoUser.SThrust		= 0;
	ProtoUser.SBlunt		= 0;
	ProtoUser.SLong			= 0;
	ProtoUser.Hidden		= FALSE;
	ProtoUser.SSJ			= FALSE;
	ProtoUser.Assoc			= 0;
	ProtoUser.Master		= FALSE;
	ProtoUser.TGuild		= FALSE;
	ProtoUser.LastInput		= 0;
	ProtoUser.Next			= NULL;
	ProtoUser.USWeap		= NULL;
	ProtoUser.Pending		= NULL;
	ProtoUser.USArm			= NULL;
	ProtoUser.USShield		= NULL;
	ProtoUser.USRingL		= NULL;
	ProtoUser.USRingR		= NULL;
	ProtoUser.DefPlayer		= NULL;
	ProtoUser.DefMon		= NULL;
	ProtoUser.Entry			= XInit;
	ProtoUser.Follow		= NULL;
	ProtoUser.NextUser		= NULL;
	ProtoUser.LastAtk		= 0;
	ProtoUser.LastHeal		= 0;
	ProtoUser.HitAtTime		= 0;
	ProtoUser.Drunk			= 0;
	ProtoUser.It			= FALSE;
	ProtoUser.Suck			= FALSE;
	ProtoUser.Plague		= FALSE;
	ProtoUser.Brief			= FALSE;
	ProtoUser.MesBlock		= FALSE;
	ProtoUser.Spells		= 0L;
	ProtoUser.LastCmd[0]	= '\0';
	ProtoUser.WData[0]		= '\0';
	ProtoUser.Data			= 0;
}
