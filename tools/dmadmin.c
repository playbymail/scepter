/*
 * dmadmin -- manage a game's DMs from outside the game (#139).
 *
 *	dmadmin GAMEDIR list
 *	dmadmin GAMEDIR history
 *	dmadmin GAMEDIR create NAME RANK
 *	dmadmin GAMEDIR rank NAME RANK
 *	dmadmin GAMEDIR password NAME
 *	dmadmin GAMEDIR remove NAME
 *
 * RANK is master, dm or associate, or for "rank" also none.  "list" prints
 * the DMs, most senior first; "history" every change of rank, by the game
 * or by dmadmin, oldest first.  "create" makes a new 2nd-level DM
 * character in the start room; "remove" deletes a DM character.  The last
 * master DM can't be demoted or removed.  "create" and "password" read the
 * password twice, not echoed, if standard input is a terminal, or else
 * from its first line; never from the command line.  Each change of rank
 * is recorded with the account that ran dmadmin: $SUDO_USER under sudo,
 * otherwise the login name, so runuser doesn't hide who it was.
 *
 * Stop the server before changing anything (docs/running.md): the game
 * saves a character who is playing over a change made here.  "list" and
 * "history" only read, and may run at any time.  The game directory is
 * opened as scepterd opens it.
 *
 * main.c is included so the server's own start-up code can be reused.
 */
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <termios.h>

#define main scepterd_main
#include "main.c"
#undef main

static const char	*RankWord[] = { "none", "associate", "dm", "master" };

static void Usage(void)
{
	fprintf(stderr,
		"usage: dmadmin GAMEDIR list\n"
		"       dmadmin GAMEDIR history\n"
		"       dmadmin GAMEDIR create NAME master|dm|associate\n"
		"       dmadmin GAMEDIR rank NAME master|dm|associate|none\n"
		"       dmadmin GAMEDIR password NAME\n"
		"       dmadmin GAMEDIR remove NAME\n");
	exit(2);
}

static void Fail(const char *Fmt, ...)
{
	va_list	Args;

	fprintf(stderr, "dmadmin: ");
	va_start(Args, Fmt);
	vfprintf(stderr, Fmt, Args);
	va_end(Args);
	fprintf(stderr, "\n");
	DBRollback();
	DBClose();
	exit(1);
}

static void Report(const char *Msg)
{
	fprintf(stderr, "dmadmin: %s\n", Msg);
}

static int RankNamed(const char *Word, int NoneOk)
{
	int		R;

	for (R = NoneOk ? RNone : RAssoc; R <= RMaster; R++)
		if (!strcmp(Word, RankWord[R]))
			return R;
	Fail("%s: no such rank; use master, dm, associate%s", Word,
		 NoneOk ? " or none" : "");
	return -1;
}

/*
 * NAME as the game saves it, in capitals.  A new name must be one the
 * login prompt takes as a name: 1 to 10 letters, and not a word the
 * command parser skips.
 */
static void SavedKey(const char *Name, char *Key, int New)
{
	static const char	*Skipped[] =
	{
		"AT", "FOR", "FROM", "IN", "OF", "ON", "SPELL", "TO", "THE", "WITH",
		"STOP", NULL
	};
	const char			*c;
	int					I;

	if (!*Name || strlen(Name) > sizeof(Alfa) - 1)
		Fail("%s: a name is 1 to %d letters", Name, (int)sizeof(Alfa) - 1);
	for (c = Name; *c; c++)
		if (!(*c >= 'A' && *c <= 'Z' || *c >= 'a' && *c <= 'z'))
			Fail("%s: a name is 1 to %d letters", Name, (int)sizeof(Alfa) - 1);
	strcpy(Key, Name);
	CapAlfa(Key);
	if (New)
		for (I = 0; Skipped[I]; I++)
			if (!strcmp(Key, Skipped[I]))
				Fail("%s: the game can't take that as a name", Name);
}

/* Read saved character Key into *P, with its objects, to save it back */
static void Load(const char *Key, UserPoint P)
{
	int		Found;

	if (!GetPlayer(Key, P, &Found))
		Fail("cannot read %s", Key);
	if (!Found)
		Fail("%s: no such character", Key);
}

/* Save *P, read by Load(), as Key, and free its objects */
static void Save(const char *Key, UserPoint P)
{
	ObjectPoint	O;
	int			Ok = PutPlayer(Key, P);

	while ((O = P->ObjectTail))
	{
		P->ObjectTail = O->Next;
		free(O);
	}
	if (!Ok)
		Fail("cannot save %s", Key);
}

/*
 * Who is running dmadmin, for the history: the account sudo was run from,
 * or the one that logged in (not "scepter" under runuser), or else the
 * account dmadmin runs as.
 */
static void Account(char *By, size_t Size)
{
	const char		*Sudo = getenv("SUDO_USER"), *Login = getlogin();
	struct passwd	*Pw;

	if (Sudo && *Sudo)
		snprintf(By, Size, "%s", Sudo);
	else
	if (Login && *Login)
		snprintf(By, Size, "%s", Login);
	else
	if ((Pw = getpwuid(getuid())))
		snprintf(By, Size, "%s", Pw->pw_name);
	else
		snprintf(By, Size, "uid %ld", (long)getuid());
}

static void Record(const char *Key, int Old, int New)
{
	RankChangeType	R;

	R.Time = RealTime;
	snprintf(R.Player, sizeof(R.Player), "%s", Key);
	R.Old = Old;
	R.New = New;
	R.Tool = TRUE;
	Account(R.By, sizeof(R.By));
	if (!PutRankChange(&R))
		Fail("cannot record the change of rank");
}

/* One line of Fp into Line, without its newline; FALSE at end of file */
static int GetLine(FILE *Fp, char *Line, size_t Size)
{
	size_t	Len;

	if (!fgets(Line, (int)Size, Fp))
		return FALSE;
	Len = strlen(Line);
	if (Len && Line[Len - 1] == '\n')
		Line[--Len] = '\0';
	else
	if (Len == Size - 1)
		Fail("the password is longer than %d characters", MaxPWLen);
	return TRUE;
}

/* Line without leading and trailing spaces, as the game reads a password */
static void Trim(char *Line)
{
	size_t	Len = strlen(Line), Start = strspn(Line, " ");

	while (Len > Start && Line[Len - 1] == ' ')
		Len--;
	memmove(Line, Line + Start, Len - Start);
	Line[Len - Start] = '\0';
}

/* Ask for a password on the terminal that is standard input, not echoed */
static void Ask(const char *Prompt, char *Line, size_t Size)
{
	struct termios	Old, Quiet;
	int				Ok;

	fprintf(stderr, "%s", Prompt);
	if (tcgetattr(STDIN_FILENO, &Old))
		Fail("cannot turn off echo: %s", strerror(errno));
	Quiet = Old;
	Quiet.c_lflag &= ~(tcflag_t)ECHO;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &Quiet))
		Fail("cannot turn off echo: %s", strerror(errno));
	Ok = GetLine(stdin, Line, Size);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &Old);
	fprintf(stderr, "\n");
	if (!Ok)
		Fail("no password given");
}

/* A new password, hashed into Hash */
static void NewPassword(char *Hash)
{
	char	PassWord[MaxPWLen + 2], Again[MaxPWLen + 2];
	size_t	Len;

	if (isatty(STDIN_FILENO))
	{
		Ask("Password: ", PassWord, sizeof(PassWord));
		Ask("Again: ", Again, sizeof(Again));
		if (strcmp(PassWord, Again))
			Fail("the passwords don't match");
	}
	else
	if (!GetLine(stdin, PassWord, sizeof(PassWord)))
		Fail("no password on standard input");
	Trim(PassWord);
	Len = strlen(PassWord);
	if (Len < MinPWLen || Len > MaxPWLen)
		Fail("a password is %d to %d characters", MinPWLen, MaxPWLen);
	if (!SetPW(Hash, PassWord))
		Fail("cannot hash the password");
}

static void ListDMs(void)
{
	UserType	P;
	long		After = 0;
	int			Found, Any = FALSE;

	for (;;)
	{
		if (!NextPlayer(&After, &P, &Found))
			Fail("cannot read the characters");
		if (!Found)
			break;
		if (RankOf(&P) == RNone)
			continue;
		if (RankOf(&P) == RAssoc)
			printf("%-10s associate %d\n", P.Name, P.Assoc);
		else
			printf("%-10s %s\n", P.Name, RankWord[RankOf(&P)]);
		Any = TRUE;
	}
	if (!Any)
		printf("no DMs\n");
}

static void ShowHistory(void)
{
	RankChangeType	R;
	long			After = 0;
	int				Found;
	char			When[32];
	time_t			T;

	for (;;)
	{
		if (!NextRankChange(&After, &R, &Found))
			Fail("cannot read the history");
		if (!Found)
			break;
		T = (time_t)R.Time;
		strftime(When, sizeof(When), "%Y-%m-%d %H:%M:%S", localtime(&T));
		printf("%s  %-10s %s -> %s  by %s (%s)\n", When, R.Player,
			   R.Old < 0 ? "new" : RankWord[R.Old],
			   R.New < 0 ? "removed" : RankWord[R.New], R.By,
			   R.Tool ? "dmadmin" : "game");
	}
}

/* Fail unless a master DM other than saved character Key is left */
static void NotLastMaster(const char *Key, const char *Doing)
{
	if (!MasterCount(NULL, Key))
		Fail("%s is the last master DM, so can't be %s", Key, Doing);
}

static void CreateDM(const char *Name, const char *Word)
{
	UserType	P;
	Alfa		Key;
	int			R = RankNamed(Word, FALSE), Have;

	SavedKey(Name, Key, TRUE);
	if ((Have = HavePlayer(Key)) < 0)
		Fail("cannot read the characters");
	if (Have)
		Fail("%s: there is already a character with that name", Key);

	P = ProtoUser;
	strcpy(P.Name, Key);
	P.Lvl = 2;					/* the game saves only 2nd level and up */
	P.Class = DM;
	P.RmCode = StartRoom;
	P.LastAccess = Today;
	SetRank(&P, R);
	NewPassword(P.PWHash);

	if (!DBBegin() || !PutPlayer(Key, &P))
		Fail("cannot save %s", Key);
	Record(Key, -1, R);
	if (!DBCommit())
		Fail("cannot save %s", Key);
	printf("%s: made, rank %s\n", Key, RankWord[R]);
}

static void SetDMRank(const char *Name, const char *Word)
{
	UserType	P;
	Alfa		Key;
	int			R = RankNamed(Word, TRUE), Old;

	SavedKey(Name, Key, FALSE);
	if (!DBBegin())
		Fail("cannot change %s", Key);
	Load(Key, &P);
	if ((Old = RankOf(&P)) == R)
	{
		DBRollback();
		printf("%s: already %s\n", Key, R == RNone ? "not a DM" : RankWord[R]);
		return;
	}
	if (Old == RMaster)
		NotLastMaster(Key, "demoted");
	SetRank(&P, R);
	Save(Key, &P);
	Record(Key, Old, R);
	if (!DBCommit())
		Fail("cannot save %s", Key);
	printf("%s: %s -> %s\n", Key, RankWord[Old], RankWord[R]);
}

static void ResetPassword(const char *Name)
{
	UserType	P;
	Alfa		Key;
	char		Hash[PWHashLen];

	SavedKey(Name, Key, FALSE);
	NewPassword(Hash);
	Load(Key, &P);
	strcpy(P.PWHash, Hash);
	Save(Key, &P);
	printf("%s: password set\n", Key);
}

static void RemoveDM(const char *Name)
{
	UserType	P;
	Alfa		Key;
	int			Old, Found;

	SavedKey(Name, Key, FALSE);
	if (!DBBegin())
		Fail("cannot remove %s", Key);
	Load(Key, &P);
	if ((Old = RankOf(&P)) == RNone)
		Fail("%s is not a DM; dmadmin removes only DMs", Key);
	if (Old == RMaster)
		NotLastMaster(Key, "removed");
	if (!DropPlayer(Key, &Found) || !Found)
		Fail("cannot remove %s", Key);
	Record(Key, Old, -1);
	if (!DBCommit())
		Fail("cannot remove %s", Key);
	printf("%s: removed\n", Key);
}

int main(int argc, char **argv)
{
	const char	*Cmd;
	int			Args;

	if (argc < 3)
		Usage();
	Cmd = argv[2];
	Args = argc - 3;
	if (!(!strcmp(Cmd, "list") && Args == 0 || !strcmp(Cmd, "history") && Args == 0
	||	  !strcmp(Cmd, "create") && Args == 2 || !strcmp(Cmd, "rank") && Args == 2
	||	  !strcmp(Cmd, "password") && Args == 1 || !strcmp(Cmd, "remove") && Args == 1))
		Usage();
	if (chdir(argv[1]))
	{
		perror(argv[1]);
		return 1;
	}

	/* the same start-up as scepterd's main(), without players */
	DBReport = Report;
	if (!OpenGame("dmadmin"))
		return 1;
	QCre();
	Init_Scepter();
	LoadSpecial();
	Init();
	RealTime = time(NULL);
	Today = GetDate(RealTime);

	if (!strcmp(Cmd, "list"))
		ListDMs();
	else
	if (!strcmp(Cmd, "history"))
		ShowHistory();
	else
	if (!strcmp(Cmd, "create"))
		CreateDM(argv[3], argv[4]);
	else
	if (!strcmp(Cmd, "rank"))
		SetDMRank(argv[3], argv[4]);
	else
	if (!strcmp(Cmd, "password"))
		ResetPassword(argv[3]);
	else
		RemoveDM(argv[3]);

	DBClose();
	return ferror(stdout) || fflush(stdout) ? 1 : 0;
}
