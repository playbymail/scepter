/*
 * Game data file headers (docs/data-files.md).
 *
 * Each binary file in a game directory starts with a DataHeadSize-byte
 * header, written byte by byte so that it reads the same on every build:
 *
 *    0  magic, 8 bytes, naming the kind of file ("SCEPDUNG", ...)
 *    8  format version, uint32 little-endian (DataVersion)
 *   12  record size, uint32 little-endian (sizeof the record this build uses)
 *   16  reserved, zero
 *
 * The records follow, unchanged.  scepterd, fightsim and the editor open
 * only files whose magic, version and record size match this build
 * exactly; tools/upgradegame converts older games.  Files from before
 * headers existed count as version 0.
 *
 * This file uses no game state, so mkgame and upgradegame link it alone.
 */
#include <errno.h>
#include <string.h>

#include "common.h"
#include "scepter.h"

DataKindType DataKind[NDataKinds] =
{
	{ "SCEPDUNG", "dungeon",		sizeof(RoomType) },
	{ "SCEPDBOB", "object db",		sizeof(ObjRec) },
	{ "SCEPDBMN", "monster db",		sizeof(MonRec) },
	{ "SCEPPERS", "player file",	sizeof(UserFRec) },
};

DataFileType DataFile[NDataFiles] =
{
	{ "edungeon",	DDungeon },
	{ "dbobj",		DObjDB },
	{ "dbmon",		DMonDB },
	{ "eperson",	DPerson },
};

static unsigned long GetU32(const unsigned char *p)
{
	return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16
		| (unsigned long)p[3] << 24;
}

static void PutU32(unsigned char *p, unsigned long v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8 & 0xff;
	p[2] = v >> 16 & 0xff;
	p[3] = v >> 24 & 0xff;
}

int WriteDataHead(FILE *fp, int Kind)
{
	unsigned char	Head[DataHeadSize];

	memset(Head, 0, sizeof Head);
	memcpy(Head, DataKind[Kind].Magic, 8);
	PutU32(Head + 8, DataVersion);
	PutU32(Head + 12, (unsigned long)DataKind[Kind].RecSize);
	return fwrite(Head, sizeof Head, 1, fp) == 1;
}

/*
 * Read the header at the start of fp, leaving fp just after it.  Returns
 * DataOK, or one of the Data* problems, setting *Version (0 for a file
 * without a header) and *RecSize when they are known.
 */
int ReadDataHead(FILE *fp, int Kind, unsigned long *Version, unsigned long *RecSize)
{
	unsigned char	Head[DataHeadSize];
	size_t			n;
	int				i;

	*Version = *RecSize = 0;
	rewind(fp);
	n = fread(Head, 1, sizeof Head, fp);

	/* Only an exact magic counts: a version 0 player file can start with
	 * any capitalised name, "SCEPTERFAN" included. */
	for (i = 0; i < NDataKinds; i++)
		if (n >= 8 && !memcmp(Head, DataKind[i].Magic, 8))
			break;
	if (i == NDataKinds)
	{
		long	Size;

		/* no magic: a headerless (version 0) file, or not ours at all */
		if (fseek(fp, 0L, SEEK_END) || (Size = ftell(fp)) < 0)
			return DataUnreadable;
		rewind(fp);
		return Size % (long)DataKind[Kind].RecSize ? DataForeign : DataNoHead;
	}
	if (i != Kind)
		return DataWrongKind;
	if (n < sizeof Head)
		return DataShort;
	*Version = GetU32(Head + 8);
	*RecSize = GetU32(Head + 12);
	if (*Version != DataVersion)
		return DataWrongVersion;
	if (*RecSize != DataKind[Kind].RecSize)
		return DataWrongSize;
	return DataOK;
}

/* Describe a ReadDataHead() result in Msg. */
void DataProblem(int Result, int Kind, unsigned long Version,
				 unsigned long RecSize, char *Msg, size_t Len)
{
	switch (Result)
	{
		case DataNoHead:
			snprintf(Msg, Len, "no header (format version 0); this build reads"
					 " version %d, so run upgradegame on the game directory",
					 DataVersion);
			break;
		case DataWrongVersion:
			snprintf(Msg, Len, "format version %lu; this build reads version %d%s",
					 Version, DataVersion, Version < DataVersion
					 ? ", so run upgradegame on the game directory"
					 : ", so it needs a newer scepterd");
			break;
		case DataWrongSize:
			snprintf(Msg, Len, "records are %lu bytes; this build's %s records"
					 " are %lu bytes (built with a different compiler or ABI?)",
					 RecSize, DataKind[Kind].Label,
					 (unsigned long)DataKind[Kind].RecSize);
			break;
		case DataWrongKind:
			snprintf(Msg, Len, "wrong kind of file; expected the %s",
					 DataKind[Kind].Label);
			break;
		case DataShort:
			snprintf(Msg, Len, "header is truncated");
			break;
		case DataForeign:
			snprintf(Msg, Len, "not a Scepter data file (expected the %s)",
					 DataKind[Kind].Label);
			break;
		case DataUnreadable:
			snprintf(Msg, Len, "cannot be read: %s", strerror(errno));
			break;
		default:
			snprintf(Msg, Len, "ok");
			break;
	}
}

/*
 * Open a data file for update and check its header, leaving it positioned
 * at the first record.  On failure, say why on stderr and return NULL.
 */
FILE *OpenData(const char *Path, int Kind)
{
	FILE			*fp;
	unsigned long	Version, RecSize;
	int				Result;
	char			Msg[256];

	if (!(fp = fopen(Path, FOPEN)))
	{
		fprintf(stderr, "%s: %s\n", Path, strerror(errno));
		return NULL;
	}
	if ((Result = ReadDataHead(fp, Kind, &Version, &RecSize)) != DataOK)
	{
		DataProblem(Result, Kind, Version, RecSize, Msg, sizeof Msg);
		fprintf(stderr, "%s: %s\n", Path, Msg);
		fclose(fp);
		return NULL;
	}
	return fp;
}

/* fseek to byte Offset of the records, past the header. */
int SeekRec(FILE *fp, long Offset)
{
	return fseek(fp, DataHeadSize + Offset, SEEK_SET);
}

/*
 * Check every data file in game directory Dir, reporting each problem on
 * stderr.  Returns the number of files that this build cannot open.
 */
int CheckGame(const char *Dir)
{
	int		i, Bad = 0;

	for (i = 0; i < NDataFiles; i++)
	{
		char	Path[1100];
		FILE	*fp;

		snprintf(Path, sizeof Path, "%s/%s", Dir, DataFile[i].Name);
		if (!(fp = OpenData(Path, DataFile[i].Kind)))
			Bad++;
		else
			fclose(fp);
	}
	return Bad;
}
