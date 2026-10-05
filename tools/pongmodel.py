#!/usr/bin/env python3
"""Predict pong.rom's picture, pixel for pixel, so the count can be written
down before the ROM is run.

Written from the design and from the drawing primitives the ROM uses - the
7x8 seven-segment digit out of asm/clock.tal, the 2-bit-per-pixel sprite, and
the draw order in @draw - so a disagreement is the ROM's fault and the
disagreement is the finding.

    python3 tools/pongmodel.py              the count at -n 2, the verified one
    python3 tools/pongmodel.py --frames 200 counts for 200 frames
    python3 tools/pongmodel.py --ascii      frame 2, as a picture
"""
import sys

# ---------------------------------------------------------------- geometry

W, H = 320, 200
PADDLE_W, PADDLE_H = 3, 26
LEFT_X = 18                      # the player's paddle
RIGHT_X = 299                    # the computer's; 299..301, clear of the digits
BALL = 3                         # the ball is a 3x3 block
DASH_X = 159                     # the centre line
DASH_ON, DASH_OFF = 4, 4
DIGIT_Y = 8
DIGIT_XL, DIGIT_XR = 60, 232   # the tens; the units sit ten to the right
BALL_X0, BALL_Y0 = 158, 98
BALL_DX, BALL_DY = 3, 2
AI_SPEED = 3
AI_DEAD = 2                      # inside this many pixels the paddle holds still

WHITE, GREY153, GREY85 = 1, 2, 3
TOP_LIMIT = H - PADDLE_H          # 174: the lowest a paddle's top can be

# Which of the seven segments each digit lights, clockwise from the top bar.
# Copied from &segcodes in asm/clock.tal - the same house digit.
SEGCODES = [0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f]


def digit(x, y, n):
    """The 8-wide, 8-tall seven-segment digit from asm/clock.tal.

    Each horizontal segment is an 8x2 bar, each vertical a 2x4 bar with a
    second one two rows down, and they overlap where they cross - so the count
    is the size of the union, which is not six times the biggest piece.

    The 8 comes from the mask: a sprite byte holds four pixels at two bits each,
    so a bar eight across is two bytes of $ff.  asm/clock.tal uses $ff, $c0,
    which is six across, because its right-hand vertical sits at x=4 and the bar
    only has to reach it.  Both are legitimate; the number has to come from
    whichever mask the ROM actually uses, so it is written down here.
    """
    code = SEGCODES[n]
    px = set()
    # (x offset, y offset, horizontal?)
    SEG = [(0, 0, True), (4, 0, False), (4, 3, False), (0, 6, True),
           (0, 3, False), (0, 0, False), (0, 3, True)]
    for i, (sx, sy, horiz) in enumerate(SEG):
        if not (code >> i) & 1:
            continue
        if horiz:
            for dx in range(8):
                for dy in range(2):
                    px.add((x + sx + dx, y + sy + dy))
        else:
            for dx in range(2):
                for dy in range(4):
                    px.add((x + sx + dx, y + sy + dy))
    return px


# ---------------------------------------------------------------- the game

class Pong:
    def __init__(self, mouse_y=0):
        self.by = PADDLE_H // 2           # player's paddle top
        self.ai = PADDLE_H // 2          # computer's paddle top
        self.bx, self.byy = BALL_X0, BALL_Y0
        self.dx, self.dy = BALL_DX, BALL_DY
        self.score_l = self.score_r = 0
        self.serve_to_right = True        # the first serve goes right
        self.mouse_y = mouse_y

    def paddle(self, top, x):
        """A paddle's pixels: three columns, twenty-six rows."""
        return {(x + dx, top + dy)
                for dx in range(PADDLE_W) for dy in range(PADDLE_H)
                if 0 <= top + dy < H}

    def step(self):
        """One frame: the paddles move, then the ball does."""
        # the player's paddle follows the mouse, its middle under the pointer
        want = self.mouse_y - PADDLE_H // 2
        if want < 0:
            want = 0
        elif want > TOP_LIMIT:
            want = TOP_LIMIT
        self.by = want

        # The computer's paddle chases the ball's middle, but never faster than
        # AI_SPEED and never inside its own dead zone. What it chases *while* is
        # the ball heading its way, not the ball being on its half of the screen:
        # testing the half gives it one frame of movement per rally and then it
        # parks, so the ball walks past every time and the score runs away.
        target = self.dx > 0
        mid = self.byy + BALL // 2
        aim = self.ai + PADDLE_H // 2
        if target and abs(mid - aim) >= AI_DEAD:
            d = mid - aim
            step = AI_SPEED if d > 0 else -AI_SPEED
            self.ai += step
            if self.ai < 0:
                self.ai = 0
            elif self.ai > TOP_LIMIT:
                self.ai = TOP_LIMIT

        # the ball
        self.bx += self.dx
        self.byy += self.dy
        if self.byy < 0:
            self.byy = 0
            self.dy = -self.dy
        elif self.byy > H - BALL:
            self.byy = H - BALL
            self.dy = -self.dy

        # a paddle: the ball has to be level with one and going into it
        ball_lo, ball_hi = self.byy, self.byy + BALL - 1
        if self.dx < 0 and self.bx <= LEFT_X + PADDLE_W and self.bx + BALL > LEFT_X:
            if self.by <= ball_hi and self.by + PADDLE_H - 1 >= ball_lo:
                self.bx = LEFT_X + PADDLE_W
                self.dx = -self.dx
                self.dy = self.english(self.by)
        elif self.dx > 0 and self.bx + BALL >= RIGHT_X and self.bx < RIGHT_X + PADDLE_W:
            if self.ai <= ball_hi and self.ai + PADDLE_H - 1 >= ball_lo:
                self.bx = RIGHT_X - BALL
                self.dx = -self.dx
                self.dy = self.english(self.ai)

        # A miss: the ball is past a paddle entirely.  The score stops at
        # ninety-nine, because the tens digit is a single @digit and a value of
        # ten would index &segcodes off the end of it.  It used to stop at nine
        # for the same reason with one digit, and the ROM had no such guard at
        # all, so a tenth point read the first byte of the bar mask and came out
        # on screen as an eight.
        if self.bx + BALL < LEFT_X:
            if self.score_r < 99:
                self.score_r += 1
            self.serve(False)
        elif self.bx > RIGHT_X + PADDLE_W - 1:
            if self.score_l < 99:
                self.score_l += 1
            self.serve(True)

    def english(self, top):
        """Where the ball goes after hitting a paddle: off-centre misses travel
        steeper, and the further out the steeper, which is the whole reason a
        player aims at the edge of the paddle rather than the middle."""
        off = (self.byy + BALL // 2) - (top + PADDLE_H // 2)
        if off < 0:
            return -3 if off < -5 else -2
        if off > 0:
            return 3 if off > 5 else 2
        return self.dy

    def serve(self, to_right):
        self.bx, self.byy = BALL_X0, BALL_Y0
        self.dx = BALL_DX if to_right else -BALL_DX
        self.dy = BALL_DY
        self.serve_to_right = to_right

    # ---------------------------------------------------------------- draw

    def picture(self):
        """The frame, in the order @draw puts it down."""
        px = {}
        # the centre line, dashed
        y = 0
        while y < H:
            if (y // (DASH_ON + DASH_OFF)) % 2 == 0:
                px[(DASH_X, y)] = GREY153
            y += 1
        # the score, two digits a side: the tens ten to the left of the units,
        # which is where asm/pong.tal puts them
        for p in digit(DIGIT_XL, DIGIT_Y, self.score_l // 10):
            px[p] = GREY153
        for p in digit(DIGIT_XL + 10, DIGIT_Y, self.score_l % 10):
            px[p] = GREY153
        for p in digit(DIGIT_XR, DIGIT_Y, self.score_r // 10):
            px[p] = GREY153
        for p in digit(DIGIT_XR + 10, DIGIT_Y, self.score_r % 10):
            px[p] = GREY153
        # the paddles
        for p in self.paddle(self.by, LEFT_X):
            px[p] = WHITE
        for p in self.paddle(self.ai, RIGHT_X):
            px[p] = GREY85
        # the ball, last, so it lies on top of everything
        for dx in range(BALL):
            for dy in range(BALL):
                px[(self.bx + dx, self.byy + dy)] = WHITE
        return px


def frames(n, mouse_y=0, path=None):
    """A snapshot of every frame from 1 to n.

    A *copy of the numbers*, not the object: returning the state itself gives a
    list of n references to one object, so every entry reads as the last frame
    and the whole sweep quietly compares one picture with itself.

    `path` is dux's -m list, one point per frame, resting on the last.  Two
    offsets, both measured:

      - The reset vector's @draw runs before any point is fed, so frame 1 always
        sees a mouse at zero however the path starts.
      - duxemu advances the path index *before* it reads the point, so its first
        host_frame - which is game frame 2 - already sees path[1] and not
        path[0].  Counting frames from 1, then, game frame k reads path[k-1]:
        frame 2 reads path[1], frame 3 reads path[2].  Getting this one point out
        puts the paddle thirteen pixels out for the whole sweep, and thirteen is
        exactly half a paddle, so it looks like a game that is merely stiff.
    """
    g = Pong(0)
    out = []
    for k in range(n):
        if path:
            g.mouse_y = path[min(k, len(path) - 1)]
        g.step()
        out.append((g.bx, g.byy, g.dx, g.dy, g.by, g.ai, g.score_l, g.score_r))
    return out


def picture_of(snap):
    """The picture for one snapshot tuple."""
    g = Pong()
    (g.bx, g.byy, g.dx, g.dy, g.by, g.ai, g.score_l, g.score_r) = snap
    return g.picture()


def split(px):
    d = {}
    for c in px.values():
        d[c] = d.get(c, 0) + 1
    return d


def main():
    args = sys.argv[1:]
    if '--frames' in args:
        n = int(args[args.index('--frames') + 1])
        seen = []
        for snap in frames(n):
            px = picture_of(snap)
            seen.append((len(px), split(px), snap[6], snap[7]))
        counts = sorted({c for c, _, _, _ in seen})
        splits = sorted({tuple(sorted(s.items())) for _, s, _, _ in seen})
        print('%d frames; %d distinct counts %s, %d distinct colour splits'
              % (n, len(counts), counts, len(splits)))
        print('final score %d-%d' % (seen[-1][2], seen[-1][3]))
        prev = None
        for i, (c, sp, sl, sr) in enumerate(seen, 1):
            row = (c, tuple(sorted(sp.items())), sl, sr)
            if row != prev:
                print('  frame %3d: %3d lit  %s  score %d-%d'
                      % (i, c, ' '.join('c%d=%d' % kv for kv in sorted(sp.items())), sl, sr))
                prev = row
        return
    # verify.sh renders with -n 2, and -n N runs @draw N+1 times: once from the
    # reset vector and then N frames.  So -n 2 is game frame 3, and the prediction
    # in tools/expect.txt has to be that frame's number, not frame 2's.
    #
    # The count is 461 on either, because at this point the ball is in open space
    # and overlaps nothing; it stops being constant the moment the ball crosses
    # the centre line or a paddle, which is why this is worth saying rather than
    # assuming.  Two score digits a side rather than one, hence 461 and not 365.
    snap = frames(3)[-1]
    px = picture_of(snap)
    d = split(px)
    print('%d lit of %d' % (len(px), W * H))
    print('  by colour: ' + '  '.join('c%d=%d' % (k, d[k]) for k in sorted(d)))
    print('  ball at %d,%d  dx=%d dy=%d  score %d-%d'
          % (snap[0], snap[1], snap[2], snap[3], snap[6], snap[7]))
    print('  player paddle top %d, computer %d' % (snap[4], snap[5]))
    if '--ascii' in args:
        ch = {WHITE: 'W', GREY153: 'm', GREY85: 's'}
        xs = [p[0] for p in px]
        ys = [p[1] for p in px]
        for yy in range(0, H, 2):
            print('%3d %s' % (yy, ''.join(ch.get(px.get((x, yy), 0), '.')
                                     for x in range(0, W, 1)) if '--wide' in args
                  else ''.join(ch.get(px.get((x, yy), 0), '.')
                               for x in range(min(xs), max(xs) + 1))))


main()
