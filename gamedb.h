/*
 * The game database, GAMEDIR/game.db (gamedb.c, docs/data-files.md).
 */
#ifndef	GAMEDB_H
#define	GAMEDB_H

#include <sqlite3.h>

#define	DBName		"game.db"
#define	DBVersion	1			/* PRAGMA user_version this build reads */

/* The object and monster columns of the schema, in dbstore.c's order */
#define	ObjCols	"name, article, weight, price, desc_code, desc_rec, carry, magic," \
	" permanent, invisible, cursed, kind, to_where, min_hp, max_hp, strikes," \
	" wea_plus, weap_type, sh_plus, sh_hits, arm_plus, arm_hits, multiplier," \
	" spell, closed, trap, locked, num_inside, d_to_where, d_closed, d_spring," \
	" d_toll, d_trap, d_locked, unlock, num_uses, m_spell, num_charges, m_level," \
	" t_to_where, t_active_rm, r_spell, r_level"
#define	ObjVals	"?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?," \
	" ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?"
#define	MonCols	"name, article, desc_rec, desc_code, atk_speed, ac, hits, max_hits," \
	" lvl, num, m_react, m_parley, which_obj, defend, block, follow, guard," \
	" atk_last_aggr, slow_react, fast_react, invisible, regenerate, drain," \
	" poison, anti_magic, undead, moral_react, flee, assistance, mon_spells," \
	" top, permanent, magic, summon_help, immoral_react, is_unique, watch, nice"
#define	MonVals	"?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?," \
	" ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?"

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
int		DBStart(int *Own);
int		DBFinish(int Own, int Ok);
void	DBRollback(void);
sqlite3_stmt	*DBPrepare(sqlite3_stmt **Cache, const char *Sql);

#endif
