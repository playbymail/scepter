/*
 * The game database, GAMEDIR/game.db (gamedb.c, docs/data-files.md).
 */
#ifndef	GAMEDB_H
#define	GAMEDB_H

#include <sqlite3.h>

#define	DBName		"game.db"
#define	DBVersion	1			/* PRAGMA user_version this build reads */

extern sqlite3	*GameDB;		/* the open game, for the storage functions */
extern void		(*DBReport)(const char *Msg);	/* where errors go; stderr */

int		DBCreate(const char *Dir);
int		DBOpen(const char *Dir);
void	DBClose(void);
int		DBVersionOf(const char *Dir, int *Version);
int		DBMigrate(const char *Dir, int DryRun);
int		DBBackup(const char *Dir, const char *To);
int		DBRestore(const char *From, const char *Dir);
int		DBBegin(void);
int		DBCommit(void);
void	DBRollback(void);
sqlite3_stmt	*DBPrepare(sqlite3_stmt **Cache, const char *Sql);

#endif
