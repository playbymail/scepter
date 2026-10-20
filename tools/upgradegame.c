/*
 * upgradegame -- bring a game directory's data files to this build's format.
 *
 *	upgradegame [-n] GAMEDIR
 *
 *	-n	only report what would change
 *
 * scepterd refuses any data file whose format version is not its own
 * (datafile.c, docs/data-files.md).  This converts an older game, one
 * version at a time, to the version this build reads:
 *
 *	0 -> 1	files from before headers existed get a header; the records
 *			are unchanged
 *	1 -> 2	player records gain PWHash (#82), empty, so each character
 *			logs in once more with its old password, which is then
 *			hashed with bcrypt; the other records are unchanged
 *
 * Stop the server first.  Nothing is changed unless every file can be
 * upgraded.  The whole game directory is first copied to
 * GAMEDIR.backup-YYYYMMDD-HHMMSS.  Each file is then rewritten to a
 * temporary file, flushed to disk and renamed over the original, keeping
 * its owner and mode, so a file is always either old or new.  If the
 * upgrade stops part way, run it again: files already upgraded are left
 * alone.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "common.h"
#include "scepter.h"

static const char	*Game;
static int			DryRun;

/* A version 0 or 1 player record: UserFRec without UserSave's PWHash */
typedef struct
{
	_Alignas(UserSave) char	Saved[offsetof(UserSave, PWHash)];
} UserSave1;

typedef struct
{
	UserSave1	User;
	short		NObj;
	short		NWeap, NArm, NShield, NRingL, NRingR;
	ObjectType	Objs[MaxObjs];
} UserFRec1;

/* The record size of Kind in format Version */
static size_t OldRecSize(int Kind, unsigned long Version)
{
	return Kind == DPerson && Version < 2 ? sizeof(UserFRec1) : DataKind[Kind].RecSize;
}

static void Fail(const char *Fmt, const char *Arg)
{
	fprintf(stderr, "upgradegame: ");
	fprintf(stderr, Fmt, Arg);
	fprintf(stderr, "\n");
	exit(1);
}

/* Copy From to To (a new file), keeping the mode and, if allowed, the owner. */
static void CopyFile(const char *From, const char *To, const struct stat *st)
{
	char	Buf[65536];
	ssize_t	n;
	int		in, out;

	if ((in = open(From, O_RDONLY)) < 0)
		Fail("%s: cannot read", From);
	if ((out = open(To, O_WRONLY | O_CREAT | O_EXCL, st->st_mode & 07777)) < 0)
		Fail("%s: cannot create", To);
	while ((n = read(in, Buf, sizeof Buf)) > 0)
		if (write(out, Buf, n) != n)
			Fail("%s: write failed", To);
	if (n < 0 || fsync(out) || close(out))
		Fail("%s: copy failed", To);
	close(in);
	if (chown(To, st->st_uid, st->st_gid))
		;	/* not root: the copy is ours, which is fine */
}

/* Copy GAMEDIR to GAMEDIR.backup-TIME, one directory level deep (dat/, ram/). */
static void Backup(char *Dest, size_t Len)
{
	time_t		t = time(NULL);
	char		Stamp[32];
	const char	*Sub[] = { "", "/dat", "/ram" };
	int			i;

	strftime(Stamp, sizeof Stamp, "%Y%m%d-%H%M%S", localtime(&t));
	snprintf(Dest, Len, "%s.backup-%s", Game, Stamp);

	for (i = 0; i < 3; i++)
	{
		char			From[1100], To[1100];
		struct stat		st;
		DIR				*d;
		struct dirent	*e;

		snprintf(From, sizeof From, "%s%s", Game, Sub[i]);
		snprintf(To, sizeof To, "%s%s", Dest, Sub[i]);
		if (stat(From, &st))
			continue;
		if (mkdir(To, st.st_mode & 07777))
			Fail("%s: cannot create the backup directory", To);
		if (chown(To, st.st_uid, st.st_gid))
			;
		if (!(d = opendir(From)))
			Fail("%s: cannot list", From);
		while ((e = readdir(d)))
		{
			char	F[1400], T[1400];

			snprintf(F, sizeof F, "%s/%s", From, e->d_name);
			snprintf(T, sizeof T, "%s/%s", To, e->d_name);
			if (!stat(F, &st) && S_ISREG(st.st_mode))
				CopyFile(F, T, &st);
		}
		closedir(d);
	}
}

/* Version 1 -> 2: give a player record an empty PWHash. */
static void Upgrade1Person(const UserFRec1 *Old, struct tUserFRec *New)
{
	memset(New, 0, sizeof(*New));
	memcpy(&New->User, &Old->User, sizeof(Old->User.Saved));
	/* the rest is laid out alike: copy it whole, padding and all */
	memcpy(&New->NObj, &Old->NObj, sizeof(*Old) - offsetof(UserFRec1, NObj));
}

_Static_assert(sizeof(UserFRec1) - offsetof(UserFRec1, NObj)
			   == sizeof(struct tUserFRec) - offsetof(struct tUserFRec, NObj),
			   "player records end alike in versions 1 and 2");

/* Rewrite a file of format Version with this build's header and records. */
static void Upgrade(const char *Path, int Kind, unsigned long Version)
{
	char		Tmp[1200], Buf[65536];
	struct stat	st;
	FILE		*in, *out;
	size_t		n;
	int			fd;

	snprintf(Tmp, sizeof Tmp, "%s.upgrade", Path);
	if (stat(Path, &st))
		Fail("%s: cannot read", Path);
	if (!(in = fopen(Path, "rb")))
		Fail("%s: cannot read", Path);
	unlink(Tmp);
	if ((fd = open(Tmp, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 07777)) < 0)
		Fail("%s: cannot create", Tmp);
	if (!(out = fdopen(fd, "wb")))
		Fail("%s: cannot create", Tmp);
	if (!WriteDataHead(out, Kind))
		Fail("%s: write failed", Tmp);
	if (Version > 0 && fseek(in, DataHeadSize, SEEK_SET))
		Fail("%s: cannot read", Path);
	if (Kind == DPerson && Version < 2)
	{
		UserFRec1			Old;
		struct tUserFRec	New;

		while (fread(&Old, sizeof Old, 1, in) == 1)
		{
			Upgrade1Person(&Old, &New);
			if (fwrite(&New, sizeof New, 1, out) != 1)
				Fail("%s: write failed", Tmp);
		}
	}
	else
		while ((n = fread(Buf, 1, sizeof Buf, in)) > 0)
			if (fwrite(Buf, 1, n, out) != n)
				Fail("%s: write failed", Tmp);
	if (ferror(in) || fflush(out) || fsync(fd) || fclose(out))
		Fail("%s: write failed", Tmp);
	fclose(in);
	if (chown(Tmp, st.st_uid, st.st_gid))
		;
	if (rename(Tmp, Path))
		Fail("%s: cannot replace", Path);
}

int main(int argc, char **argv)
{
	int				Result[NDataFiles];
	unsigned long	Version[NDataFiles], RecSize;
	long			Size;
	int				i, Opt, Bad = 0, Todo = 0;
	char			Path[1100], Msg[256], Dest[1100];

	while ((Opt = getopt(argc, argv, "n")) != -1)
		switch (Opt)
		{
			case 'n':	DryRun = TRUE;	break;
			default:	Fail("usage: upgradegame [-n] GAMEDIR%s", "");
		}
	if (argc - optind != 1)
		Fail("usage: upgradegame [-n] GAMEDIR%s", "");
	Game = argv[optind];

	/* look at every file before changing any */
	for (i = 0; i < NDataFiles; i++)
	{
		FILE	*fp;
		int		Kind = DataFile[i].Kind;

		snprintf(Path, sizeof Path, "%s/%s", Game, DataFile[i].Name);
		if (!(fp = fopen(Path, "rb")))
		{
			fprintf(stderr, "%s: %s\n", Path, strerror(errno));
			Bad++;
			continue;
		}
		Result[i] = ReadDataHead(fp, Kind, &Version[i], &RecSize);
		if (fseek(fp, 0L, SEEK_END) || (Size = ftell(fp)) < 0)
			Result[i] = DataUnreadable;
		fclose(fp);

		/* ReadDataHead() judges a headerless file by this build's records */
		if (Result[i] == DataNoHead || Result[i] == DataForeign)
			Result[i] = Size % OldRecSize(Kind, 0) ? DataForeign : DataNoHead;
		else if (Result[i] == DataWrongVersion && Version[i] < DataVersion)
		{
			if (RecSize != OldRecSize(Kind, Version[i]))
				Result[i] = DataWrongSize;
			else if ((Size - DataHeadSize) % RecSize)
				Result[i] = DataForeign;
		}

		if (Result[i] == DataOK)
			printf("%s: version %d, up to date\n", DataFile[i].Name, DataVersion);
		else if (Result[i] == DataNoHead || Result[i] == DataWrongVersion
				 && Version[i] < DataVersion)
		{
			printf("%s: version %lu, %s to version %d\n", DataFile[i].Name,
				   Version[i], DryRun ? "would upgrade" : "upgrading", DataVersion);
			Todo++;
		}
		else
		{
			/* a newer version, a foreign file, or a different ABI */
			DataProblem(Result[i], Kind, Version[i], RecSize, Msg, sizeof Msg);
			fprintf(stderr, "%s: %s\n", Path, Msg);
			Bad++;
		}
	}

	if (Bad)
		Fail("nothing changed: %s cannot be upgraded", Game);
	if (!Todo)
	{
		printf("%s is already at format version %d\n", Game, DataVersion);
		return 0;
	}
	if (DryRun)
		return 0;

	Backup(Dest, sizeof Dest);
	printf("backed up %s to %s\n", Game, Dest);
	for (i = 0; i < NDataFiles; i++)
		if (Result[i] != DataOK)
		{
			snprintf(Path, sizeof Path, "%s/%s", Game, DataFile[i].Name);
			Upgrade(Path, DataFile[i].Kind, Version[i]);
		}
	if (CheckGame(Game))
		Fail("%s: upgraded files fail the check; restore the backup", Game);
	printf("%s is now at format version %d\n", Game, DataVersion);
	return 0;
}
