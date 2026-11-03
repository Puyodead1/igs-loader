"""Read the loader's software lists (hash/<machine>.xml, MAME-style).

    import softlist
    sw = softlist.get('scratch_m101cn')
    sw.machine, sw.description, sw.cloneof, sw.feature('asset_mask'),
    sw.disk_name, sw.disk_sha1, sw.roms  [(cabinet path, size, sha1)]
    sw.tunables  [Tunable: name, type, default, group, text]

The lists are found in $IGS_HASHPATH (';'-separated directories) or in the
loader repo's hash/.  Format: docs/IGS_LOADER.md, "Rom sets".
"""
import glob
import os
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Tunable:
    """<info name="tunable" value="NAME type default [Group] text"/>: a game plugin's
    environment variable; type int, hex, bool, str or enum:a|b|c; default '-' = unset."""

    def __init__(self, name, type, default, group, text):
        self.name, self.type, self.default, self.group, self.text = name, type, default, group, text

    def choices(self):
        return self.type[5:].split('|') if self.type.startswith('enum:') else None

    def __repr__(self):
        return 'Tunable(%s %s %s)' % (self.name, self.type, self.default)


def parse_tunable(v):
    f = v.split(None, 3)
    if len(f) < 4:
        raise ValueError('tunable %r is not "NAME type default text"' % v)
    group, text = '', f[3]
    if text.startswith('[') and ']' in text:
        group, text = text[1:].split(']', 1)
    return Tunable(f[0], f[1], f[2], group, text.strip())


class Software:
    def __init__(self, machine, e):
        self.machine = machine
        self.name = e.get('name')
        self.cloneof = e.get('cloneof')
        text = lambda tag: (e.findtext(tag) or '').strip()
        self.description, self.year, self.publisher = text('description'), text('year'), text('publisher')
        self.info = {i.get('name'): i.get('value') for i in e.iter('info')}
        self.tunables = [parse_tunable(i.get('value')) for i in e.iter('info') if i.get('name') == 'tunable']
        self.features = {}
        for f in list(e.iter('sharedfeat')) + list(e.iter('feature')):
            self.features[f.get('name')] = f.get('value')
        d = e.find('.//disk')
        self.disk_name = d.get('name') if d is not None else None
        self.disk_sha1 = d.get('sha1') if d is not None else None
        self.roms = [(r.get('name'), int(r.get('size', '-1')), r.get('sha1'))
                     for r in e.iter('rom')]

    def feature(self, name, default=None):
        return self.features.get(name, default)

    def manifest(self):
        """The old KEY=VALUE view: upper-case feature names, plus NAME/MACHINE/PLUGIN."""
        m = {k.upper(): v for k, v in self.features.items()}
        m.update(NAME=self.description, MACHINE=self.machine, ID=self.name)
        if self.cloneof:
            m['PARENT'] = self.cloneof
        return m


def hash_dirs():
    env = os.environ.get('IGS_HASHPATH')
    return [d for d in env.split(';') if d] if env else [os.path.join(ROOT, 'hash')]


def load():
    out = {}
    for d in hash_dirs():
        for path in sorted(glob.glob(os.path.join(d, '*.xml'))):
            root = ET.parse(path).getroot()
            if root.tag != 'softwarelist':
                continue
            for e in root.iter('software'):
                sw = Software(root.get('name'), e)
                if sw.name in out:
                    raise ValueError('software %s is in both %s and %s' % (sw.name, out[sw.name].machine, sw.machine))
                out[sw.name] = sw
    return out


def get(name):
    lists = load()
    if name not in lists:
        raise SystemExit('unknown game %r; known: %s' % (name, ', '.join(sorted(lists))))
    return lists[name]
