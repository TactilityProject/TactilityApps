# SPDX-License-Identifier: GPL-3.0-or-later
"""Diacritic folding + Greek/Cyrillic transliteration for search keys (no deps)."""
import unicodedata, re

SPECIAL = {'ł':'l','đ':'d','ø':'o','ß':'ss','æ':'ae','œ':'oe','ð':'d','þ':'th','ı':'i','ħ':'h','ŧ':'t','ŋ':'n','ə':'e','ĳ':'ij','ĸ':'k','ſ':'s'}
APOS = "'’ʼ`´ʻ"

def _strip_marks(ch):
    d = unicodedata.normalize('NFD', ch)
    return ''.join(c for c in d if unicodedata.category(c) != 'Mn')

def fold_latin(s):
    """lowercase, strip diacritics from Latin(-like) letters, punctuation -> single space.
    Greek/Cyrillic letters pass through (lowercased) - see fold_orig for those."""
    out = []
    for ch in s.lower():
        if ch in APOS or unicodedata.category(ch) == 'Mn': continue
        if ch in SPECIAL: out.append(SPECIAL[ch]); continue
        o = ord(ch)
        if o < 128:
            out.append(ch if ch.isalnum() else ' ')
        elif ch.isalnum():
            if 0x370 <= o <= 0x52F:      # Greek/Cyrillic: handled elsewhere
                out.append(ch)
            else:
                out.append(_strip_marks(ch))   # latin w/ marks, vietnamese tones etc.
        else: out.append(' ')
    return re.sub(r'\s+', ' ', ''.join(out)).strip()

def fold_orig(s):
    """folded form kept in the ORIGINAL script (Greek/Cyrillic): lowercase, strip tonos/stress, final sigma, ё->е, ґ->г"""
    out = []
    for ch in s.lower():
        if ch in APOS: continue
        o = ord(ch)
        if 0x370 <= o <= 0x3FF:
            ch = 'σ' if ch == 'ς' else _strip_marks(ch)
        elif 0x400 <= o <= 0x52F:
            ch = {'ё':'е','ґ':'г'}.get(ch, ch)
            ch = ''.join(c for c in ch if c != '́')
        if ch.isalnum() or ch.isspace(): out.append(ch)
        elif unicodedata.category(ch) == 'Mn': continue
        else: out.append(' ')
    return re.sub(r'\s+', ' ', ''.join(out)).strip()

def has_nonlatin(s):
    return any(0x370 <= ord(c) <= 0x52F for c in s)

GREEK = dict(zip('αβγδεζηθικλμνξοπρστυφχψω', ['a','v','g','d','e','z','i','th','i','k','l','m','n','x','o','p','r','s','t','y','f','ch','ps','o']))
GREEK['ς'] = 's'
CYR_BASE = {'а':'a','б':'b','в':'v','г':'g','д':'d','е':'e','ё':'yo','ж':'zh','з':'z','и':'i','й':'y','к':'k','л':'l','м':'m','н':'n','о':'o','п':'p','р':'r','с':'s','т':'t','у':'u','ф':'f','х':'kh','ц':'ts','ч':'ch','ш':'sh','щ':'shch','ъ':'','ы':'y','ь':'','э':'e','ю':'yu','я':'ya','і':'i','ї':'i','є':'ye','ґ':'g','ў':'u'}
CYR_UK = dict(CYR_BASE, **{'г':'h','и':'y','й':'y','ґ':'g','ї':'i','є':'ye','ю':'yu','я':'ya','ь':'','ъ':''})
CYR_SR = dict(CYR_BASE, **{'ђ':'dj','ж':'z','ј':'j','љ':'lj','њ':'nj','ћ':'c','ч':'c','џ':'dz','ш':'s','х':'h','и':'i','ц':'c','ъ':'','ь':''})
PROFILES = {'ru': CYR_BASE, 'uk': CYR_UK, 'sr': CYR_SR}

def translit(s, profile='ru'):
    """Greek (ELOT-743-ish) + Cyrillic -> Latin, then fold_latin."""
    tab = PROFILES[profile]
    s = s.lower()
    # greek: strip accents first, digraphs
    g = []
    for ch in s:
        if 0x370 <= ord(ch) <= 0x3FF: ch = 'σ' if ch == 'ς' else _strip_marks(ch)
        g.append(ch)
    s = ''.join(g)
    out = []; i = 0; n = len(s)
    def wordstart(i): return i == 0 or not s[i-1].isalpha()
    while i < n:
        ch = s[i]; nx = s[i+1] if i+1 < n else ''
        if 0x3B1 <= ord(ch) <= 0x3C9 or ch == 'ς':
            two = ch + nx
            if two == 'ου': out.append('ou'); i += 2; continue
            if two in ('αυ','ευ'):
                nn = s[i+2] if i+2 < n else ''
                out.append(('a' if ch == 'α' else 'e') + ('f' if (nn == '' or nn in 'θκξπστφχψ') else 'v')); i += 2; continue
            if two == 'μπ': out.append('b' if wordstart(i) else 'mp'); i += 2; continue
            if two == 'ντ': out.append('d' if wordstart(i) else 'nt'); i += 2; continue
            if two == 'γγ': out.append('ng'); i += 2; continue
            if two == 'γκ': out.append('g' if wordstart(i) else 'nk'); i += 2; continue
            out.append(GREEK.get(ch, ch)); i += 1; continue
        if 0x400 <= ord(ch) <= 0x52F:
            if ch == 'ї' and profile == 'uk' and wordstart(i): out.append('yi')
            else: out.append(tab.get(ch, ''))
            i += 1; continue
        out.append(ch); i += 1
    return fold_latin(''.join(out))

def search_keys(name, latin=None, en=None, profile='ru', use_en=False):
    """set of folded keys for one feature name."""
    keys = []
    k = fold_latin(name) if not has_nonlatin(name) else None
    if k is None:
        keys.append(translit(name, profile))
        keys.append(fold_orig(name))
        if latin: keys.append(fold_latin(latin))
    else:
        keys.append(k)
        if latin and not has_nonlatin(latin): keys.append(fold_latin(latin))
    if use_en and en and not has_nonlatin(en): keys.append(fold_latin(en))
    seen = []; 
    for x in keys:
        if x and x not in seen: seen.append(x)
    return seen

def suffix_keys(key, minlen=3):
    """word-start suffix keys (so 'stefanika' finds 'milana rastislava stefanika')"""
    w = key.split(' ')
    out = []
    for i in range(1, len(w)):
        if len(w[i]) >= minlen: out.append(' '.join(w[i:]))
    return out

if __name__ == '__main__':
    tests = [('Štefánikova','stefanikova'),('Łódź','lodz'),('Đường','duong'),('Mäsiarska','masiarska'),('İstanbul','istanbul'),('Çankaya','cankaya'),
             ('Székesfehérvár','szekesfehervar'),('Hlavná stanica','hlavna stanica'),('Straße','strasse'),('Ørsted','orsted'),('Nguyễn Thiếp','nguyen thiep'),
             ('Wrocław','wroclaw'),('Győr','gyor'),('nám. Slobody','nam slobody'),('Ulica 29. augusta','ulica 29 augusta'),('Phố Hàng Bạc','pho hang bac')]
    for a,b in tests: assert fold_latin(a)==b,(a,fold_latin(a),b)
    print(translit('Київ','uk'), fold_orig('Київ'), translit('Αθήνα'), fold_orig('Αθήνα'), translit('Београд','sr'), translit('Ποσειδώνος'), translit('Μπακάλικο'), translit('Рейтарська вулиця','uk'), translit('Москва','ru'),translit('Ђорђа Станојевића','sr'))
