"""A cell canvas with half-block pixels, plus a legible 5x7 display face.

Terminal cells are about twice as tall as wide, so each cell holds two
stacked pixels (▀ ▄ █). At scale 2 a letter is 10 columns by 7 rows with
strokes two columns thick: proper letterforms rather than 3x5 blobs.
"""
import unicodedata

GLYPHS = {
    'A': ('.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'),
    'B': ('####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'),
    'C': ('.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'),
    'D': ('####.', '#...#', '#...#', '#...#', '#...#', '#...#', '####.'),
    'E': ('#####', '#....', '#....', '####.', '#....', '#....', '#####'),
    'F': ('#####', '#....', '#....', '####.', '#....', '#....', '#....'),
    'G': ('.###.', '#...#', '#....', '#.###', '#...#', '#...#', '.###.'),
    'H': ('#...#', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'),
    'I': ('###', '.#.', '.#.', '.#.', '.#.', '.#.', '###'),
    'J': ('..###', '...#.', '...#.', '...#.', '...#.', '#..#.', '.##..'),
    'K': ('#...#', '#..#.', '#.#..', '##...', '#.#..', '#..#.', '#...#'),
    'L': ('#....', '#....', '#....', '#....', '#....', '#....', '#####'),
    'M': ('#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'),
    'N': ('#...#', '##..#', '#.#.#', '#.#.#', '#..##', '#...#', '#...#'),
    'O': ('.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'),
    'P': ('####.', '#...#', '#...#', '####.', '#....', '#....', '#....'),
    'Q': ('.###.', '#...#', '#...#', '#...#', '#.#.#', '#..#.', '.##.#'),
    'R': ('####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'),
    'S': ('.####', '#....', '#....', '.###.', '....#', '....#', '####.'),
    'T': ('#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'),
    'U': ('#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'),
    'V': ('#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'),
    'W': ('#...#', '#...#', '#...#', '#.#.#', '#.#.#', '##.##', '#...#'),
    'X': ('#...#', '#...#', '.#.#.', '..#..', '.#.#.', '#...#', '#...#'),
    'Y': ('#...#', '#...#', '.#.#.', '..#..', '..#..', '..#..', '..#..'),
    'Z': ('#####', '....#', '...#.', '..#..', '.#...', '#....', '#####'),
    '0': ('.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'),
    '1': ('..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'),
    '2': ('.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'),
    '3': ('.###.', '#...#', '....#', '..##.', '....#', '#...#', '.###.'),
    '4': ('...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'),
    '5': ('#####', '#....', '####.', '....#', '....#', '#...#', '.###.'),
    '6': ('.###.', '#....', '#....', '####.', '#...#', '#...#', '.###.'),
    '7': ('#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'),
    '8': ('.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'),
    '9': ('.###.', '#...#', '#...#', '.####', '....#', '....#', '.###.'),
    ':': ('...', '...', '.#.', '...', '.#.', '...', '...'),
    '-': ('....', '....', '....', '####', '....', '....', '....'),
    '.': ('.', '.', '.', '.', '.', '.', '#'),
    "'": ('#', '#', '.', '.', '.', '.', '.'),
    '°': ('.##.', '#..#', '#..#', '.##.', '....', '....', '....'),
    ' ': ('...', '...', '...', '...', '...', '...', '...'),
}
EIGHTHS = ' ▁▂▃▄▅▆▇█'


def displayable(text):
    """Upper-case ASCII form of text if the display face can draw it, else None."""
    word = unicodedata.normalize('NFKD', text.upper()).encode('ascii', 'ignore').decode()
    word = ' '.join(word.replace(',', ' ').split())
    return word if word and all(ch in GLYPHS for ch in word) else None


def big_width(word, scale):
    return (sum(len(GLYPHS[ch][0]) for ch in word) + len(word) - 1) * scale


def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(stops, t):
    """Colour at t in [0, 1] along evenly spaced stops."""
    t = max(0.0, min(1.0, t)) * (len(stops) - 1)
    index = min(int(t), len(stops) - 2)
    return lerp(stops[index], stops[index + 1], t - index)


def rgb(value):
    return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))


class Canvas:
    """Characters over a layer of half-cell pixels, rendered to SGR rows."""

    def __init__(self, width, height):
        self.width, self.height = width, height
        self.pixels = [[None] * width for _ in range(height * 2)]
        self.cells = [[None] * width for _ in range(height)]

    def pixel(self, x, y, color):
        if 0 <= x < self.width and 0 <= y < self.height * 2:
            self.pixels[y][x] = color

    def text(self, x, y, string, color, bold=False):
        if not 0 <= y < self.height:
            return
        for offset, char in enumerate(string):
            column = x + offset
            if 0 <= column < self.width:
                if unicodedata.east_asian_width(char) in 'WF' or unicodedata.category(char)[0] in 'CM':
                    char = '?'
                self.cells[y][column] = (char, color, bold)

    def center(self, y, string, color, bold=False):
        self.text((self.width - len(string)) // 2, y, string, color, bold)

    def empty(self, x, y):
        return (0 <= x < self.width and 0 <= y < self.height and self.cells[y][x] is None
                and self.pixels[y * 2][x] is None and self.pixels[y * 2 + 1][x] is None)

    def big(self, x, y, word, scale, color):
        """Draw word with its top-left at cell (x, y). color(fraction) -> rgb."""
        total = max(1, big_width(word, scale))
        cursor = x
        for char in word:
            glyph = GLYPHS[char]
            for row, bits in enumerate(glyph):
                for col, bit in enumerate(bits):
                    if bit != '#':
                        continue
                    for dx in range(scale):
                        column = cursor + col * scale + dx
                        shade = color((column - x) / total)
                        for dy in range(scale):
                            self.pixel(column, y * 2 + row * scale + dy, shade)
            cursor += (len(glyph[0]) + 1) * scale

    def rows(self):
        """One string per row. Colours are 24-bit; every row resets at its end."""
        out = []
        for y in range(self.height):
            parts, state = [], None
            top_row, bottom_row, cells = self.pixels[y * 2], self.pixels[y * 2 + 1], self.cells[y]
            for x in range(self.width):
                cell, top, bottom = cells[x], top_row[x], bottom_row[x]
                if cell:
                    char, fg, bg, bold = cell[0], cell[1], None, cell[2]
                elif top and bottom:
                    char, fg, bg, bold = ('█', top, None, False) if top == bottom else ('▀', top, bottom, False)
                elif top:
                    char, fg, bg, bold = '▀', top, None, False
                elif bottom:
                    char, fg, bg, bold = '▄', bottom, None, False
                else:
                    char, fg, bg, bold = ' ', None, None, False
                if char == ' ' and bg is None:
                    fg, bold = None, False
                style = (fg, bg, bold)
                if style != state:
                    codes = ['0']
                    if bold:
                        codes.append('1')
                    if fg:
                        codes.append('38;2;%d;%d;%d' % fg)
                    if bg:
                        codes.append('48;2;%d;%d;%d' % bg)
                    parts.append('\033[' + ';'.join(codes) + 'm')
                    state = style
                parts.append(char)
            parts.append('\033[0m')
            out.append(''.join(parts))
        return out
