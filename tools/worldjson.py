#!/usr/bin/env python3
"""Convert a Scepter world between the text format (docs/world-format.md)
and JSON, and check that the two copies of a world agree.

    worldjson.py to-json  WORLD JSON     # write JSON from the world file
    worldjson.py to-world JSON WORLD     # write the world file from JSON
    worldjson.py check    WORLD JSON     # exit 1 unless they are in sync

The world file is the source.  Both outputs are canonical: to-world writes
its own comments, so comments added by hand to a world file are not kept.
"""
import json
import sys

DIRS = ['north', 'south', 'east', 'west', 'up', 'down']
ROLES = ['bar', 'newsstand', 'pawnshop', 'smith', 'store']

# (json key, world key, type); type is int, bool, str, desc or a list type
WORLD_KEYS = [
    ('name', 'name', str), ('startRoom', 'start-room', int),
    ('reviveRoom', 'revive-room', int), ('jailRoom', 'jail-room', int),
    ('monsterSpeed', 'monster-speed', int), ('notice', 'notice', str),
]

OBJ_KEYS = [
    ('name', 'name', str), ('article', 'article', str), ('kind', 'kind', str),
    ('weight', 'weight', int), ('price', 'price', int), ('desc', 'desc', 'desc'),
    ('carry', 'carry', bool), ('magic', 'magic', bool),
    ('permanent', 'permanent', bool), ('invisible', 'invisible', bool),
    ('cursed', 'cursed', bool),
]

KIND_KEYS = {
    'weapon': [('minDamage', 'min-damage', int), ('maxDamage', 'max-damage', int),
               ('strikes', 'strikes', int), ('skill', 'skill', str),
               ('plus', 'plus', int)],
    'armor': [('plus', 'plus', int), ('hits', 'hits', int)],
    'shield': [('plus', 'plus', int), ('hits', 'hits', int)],
    'coins': [('multiplier', 'multiplier', int)],
    'scroll': [('spell', 'spell', str)],
    'ring': [('spell', 'spell', str), ('level', 'level', int)],
    'device': [('spell', 'spell', str), ('charges', 'charges', int),
               ('level', 'level', int)],
    'chest': [('closed', 'closed', bool), ('trap', 'trap', int),
              ('lock', 'lock', int)],
    'door': [('to', 'to', int), ('closed', 'closed', bool), ('lock', 'lock', int),
             ('trap', 'trap', int), ('spring', 'spring', bool),
             ('toll', 'toll', bool)],
    'keys': [('unlocks', 'unlocks', int), ('uses', 'uses', int)],
    'portal': [('to', 'to', int)],
    'teleport': [('to', 'to', int), ('activeRoom', 'active-room', int)],
    'treasure': [], 'card': [], 'misc': [],
}

MON_KEYS = [
    ('name', 'name', str), ('article', 'article', str), ('desc', 'desc', 'desc'),
    ('level', 'level', int), ('hits', 'hits', int), ('maxHits', 'max-hits', int),
    ('ac', 'ac', int), ('attackSpeed', 'attack-speed', int),
    ('parley', 'parley', int), ('treasureTable', 'treasure-table', int),
] + [(j, w, bool) for j, w in [
    ('defend', 'defend'), ('attackLast', 'attack-last'), ('follow', 'follow'),
    ('guard', 'guard'), ('block', 'block'), ('slowReact', 'slow-react'),
    ('fastReact', 'fast-react'), ('moralReact', 'moral-react'),
    ('immoralReact', 'immoral-react'), ('nice', 'nice'),
    ('assistance', 'assistance'), ('magic', 'magic'),
    ('invisible', 'invisible'), ('regenerate', 'regenerate'), ('drain', 'drain'),
    ('poison', 'poison'), ('antiMagic', 'anti-magic'), ('undead', 'undead'),
    ('flee', 'flee'), ('castsSpells', 'casts-spells'),
    ('summonHelp', 'summon-help'), ('unique', 'unique'), ('watch', 'watch'),
    ('permanent', 'permanent')]]

ROOM_KEYS = [
    ('out', 'out', int), ('desc', 'desc', 'desc'),
    ('encounterTable', 'encounter-table', int),
    ('encounterRate', 'encounter-rate', int), ('greeting', 'greeting', str),
    ('safe', 'safe', bool), ('dark', 'dark', bool),
    ('antiMagic', 'anti-magic', bool), ('notifyDM', 'notify-dm', bool),
]


class WorldError(Exception):
    pass


# ---------- JSON <-> world values ----------

def to_text(v, t):
    if t == 'desc':
        return f"{v['record']}:{v['line']}"
    if t is bool:
        return 'yes' if v else 'no'
    if isinstance(v, list):
        return ' '.join(map(str, v))
    return str(v)


def from_text(s, t, where):
    try:
        if t == 'desc':
            r, l = s.split(':')
            return {'record': int(r), 'line': int(l)}
        if t is bool:
            if s not in ('yes', 'no'):
                raise ValueError
            return s == 'yes'
        if t is int:
            return int(s)
        return s
    except ValueError:
        raise WorldError(f"{where}: bad value '{s}'")


def put_keys(out, d, keys):
    for jk, wk, t in keys:
        if jk in d:
            out.append(f"{wk} = {to_text(d[jk], t)}")


def take_keys(kv, keys, where):
    d = {}
    for jk, wk, t in keys:
        if wk in kv:
            d[jk] = from_text(kv.pop(wk), t, where)
    return d


# ---------- JSON -> world ----------

def obj_text(o):
    out = []
    put_keys(out, o, OBJ_KEYS)
    sub = o.get(o['kind'], {})
    put_keys(out, sub, KIND_KEYS[o['kind']])
    return out


def block(out, header, lines):
    out.append(f"[{header}]")
    out.extend(lines)
    out.append('')


def to_world(w):
    out = [f"# Scepter world \"{w['name']}\".",
           "# Format: docs/world-format.md.  Keep var/*/world.json in sync with",
           "# tools/worldjson.py (make check-worlds).", '']
    wl = []
    put_keys(wl, w, WORLD_KEYS)
    for i, n in enumerate(w.get('news', []), 1):
        wl.append(f"news-{i} = {n}")
    block(out, 'world', wl)

    out.append("# ---- descriptions: RECORD:LINE is referenced by desc = ----\n")
    for d in w['descriptions']:
        block(out, f"description {d['record']}",
              [f"{i} = {l}" for i, l in enumerate(d['lines'], 1)])

    out.append("# ---- object templates (OList) ----\n")
    for o in w['objects']:
        block(out, f"object {o['id']}", obj_text(o))

    out.append("# ---- monster templates (MList) ----\n")
    for m in w['monsters']:
        ml = []
        put_keys(ml, m, MON_KEYS)
        block(out, f"monster {m['id']}", ml)

    out.append("# ---- random encounter and treasure tables ----\n")
    for e in w['encounters']:
        block(out, f"encounter {e['id']}", [f"monsters = {to_text(e['entries'], list)}"])
    for t in w['treasures']:
        block(out, f"treasure {t['id']}", [f"objects = {to_text(t['entries'], list)}"])

    out.append("# ---- rooms ----\n")
    for r in w['rooms']:
        rid = r['id']
        rl = [f"{d} = {to}" for d, to in zip(DIRS, r['exits']) if to]
        put_keys(rl, r, ROOM_KEYS)
        for role in ROLES:
            if r.get(role) or (role == 'store' and 'stock' in r):
                rl.append(f"role = {role}")
        for k in ('stock', 'staggers', 'trains'):
            if k in r:
                rl.append(f"{k} = {to_text(r[k], list)}")
        block(out, f"room {rid}", rl)
        for k, o in enumerate(r.get('objects', []), 1):
            block(out, f"room {rid} / object {k}", obj_text(o))
            for j, c in enumerate(o.get('chest', {}).get('contents', []), 1):
                block(out, f"room {rid} / object {k} / object {j}", obj_text(c))
        for k, m in enumerate(r.get('monsters', []), 1):
            ml = []
            put_keys(ml, m, MON_KEYS)
            block(out, f"room {rid} / monster {k}", ml)
            for j, o in enumerate(m.get('inventory', []), 1):
                block(out, f"room {rid} / monster {k} / object {j}", obj_text(o))
    return '\n'.join(out).rstrip() + '\n'


# ---------- world -> JSON ----------

def parse_blocks(text, fname):
    blocks, cur = [], None
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line[0] in '#;':
            continue
        if line.startswith('['):
            if not line.endswith(']'):
                raise WorldError(f"{fname}:{n}: bad block header")
            toks = line[1:-1].replace('/', ' ').split()
            cur = (toks, {}, n)
            blocks.append(cur)
            continue
        if cur is None or '=' not in line:
            raise WorldError(f"{fname}:{n}: expected key = value inside a block")
        k, v = line.split('=', 1)
        k, v = k.strip(), v.strip()
        if k in cur[1]:
            raise WorldError(f"{fname}:{n}: duplicate key '{k}'")
        cur[1][k] = v
    return blocks


def obj_json(kv, where):
    o = take_keys(kv, OBJ_KEYS, where)
    kind = o.get('kind')
    if kind not in KIND_KEYS:
        raise WorldError(f"{where}: bad or missing kind")
    sub = take_keys(kv, KIND_KEYS[kind], where)
    if sub or kind == 'chest':
        o[kind] = sub
    return o


def done(kv, where):
    if kv:
        raise WorldError(f"{where}: unknown keys {sorted(kv)}")


def to_json(text, fname='world'):
    w = {}
    descs, rooms, mons, objs, encs, tres = [], {}, [], [], [], []
    for toks, kv, n in parse_blocks(text, fname):
        where = f"{fname}:{n}"
        kind = toks[0]
        if kind == 'world':
            w.update(take_keys(kv, WORLD_KEYS, where))
            news = [kv.pop(f"news-{i}") for i in range(1, 6) if f"news-{i}" in kv]
            if news:
                w['news'] = news
            done(kv, where)
            continue
        ident = int(toks[1])
        if kind == 'description':
            lines = [kv.pop(str(i)) for i in range(1, 11) if str(i) in kv]
            done(kv, where)
            descs.append({'record': ident, 'lines': lines})
        elif kind == 'object':
            objs.append({'id': ident, **obj_json(kv, where)})
            done(kv, where)
        elif kind == 'monster':
            mons.append({'id': ident, **take_keys(kv, MON_KEYS, where)})
            done(kv, where)
        elif kind in ('encounter', 'treasure'):
            key = 'monsters' if kind == 'encounter' else 'objects'
            entries = [int(x) for x in kv.pop(key, '').split()]
            done(kv, where)
            (encs if kind == 'encounter' else tres).append(
                {'id': ident, 'entries': entries})
        elif kind == 'room' and len(toks) == 2:
            exits = [int(kv.pop(d, 0)) for d in DIRS]
            r = {'id': ident, 'exits': exits}
            r.update(take_keys(kv, ROOM_KEYS, where))
            role = kv.pop('role', None)
            if role:
                if role not in ROLES:
                    raise WorldError(f"{where}: bad role '{role}'")
                if role != 'store' or 'stock' not in kv:
                    r[role] = True
            for k, t in (('stock', int), ('staggers', int), ('trains', str)):
                if k in kv:
                    r[k] = [t(x) for x in kv.pop(k).split()]
            done(kv, where)
            rooms[ident] = r
        elif kind == 'room':
            r = rooms[ident]
            if toks[2] == 'object' and len(toks) == 4:
                r.setdefault('objects', []).append(obj_json(kv, where))
            elif toks[2] == 'object':
                chest = r['objects'][-1].setdefault('chest', {})
                chest.setdefault('contents', []).append(obj_json(kv, where))
            elif len(toks) == 4:
                r.setdefault('monsters', []).append(take_keys(kv, MON_KEYS, where))
            else:
                m = r['monsters'][-1]
                m.setdefault('inventory', []).append(obj_json(kv, where))
            done(kv, where)
        else:
            raise WorldError(f"{where}: unknown block '{kind}'")

    w['descriptions'] = descs
    w['rooms'] = list(rooms.values())
    w['monsters'] = mons
    w['objects'] = objs
    w['encounters'] = encs
    w['treasures'] = tres
    return dump_json(w)


def dump_json(w):
    def scalar(v):
        return not isinstance(v, (dict, list))

    def flat(v):
        return scalar(v) or (isinstance(v, list) and all(scalar(x) for x in v))

    def emit(v, ind=0):
        pad = '  ' * ind
        if isinstance(v, dict):
            if all(flat(x) for x in v.values()):
                s = json.dumps(v, ensure_ascii=False)
                if len(s) <= 100:
                    return s
            items = [f'{pad}  {json.dumps(k)}: {emit(x, ind + 1)}' for k, x in v.items()]
            return '{\n' + ',\n'.join(items) + '\n' + pad + '}'
        if isinstance(v, list):
            if all(scalar(x) for x in v):
                s = json.dumps(v, ensure_ascii=False)
                if len(s) <= 100:
                    return s
            items = [f'{pad}  {emit(x, ind + 1)}' for x in v]
            return '[\n' + ',\n'.join(items) + '\n' + pad + ']'
        return json.dumps(v, ensure_ascii=False)

    return emit(w) + '\n'


def main(argv):
    if len(argv) != 4 or argv[1] not in ('to-json', 'to-world', 'check'):
        sys.exit(__doc__)
    cmd, a, b = argv[1:]
    try:
        if cmd == 'to-json':
            out = to_json(open(a).read(), a)
            open(b, 'w').write(out)
        elif cmd == 'to-world':
            open(b, 'w').write(to_world(json.load(open(a))))
        else:
            world = open(a).read()
            js = to_json(world, a)
            problems = []
            if js != open(b).read():
                problems.append(f"{b} is out of date; run: {argv[0]} to-json {a} {b}")
            if to_world(json.loads(js)) != world:
                problems.append(f"{a} is not in canonical form; run: "
                                f"{argv[0]} to-world {b} {a} after updating {b}")
            if problems:
                sys.exit('\n'.join(problems))
    except WorldError as e:
        sys.exit(str(e))


if __name__ == '__main__':
    main(sys.argv)
