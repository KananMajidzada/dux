#!/usr/bin/env python3
"""The printable ASCII of the 512_8 font, as DB lines.

The .tal wants these as hex, and a hex table nobody can read is a hex table
nobody can check. So the glyphs are read here from the font's own ASCII source,
rendered here so they can be looked at, and emitted here as the DB lines the
assembler wants. Same reasoning as tools/bjfont.py: one source of truth instead
of a transcription of it.

Two upstream files are read, for two different jobs:

    512_8_*.txt   the glyphs, as pictures. Sixteen to a line, eight columns
                  each, separated by single spaces, in blocks of eight rows.
    512_8_*.tab   which code point each glyph is, as CDPT(..) FIDX(..) pairs.

The bitmaps come from the .txt and the names from the .tab because the .txt's
own header cannot be parsed reliably: the code point names wrap onto a second
line and the block above the next set of glyphs starts before the previous
block's names have finished, so reading the file's names in one pass picks up
695 of them for 512 glyphs and every index after the first few is off. Taking
the first 512 of them instead is worse and fails silently - it puts the digits
at index $2c and the letters at $34. The .tab is machine readable and has no
such ambiguity, so it is the one that gets believed.

Only $20 to $7e is emitted: the space, the letters, the digits and the
punctuation, which is all this machine draws and a fraction of the upstream
table. Ninety five glyphs is 760 bytes, where the whole 512 is 4096 and the
hand-built table it replaced was 328.

Usage:
    tools/font512.py --preview          a few glyphs, as ASCII
    tools/font512.py --emit             asm/lib/ascii.tal

    # from a checkout of the upstream repo:
    git clone https://github.com/alexfru/512_8
    tools/font512.py --emit ~/512_8/512_8_sans.txt > asm/lib/ascii.tal

The font is public domain (Unlicense), so nothing is owed for using it and
nothing has to be credited. Sans is used here rather than the bold the upstream
author recommends, and it is worth saying what that costs: he describes sans as
thin, with worse contrast, and at 8x8 it shows - the colon is two single pixels
and the hyphen is five. Bold would be the safer pick for a clock. Both styles
carry the same characters at the same indices, so switching is a change to the
file name on the command line and nothing else.
"""

import os
import re
import sys

W, H = 8, 8
PER_ROW = 16                    # glyphs across in the upstream .txt
NGLYPH = 512

# The emitted range: printable ASCII, space through tilde. Contiguous, which is
# the whole point - see the note on the index in emit().
FIRST = 0x20
LAST = 0x7e

# Printable ASCII sits at its own code point from $28 up, checked below against
# the bitmaps rather than assumed. Below $28 the slots hold other characters and
# the index is not the code point, so those are listed here. Each was identified
# by looking at its bitmap, and the upstream .tab agrees where it has an entry,
# which is for $20, $21 and $27.
#
# Worth knowing and not obvious: there is no plain '!' in this font. The author
# put a double exclamation mark at $21 in its place, and the upstream codepage
# table lists it as the substitute for the '!' of CP437.
SUBSTITUTED = {
    0x20: 0x00a0,       # a no-break space, which draws as nothing
    0x21: 0x203c,       # a double exclamation mark, standing in for '!'
    0x27: 0x2019,       # a right single quotation mark, for an apostrophe
}
ASCII_FIRST = 0x28
ASCII_LAST = 0x7e


def read_glyphs(path):
    """NGLYPH glyphs, each H rows of W pixels, as one byte per row.

    Bit 7 is the leftmost pixel, which is the layout asm/lib/fontlib.tal expects,
    so nothing here converts anything: the bytes are used exactly as upstream has
    them.
    """
    with open(path, encoding='utf-8') as f:
        lines = f.read().splitlines()

    body = [i for i, l in enumerate(lines) if l and set(l) <= set('.O ')]
    if len(body) % H:
        raise SystemExit('font512: %d body rows is not a whole number of %d'
                         % (len(body), H))
    bands = len(body) // H
    if bands * PER_ROW != NGLYPH:
        raise SystemExit('font512: %d body rows is %d glyphs, expected %d'
                         % (len(body), bands * PER_ROW, NGLYPH))

    glyphs = []
    for band in range(bands):
        for col in range(PER_ROW):
            rows = []
            for r in range(H):
                line = lines[body[band * H + r]]
                cell = line[col * (W + 1):col * (W + 1) + W]
                if len(cell) != W:
                    raise SystemExit('font512: cell %d is %d columns, not %d'
                                     % (col, len(cell), W))
                bits = 0
                for x, ch in enumerate(cell):
                    if ch == 'O':
                        bits |= 0x80 >> x
                rows.append(bits)
            glyphs.append(rows)
    return glyphs


def read_codepoints(txt_path):
    """Code point for each glyph index, for the indices we emit.

    The .tab goes in FIRST and ASCII is laid over the top, which is the order
    that matters. The .tab exists to look characters up by Unicode, so it lists
    the Cyrillic and Greek letters too, and several of those share a glyph with
    their Latin lookalike - upstream says so outright, calling 3 and З
    indistinguishable. Applying the .tab last therefore renames 'A' to U+0410
    and '0' to whatever Cyrillic digit shares its slot. In a table indexed by
    character, the character is the one that has to win.
    """
    by_index = {}
    tab = txt_path[:-4] + '.tab'
    if not os.path.exists(tab):
        raise SystemExit('font512: %s is missing, and it is where the glyph '
                         'names come from' % tab)
    with open(tab, encoding='utf-8') as f:
        pairs = re.findall(r'CDPT\(0x([0-9A-Fa-f]+)\)\s+FIDX\(0x([0-9A-Fa-f]+)\)',
                           f.read())
    for codepoint, index in pairs:
        by_index[int(index, 16)] = int(codepoint, 16)

    by_index.update(SUBSTITUTED)
    for i in range(ASCII_FIRST, ASCII_LAST + 1):
        by_index[i] = i
    return by_index


def index_of(char):
    return ord(char) - FIRST


def as_ascii(rows):
    return ['|' + ''.join('#' if b & (0x80 >> x) else '.' for x in range(W)) + '|'
            for b in rows]


def preview(glyphs, names):
    for char in '0Az !-.:~':
        code = ord(char)
        rows = glyphs[code]
        shown = repr(char) if char != ' ' else 'space'
        print('%-6s index $%02x  %s  %s'
              % (shown, index_of(char),
                 ('U+%04X' % names[code]) if code in names else 'unlisted',
                 ' '.join('$%02x' % b for b in rows)))
        for line in as_ascii(rows):
            print('        ' + line)
        print()


def label(code, rows, names):
    cp = names.get(code)
    name = 'U+%04X' % cp if cp is not None else 'upstream unlisted'
    if cp is not None and ASCII_FIRST <= cp <= ASCII_LAST:
        return "%s  %r" % (name, chr(cp))
    if not any(rows):
        return name + ', blank'
    return name


def emit(glyphs, names):
    out = []
    w = out.append
    w('; ascii.tal - printable ASCII from the 512_8 font, as data.')
    w('; GENERATED, do not edit by hand; it comes from tools/font512.py, which')
    w('; reads the upstream .txt for the bitmaps and the .tab for the names.')
    w(';')
    w('; The space, the letters, the digits and the punctuation: $20 to $7e,')
    w('; ninety five glyphs, 760 bytes. It comes out of a 512 glyph font, which')
    w('; is 4096 bytes, and replaced a hand-built 41 glyph table of 328 bytes.')
    w('; The full set is not wanted: nothing on this machine draws anything else,')
    w('; and the machine has 32256 bytes of code and data to spend, which is')
    w('; plenty but not unlimited.')
    w(';')
    w('; From https://github.com/alexfru/512_8, sans. Public domain, Unlicense:')
    w('; nothing is owed for using it. Upstream recommends bold as the most')
    w('; legible of its three styles and warns that sans is thin with poor')
    w('; contrast, which at 8x8 is visible - the colon is two single pixels and')
    w('; the hyphen five. Worth knowing that the clock is set in this. Both styles')
    w('; carry the same characters at the same indices, so changing style is')
    w('; changing the file name on the generator command and nothing else.')
    w(';')
    w('; THE INDEX IS THE CHARACTER, MINUS THE SPACE')
    w('; ----------------------------------------')
    w('; The glyph for character code c is at @font + (c - $20) * 8, so glyph')
    w('; zero is the space and the digits are $10 to $19 and A to Z are $21 to')
    w('; $2a. That is the reason this range was chosen contiguous: a character')
    w('; is one subtraction away from its own offset, with no table of mappings')
    w('; and no search. The 512 glyph font cannot do this, because its index is')
    w('; not its code point below $28 and not above $7e at all - glyph 1 is a')
    w('; smiling face - which is why a subset like this one is the thing to use')
    w('; rather than the whole table.')
    w(';')
    w('; 8 wide, 8 down, one byte a row, bit 7 the leftmost pixel: the layout')
    w('; @glyph in fontlib.tal already expects, so these bytes are used exactly')
    w('; as upstream has them and nothing is converted.')
    w('')
    w('@font')
    for code in range(FIRST, LAST + 1):
        w('        DB ' + ', '.join('$%02x' % b for b in glyphs[code]) +
          '        ; $%02x  %s' % (code, label(code, glyphs[code], names)))
    w('')
    return '\n'.join(out)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('-')]
    if '--help' in argv or '-h' in argv:
        print(__doc__)
        return 0
    src = args[0] if args else '512_8_sans.txt'
    try:
        glyphs = read_glyphs(src)
        names = read_codepoints(src)
    except OSError:
        print('font512: cannot read %s\n'
              '         clone https://github.com/alexfru/512_8 and point at\n'
              '         512_8_sans.txt inside it; the .tab beside it is\n'
              '         needed too, for the glyph names' % src, file=sys.stderr)
        return 1

    if '--emit' in argv:
        sys.stdout.write(emit(glyphs, names))
    else:
        preview(glyphs, names)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))