// Mumoria - animations: spells, missiles and blows, drawn over the map of
// everyone on the level who can see them. See docs/MUMORIA_DESIGN.md 2.18.
//
// The engine settles everything at once, under the world lock. These calls
// only queue timed cells (vterm.h MuFxCell); each connection's I/O thread
// plays them, so nobody waits for an animation.

#include "session.h"

#include <cmath>

namespace {

struct Palette {
    int dark, mid, bright;
    char glyph;
};

// Indexed by MuFxElement. xterm 256 colours.
const Palette PALETTE[] = {
    {127, 201, 213, '*'}, // magic missile
    {178, 226, 231, '*'}, // lightning (drawn along the bolt)
    {64, 106, 148, '*'},  // poison gas
    {28, 46, 154, '*'},   // acid
    {31, 51, 195, '*'},   // frost
    {160, 202, 226, '*'}, // fire
    {136, 229, 231, 'o'}, // holy orb
    {97, 141, 183, '*'},  // mind: sleep, confusion, slow, teleport other
    {52, 88, 160, '*'},   // drain life
    {94, 130, 180, '*'},  // stone to mud, building walls
    {67, 117, 195, '*'},  // light
    {25, 33, 81, '*'},    // arcane
    {28, 46, 120, '+'},   // healing
    {238, 244, 250, '~'}, // a spell that fizzles
    {88, 160, 196, '*'},  // blows and missiles
    {232, 236, 240, '*'}, // darkness: blindness
};

const int STEP_MS = 25;      // a bolt, per square
const int THROW_MS = 30;     // a thrown thing, per square
const int FRAME_MS = 80;     // a burst's frame
const int BLOW_MS = 50;      // a swing's frame
const int BACKLOG_MS = 1500; // never queued further ahead than this
const int CHAT_POP_MS = 90;  // a chat bubble's first dot
const int CHAT_MS = 1500;    // then the bubble

const Coord_t RING[8] = {{-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}};

thread_local int64_t cursor = 0; // when this player's animations are free again

// Damage numbers (muFxDamage): the engine takes the damage just before it
// plays the blow or the bolt, so a number waits for that animation and shows
// at its moment of impact; any left are shown when the turn ends.
struct Number {
    Coord_t at;
    int amount;
    bool taken; // red over your @; else green over what you hit
};
thread_local std::vector<Number> numbers;
const int NUMBER_MS = 450; // each of its two heights
thread_local int together = 0;
thread_local int64_t together_start = -1, together_end = 0;
thread_local uint32_t seed = 2463534242u;

// Not the engine's RNG: an animation must not change the game's rolls.
int coin() {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return (int) (seed & 1);
}

uint32_t fg(int n) {
    return MU_ATTR_BOLD | MU_ATTR_FG(n);
}

uint32_t on(int bg, int fg_colour = 231) {
    return MU_ATTR_BOLD | MU_ATTR_FG(fg_colour) | MU_ATTR_BG(bg);
}

Palette const &pal(int element) {
    return PALETTE[element >= 0 && element <= MU_FX_DARK ? element : MU_FX_ARCANE];
}

int sign(int v) {
    return (v > 0) - (v < 0);
}

bool same(Coord_t a, Coord_t b) {
    return a.y == b.y && a.x == b.x;
}

Coord_t step(Coord_t c, Coord_t d) {
    return Coord_t{c.y + d.y, c.x + d.x};
}

// On the level, its outer wall included (Umoria's coordInBounds() leaves that out).
bool on_level(Coord_t c) {
    return c.y >= 0 && c.x >= 0 && c.y < dg.height && c.x < dg.width;
}

bool open(Coord_t c) {
    return on_level(c) && dg.floor[c.y][c.x].feature_id <= MAX_OPEN_SPACE;
}

// Open, with nobody on it: sparks never hide a monster or a player.
bool empty(Coord_t c) {
    return open(c) && dg.floor[c.y][c.x].creature_id == 0 && !muOtherPlayerAt(c);
}

// Umoria's keypad directions.
Coord_t dir_vec(int dir) {
    return Coord_t{dir <= 3 ? 1 : dir >= 7 ? -1 : 0, dir % 3 == 1 ? -1 : dir % 3 == 0 ? 1 : 0};
}

// A blade or an arrow pointing along d.
char radial(Coord_t d) {
    if (d.x == 0) {
        return '|';
    }
    if (d.y == 0) {
        return '-';
    }
    return d.y == d.x ? '\\' : '/';
}

class Anim {
  public:
    // queued false: starts now and holds up none of this player's later animations
    explicit Anim(bool queued = true) : queued_(queued) {
        int64_t now = mu_vt_now_ms();
        start_ = cursor > now && queued ? cursor : now;
        if (start_ > now + BACKLOG_MS) {
            start_ = now + BACKLOG_MS;
        }
        if (together > 0 && queued) {
            if (together_start < 0) {
                together_start = start_;
            }
            start_ = together_start;
        }
    }

    // A sound at ms `at`, from square c (vterm.h MuSound).
    void sound(int at, Coord_t c, const char *name) {
        sounds_.push_back(Sound{at, c, name});
    }

    // When the blow lands or the bolt bursts: waiting damage numbers show then.
    void impact(int at) {
        impact_ = at;
    }

    // The waiting damage numbers, from this animation's impact (its start if
    // none): over the square, then a row higher, fading. Only the player whose
    // fight it is sees them (the one loaded: attacker or victim).
    void show_numbers() {
        MuSession *viewer = mu_player_loaded();
        if (viewer != nullptr) {
            std::vector<MuFxCell> cells;
            int64_t t0 = start_ + (impact_ >= 0 ? impact_ : 0);
            for (Number const &n : numbers) {
                std::string digits = std::to_string(n.amount);
                int x0 = n.at.x - ((int) digits.size() - 1) / 2;
                for (int rise = 0; rise < 2; rise++) {
                    uint32_t look = rise == 0 ? fg(n.taken ? 196 : 46) : MU_ATTR_FG(n.taken ? 160 : 34);
                    int64_t from = t0 + rise * NUMBER_MS;
                    for (size_t i = 0; i < digits.size(); i++) {
                        cells.push_back(MuFxCell{from, from + NUMBER_MS, (int16_t) (n.at.y - 1 - rise), (int16_t) (x0 + (int) i), digits[i], look});
                    }
                }
            }
            mu_vt_fx_add(mu_screen_of(viewer), cells.data(), (int) cells.size());
        }
        numbers.clear();
    }

    void put(int at, int ms, Coord_t c, char ch, uint32_t attr) {
        if (!on_level(c) || ms <= 0) {
            return;
        }
        cells_.push_back(MuFxCell{start_ + at, start_ + at + ms, (int16_t) c.y, (int16_t) c.x, ch, attr});
        if (at + ms > length_) {
            length_ = at + ms;
        }
    }

    // To every player on the level who can see each square, and the sounds
    // to everyone near enough to hear them (lock held).
    void play() {
        MuSession *self = mu_self;
        if (self == nullptr || self->level == nullptr) {
            numbers.clear();
            return;
        }
        if (!numbers.empty()) {
            show_numbers();
        }
        if (cells_.empty() && sounds_.empty()) {
            return;
        }
        int64_t end = start_ + length_;
        if (!queued_) {
            // its own time: the cursor stays
        } else if (together > 0) {
            together_end = end > together_end ? end : together_end;
        } else {
            cursor = end;
        }
        std::vector<MuFxCell> seen;
        for (MuSession *p : self->level->players) {
            bool me = p == self;
            if (!me && p->in_building) {
                continue;
            }
            Coord_t eye = me ? py.pos : p->ctx.py.pos;
            for (Sound const &k : sounds_) {
                int volume = loudness(me, eye, k.at_square);
                if (volume > 0) {
                    mu_vt_sound(mu_screen_of(p), start_ + k.at, k.name, volume);
                }
            }
            if ((me ? py.flags.blind : p->ctx.py.flags.blind) > 0) {
                continue; // the blind still hear
            }
            seen.clear();
            for (MuFxCell const &c : cells_) {
                Coord_t at{c.y, c.x};
                if (me || (coordDistanceBetween(eye, at) <= config::monsters::MON_MAX_SIGHT && los(eye, at))) {
                    seen.push_back(c);
                }
            }
            mu_vt_fx_add(mu_screen_of(p), seen.data(), (int) seen.size());
        }
    }

  private:
    struct Sound {
        int at;
        Coord_t at_square;
        const char *name;
    };

    // Full beside you, fainter with distance, muffled by walls; out of
    // hearing past MON_MAX_SIGHT. Your own doings are never faint.
    static int loudness(bool me, Coord_t ear, Coord_t from) {
        int d = coordDistanceBetween(ear, from);
        if (!me && d > config::monsters::MON_MAX_SIGHT) {
            return 0;
        }
        int volume = 100 - 4 * d;
        if (!los(ear, from)) {
            volume /= 2;
        }
        if (me) {
            return volume < 60 ? 60 : volume;
        }
        return volume < 10 ? 0 : volume;
    }

    std::vector<Sound> sounds_;
    std::vector<MuFxCell> cells_;
    bool queued_;
    int64_t start_;
    int length_ = 0;
    int impact_ = -1;
};

// The four squares beside c (diagonal: the four corners). ch 0: pointing outwards.
void around(Anim &a, int at, int ms, Coord_t c, bool diagonal, char ch, uint32_t attr) {
    for (int i = diagonal ? 1 : 0; i < 8; i += 2) {
        Coord_t n = step(c, RING[i]);
        if (empty(n)) {
            a.put(at, ms, n, ch != 0 ? ch : radial(RING[i]), attr);
        }
    }
}

// A free square beside c, across the line d came in on.
Coord_t beside(Coord_t c, Coord_t d) {
    Coord_t side[2] = {{d.x, -d.y}, {-d.x, d.y}};
    int first = coin();
    for (int i = 0; i < 2; i++) {
        Coord_t n = step(c, side[(first + i) & 1]);
        if (empty(n)) {
            return n;
        }
    }
    Coord_t past = step(c, d);
    return empty(past) ? past : c;
}

// A death: a red splash.
void splat(Anim &a, int at, Coord_t c) {
    a.put(at, FRAME_MS, c, '*', on(124));
    around(a, at, FRAME_MS, c, false, '*', fg(160));
    around(a, at, FRAME_MS, c, true, '*', fg(160));
    a.put(at + FRAME_MS, FRAME_MS, c, MU_FX_TINT, on(52));
    around(a, at + FRAME_MS, FRAME_MS, c, false, '.', fg(88));
    around(a, at + FRAME_MS, FRAME_MS, c, true, '.', fg(88));
}

// A hit: the square flashes, sparks fly out.
void burst(Anim &a, int at, Coord_t c, Palette const &p, bool kill) {
    a.put(at, FRAME_MS, c, MU_FX_TINT, on(p.bright, 16));
    around(a, at, FRAME_MS, c, false, '*', fg(p.mid));
    a.put(at + FRAME_MS, FRAME_MS, c, MU_FX_TINT, on(p.mid));
    around(a, at + FRAME_MS, FRAME_MS, c, true, 0, fg(p.bright));
    a.put(at + 2 * FRAME_MS, FRAME_MS, c, MU_FX_TINT, on(p.dark));
    around(a, at + 2 * FRAME_MS, FRAME_MS, c, true, '.', fg(p.dark));
    if (kill) {
        splat(a, at + 3 * FRAME_MS, c);
    }
}

// A spell that took hold without harm.
void sparkle(Anim &a, int at, Coord_t c, Palette const &p) {
    a.put(at, FRAME_MS, c, MU_FX_TINT, on(p.bright, 16));
    around(a, at, FRAME_MS, c, true, '*', fg(p.bright));
    a.put(at + FRAME_MS, FRAME_MS, c, MU_FX_TINT, on(p.mid));
    around(a, at + FRAME_MS, FRAME_MS, c, false, '.', fg(p.mid));
}

// Turned aside (touched: it reached the target, which shrugged it off), or
// a miss: it goes by beside the target.
void glance(Anim &a, int at, Coord_t c, Coord_t d, char head, Palette const &p, bool touched) {
    if (touched) {
        a.put(at, FRAME_MS, c, MU_FX_TINT, on(244, 16));
        at += FRAME_MS / 2;
    }
    Coord_t b = beside(c, d);
    a.put(at, FRAME_MS, b, head, fg(p.bright));
    a.put(at + FRAME_MS, FRAME_MS, b, '.', fg(p.mid));
}

// Radius 2 around center, as spellFireBall() and spellBreath() reach.
void blast(Anim &a, int at, Coord_t center, Palette const &p) {
    for (int y = center.y - 2; y <= center.y + 2; y++) {
        for (int x = center.x - 2; x <= center.x + 2; x++) {
            Coord_t s{y, x};
            if (!open(s)) {
                continue;
            }
            int r = coordDistanceBetween(center, s);
            if (r > 2 || !los(center, s)) {
                continue;
            }
            int t = at + r * 60;
            bool someone = !empty(s);
            a.put(t, 100, s, someone ? MU_FX_TINT : '*', someone ? on(p.bright, 16) : fg(p.bright));
            a.put(t + 100, 80, s, someone ? MU_FX_TINT : '*', someone ? on(p.mid) : fg(p.mid));
            a.put(t + 180, 80, s, someone ? MU_FX_TINT : '.', someone ? on(p.dark) : fg(p.dark));
        }
    }
}

// The flight from `from` along direction, up to (not on) `end`. Returns its length in ms.
int flight(Anim &a, Coord_t from, int direction, Coord_t end, Palette const &p, char head, int element) {
    bool lightning = element == MU_FX_LIGHTNING, thrown = element == MU_FX_PHYSICAL;
    int ms = thrown ? THROW_MS : STEP_MS;
    int t = 0;
    Coord_t c = from;
    for (int n = 0; n < 64; n++) {
        if (!playerMovePosition(direction, c) || same(c, end) || !open(c)) {
            break;
        }
        a.put(t, ms, c, head, fg(p.bright));
        if (lightning) {
            a.put(t + ms, 4 * ms, c, head, fg(p.mid)); // a bolt of lightning lingers
        } else if (!thrown) {
            a.put(t + ms, ms, c, '.', fg(p.mid));
        }
        t += ms;
    }
    return t;
}

enum class Weapon { Blade, Heavy, Blunt, Fist };

Weapon weapon_kind(int category) {
    switch (category) {
        case TV_NOTHING:
            return Weapon::Fist;
        case TV_POLEARM:
            return Weapon::Heavy;
        case TV_HAFTED:
        case TV_DIGGING:
        case TV_BOW:
            return Weapon::Blunt;
        default:
            return Weapon::Blade;
    }
}

} // namespace

void muFxCast(Coord_t at, int element) {
    Palette const &p = pal(element);
    Anim a;
    a.sound(0, at, element == MU_FX_FIZZLE ? "fizzle" : "cast");
    if (element == MU_FX_FIZZLE) {
        a.put(0, FRAME_MS, at, MU_FX_TINT, on(p.mid, 232));
        Coord_t puff = step(at, RING[coin() ? 1 : 7]); // smoke rises
        if (empty(puff)) {
            a.put(FRAME_MS / 2, FRAME_MS, puff, '~', fg(p.bright));
            a.put(FRAME_MS * 3 / 2, FRAME_MS, puff, '.', fg(p.dark));
        }
        a.play();
        return;
    }
    a.put(0, FRAME_MS, at, MU_FX_TINT, on(p.mid));
    around(a, 0, FRAME_MS, at, false, '+', fg(p.bright));
    a.put(FRAME_MS, FRAME_MS, at, MU_FX_TINT, on(p.bright, 16));
    around(a, FRAME_MS, FRAME_MS, at, true, 'x', fg(p.bright));
    around(a, FRAME_MS, FRAME_MS, at, false, '.', fg(p.mid));
    a.put(2 * FRAME_MS, FRAME_MS, at, MU_FX_TINT, on(p.dark));
    around(a, 2 * FRAME_MS, FRAME_MS, at, true, '.', fg(p.dark));
    a.play();
}

void muFxHeal(Coord_t at) {
    Palette const &p = pal(MU_FX_HEAL);
    Anim a;
    a.sound(0, at, "heal");
    a.put(0, 2 * FRAME_MS, at, MU_FX_TINT, on(p.dark));
    around(a, 0, FRAME_MS, at, false, '+', fg(p.bright));
    around(a, FRAME_MS, FRAME_MS, at, true, '+', fg(p.mid));
    around(a, 2 * FRAME_MS, FRAME_MS, at, false, '.', fg(p.dark));
    a.play();
}

// A rare item: its square pulses in the tier's colour under a column of light
// (taller the rarer), and a Legendary one throws sparks all around.
void muFxLoot(Coord_t at, int tier) {
    static const int COLOUR[] = {250, 40, 33, 135, 208}; // loot.cpp's TIERS
    int colour = COLOUR[tier >= 0 && tier <= 4 ? tier : 0];
    int height = tier >= 4 ? 4 : tier >= 3 ? 3 : 2;
    Anim a;
    if (tier >= 2) {
        a.sound(0, at, "loot");
    }
    for (int f = 0; f < 6; f++) {
        a.put(f * FRAME_MS, FRAME_MS, at, MU_FX_TINT, f % 2 == 0 ? on(colour, 231) : on(colour, 16));
        for (int k = 1; k <= height; k++) {
            Coord_t c{at.y - k, at.x};
            if (!empty(c)) {
                break;
            }
            if (f >= k - 1) {
                a.put(f * FRAME_MS, FRAME_MS, c, k == height ? '*' : '|', fg(colour));
            }
        }
    }
    if (tier >= 4) {
        around(a, 2 * FRAME_MS, 2 * FRAME_MS, at, true, '*', fg(colour));
        around(a, 4 * FRAME_MS, 2 * FRAME_MS, at, false, '+', fg(colour));
    }
    a.play();
}

void muFxRay(Coord_t from, int direction, Coord_t end, int element, int outcome, char glyph) {
    Palette const &p = pal(element);
    Coord_t d = dir_vec(direction);
    char head = glyph != 0 ? glyph : element == MU_FX_LIGHTNING || element == MU_FX_PHYSICAL ? radial(d) : p.glyph;
    Anim a;
    int t = flight(a, from, direction, end, p, head, element);
    a.impact(t);
    switch (outcome) {
        case MU_FX_HIT:
        case MU_FX_KILL: {
            bool kill = outcome == MU_FX_KILL || (on_level(end) && dg.floor[end.y][end.x].creature_id == 0);
            burst(a, t, end, p, kill);
            a.sound(t, end, "hit");
            if (kill) {
                a.sound(t + 3 * FRAME_MS, end, "kill");
            }
            break;
        }
        case MU_FX_AFFECT:
            sparkle(a, t, end, p);
            break;
        case MU_FX_RESIST:
            glance(a, t, end, d, head, p, true);
            a.sound(t, end, "miss");
            break;
        case MU_FX_MISS:
            glance(a, t, end, d, head, p, false);
            a.sound(t, end, "miss");
            break;
        case MU_FX_WALL:
            if (on_level(end) && !open(end)) {
                a.put(t, FRAME_MS, end, '*', fg(p.bright)); // it splashes on the wall
                a.put(t + FRAME_MS, FRAME_MS, end, '*', fg(p.dark));
            }
            break;
        default:
            break;
    }
    a.play();
}

void muFxBall(Coord_t from, int direction, Coord_t center, int element) {
    Palette const &p = pal(element);
    char head = element == MU_FX_LIGHTNING ? radial(dir_vec(direction)) : p.glyph;
    Anim a;
    int t = same(from, center) ? 0 : flight(a, from, direction, center, p, head, element);
    blast(a, t, center, p);
    a.sound(t, center, "blast");
    a.impact(t);
    a.play();
}

// A straight line from `from` to `to` (neither end drawn), for monsters, who
// aim at a square rather than along a direction. Returns its length in ms.
static int aim(Anim &a, Coord_t from, Coord_t to, Palette const &p, int ms) {
    int dy = to.y - from.y, dx = to.x - from.x;
    int n = std::max(std::abs(dy), std::abs(dx));
    int t = 0;
    for (int i = 1; i < n; i++) {
        Coord_t c{from.y + (int) std::lround((double) dy * i / n), from.x + (int) std::lround((double) dx * i / n)};
        if (open(c)) {
            a.put(t, ms, c, p.glyph, fg(p.bright));
            a.put(t + ms, 2 * ms, c, '*', fg(p.mid));
        }
        t += ms;
    }
    return t;
}

void muFxBreath(Coord_t from, Coord_t center, int element) {
    Palette const &p = pal(element);
    Anim a;
    a.sound(0, from, "roar"); // a breath is drawn with a roar
    int t = aim(a, from, center, p, 20);
    blast(a, t, center, p);
    a.sound(t, center, "blast");
    a.impact(t);
    a.play();
}

void muFxBolt(Coord_t from, Coord_t to, int element, int outcome) {
    Palette const &p = pal(element);
    Coord_t d{sign(to.y - from.y), sign(to.x - from.x)};
    Anim a;
    int t = aim(a, from, to, p, STEP_MS);
    a.impact(t);
    if (outcome == MU_FX_RESIST) {
        glance(a, t, to, d, p.glyph, p, true);
    } else if (outcome == MU_FX_AFFECT) {
        sparkle(a, t, to, p);
    } else {
        burst(a, t, to, p, false);
        a.sound(t, to, "hit");
    }
    a.play();
}

void muFxMonsterBlow(Coord_t from, Coord_t target, int description, bool hit, bool seen) {
    Coord_t d{sign(target.y - from.y), sign(target.x - from.x)};
    const int F = BLOW_MS;
    Anim a;
    a.sound(0, target, hit ? "hurt" : "miss");
    if (!hit) {
        if (seen) {
            a.put(0, F + 30, beside(target, d), '~', fg(244)); // it misses: the air beside you
        }
        a.play();
        return;
    }
    if (!seen) {
        description = 1; // an unseen monster: only the blow itself shows
    }
    switch (description) {
        case 2: // bite: teeth sink in
            a.put(0, F, target, MU_FX_TINT, on(160));
            a.put(F, F, target, 'v', on(88));
            a.put(2 * F, F, target, MU_FX_TINT, on(52));
            break;
        case 3: // claw: three rakes
            a.put(0, F, target, '/', on(160));
            a.put(F, F, target, '/', on(124));
            a.put(2 * F, F, target, '/', on(88));
            around(a, F, F, target, true, '.', fg(124));
            break;
        case 4: // sting
            a.put(0, F, target, radial(d), on(142, 16));
            a.put(F, 2 * F, target, MU_FX_TINT, on(64));
            break;
        case 7: // gaze: its eyes, then yours
            a.put(0, 2 * F, from, MU_FX_TINT, on(226, 16));
            a.put(F, 2 * F, target, MU_FX_TINT, on(229, 16));
            break;
        case 8:  // breathes on you
        case 13: // spores
            a.put(0, 2 * F, target, MU_FX_TINT, on(106));
            around(a, 0, 2 * F, target, false, '*', fg(106));
            around(a, F, 2 * F, target, true, '.', fg(64));
            break;
        case 9: // spit
            a.put(0, F, target, MU_FX_TINT, on(46, 16));
            around(a, F / 2, F, target, true, '.', fg(46));
            break;
        case 10: // a wail
            around(a, 0, 2 * F, from, false, '~', fg(250));
            around(a, F, 2 * F, from, true, '~', fg(244));
            break;
        case 5:  // touch
        case 12: // crawl
        case 14: // beg
        case 15: // slime
        case 18: // drool
        case 19: // insult
            a.put(0, 2 * F, target, MU_FX_TINT, on(description == 15 || description == 18 ? 70 : description == 12 ? 94 : 97));
            break;
        case 99: // repelled by Protection from Evil
            a.put(0, F, target, MU_FX_TINT, on(231, 16));
            a.put(F, F, from, MU_FX_TINT, on(229, 16));
            break;
        default: // hits, crushes, tramples
            a.put(0, F, target, MU_FX_TINT, on(214, 16));
            a.put(F, F, target, '*', on(160));
            a.put(2 * F, F, target, MU_FX_TINT, on(124));
            around(a, F, F, target, false, '.', fg(244));
            break;
    }
    a.play();
}

void muFxSwing(Coord_t from, Coord_t target, int weapon, int outcome, bool critical) {
    Coord_t d{sign(target.y - from.y), sign(target.x - from.x)};
    int i = 0;
    while (i < 8 && !same(RING[i], d)) {
        i++;
    }
    if (i == 8) {
        return;
    }
    // the swing sweeps across in front of the attacker, s1 to s2
    Coord_t s1 = step(from, RING[(i + 7) % 8]), s2 = step(from, RING[(i + 1) % 8]);
    char g1 = radial(RING[(i + 7) % 8]), g2 = radial(RING[(i + 1) % 8]), gt = radial(d);
    bool hit = outcome == MU_FX_HIT || outcome == MU_FX_KILL;
    const int F = BLOW_MS;
    const uint32_t steel = fg(255), dull = fg(246);

    Anim a;
    a.sound(F, target, !hit ? "miss" : critical ? "crit" : "hit");
    a.impact(F);
    int t = 0;
    switch (weapon_kind(weapon)) {
        case Weapon::Blade: // swords and daggers: a slash
            if (empty(s1)) {
                a.put(0, F, s1, g1, steel);
            }
            if (hit) {
                a.put(F, F, target, gt, on(160));
                a.put(2 * F, F, target, MU_FX_TINT, on(124));
            }
            if (empty(s2)) {
                a.put(hit ? 2 * F : F, F, s2, g2, hit ? steel : dull);
                if (!hit) {
                    a.put(2 * F, 2 * F, s2, '~', fg(244)); // through the air beside it
                }
            }
            t = 3 * F;
            break;
        case Weapon::Heavy: // axes and polearms: a chop
            if (empty(s1)) {
                a.put(0, F + 20, s1, g1, steel);
            }
            if (hit) {
                a.put(F + 20, F + 20, target, gt, on(160));
                a.put(2 * F + 40, F + 20, target, MU_FX_TINT, on(88));
                around(a, 2 * F + 40, F + 20, target, false, '.', fg(124));
            } else {
                Coord_t b = beside(target, d); // it bites the floor beside
                a.put(F + 20, F, b, gt, steel);
                a.put(2 * F + 20, F + 20, b, '*', fg(244));
                a.put(3 * F + 40, F, b, '.', fg(240));
            }
            t = 4 * F;
            break;
        case Weapon::Blunt: // maces, hammers, flails: a thump
            if (hit) {
                a.put(0, F, target, MU_FX_TINT, on(214, 16));
                a.put(F, F, target, '*', on(160));
                a.put(2 * F, F, target, MU_FX_TINT, on(124));
                around(a, F, 2 * F, target, false, '.', fg(244));
            } else {
                Coord_t b = beside(target, d);
                a.put(0, F, b, '*', fg(250));
                a.put(F, F + 20, b, '.', fg(244));
            }
            t = 3 * F;
            break;
        case Weapon::Fist:
            if (hit) {
                a.put(0, F, target, MU_FX_TINT, on(250, 16));
                a.put(F, F, target, MU_FX_TINT, on(160));
            } else {
                a.put(0, F + 20, beside(target, d), '~', fg(244));
            }
            t = 2 * F;
            break;
    }
    if (critical && hit) {
        a.put(t, FRAME_MS, target, MU_FX_TINT, on(226, 196));
        around(a, t, FRAME_MS, target, false, '*', fg(226));
        around(a, t, FRAME_MS, target, true, '*', fg(226));
        t += FRAME_MS;
    }
    if (outcome == MU_FX_KILL) {
        splat(a, t, target);
        a.sound(t, target, "kill");
    }
    a.play();
}

// A speech bubble, "...", pops up over the speaker (under them if someone is
// in the way above) for a moment.
void muFxChat(Coord_t at) {
    auto clear_row = [](Coord_t c) {
        for (int x = c.x - 1; x <= c.x + 1; x++) {
            Coord_t s{c.y, x};
            if (!on_level(s) || dg.floor[s.y][s.x].creature_id != 0 || muOtherPlayerAt(s)) {
                return false;
            }
        }
        return true;
    };
    Coord_t row{at.y - 1, at.x};
    if (!clear_row(row) && clear_row(Coord_t{at.y + 1, at.x})) {
        row.y = at.y + 1;
    }
    Anim a(false);
    a.put(0, CHAT_POP_MS, row, '.', fg(250));
    for (int i = -1; i <= 1; i++) {
        a.put(CHAT_POP_MS, CHAT_MS, Coord_t{row.y, row.x + i}, '.', on(255, 16));
    }
    a.play();
}

void muSound(Coord_t at, const char *name) {
    Anim a;
    a.sound(0, at, name);
    a.play();
}

void muFxDamage(Coord_t at, int amount, bool taken) {
    if (amount > 0) {
        numbers.push_back(Number{at, amount, taken});
    }
}

void muFxDamageShow() {
    if (!numbers.empty()) {
        Anim a; // starts when this player's animations so far have played
        a.play();
    }
}

MuFxTogether::MuFxTogether() {
    if (together++ == 0) {
        together_start = -1;
        together_end = 0;
    }
}

MuFxTogether::~MuFxTogether() {
    if (--together == 0 && together_end > cursor) {
        cursor = together_end;
    }
}

void muMapPanel(bool begin) {
    MuScreen *s = mu_vt_current;
    if (begin) {
        mu_vt_map_panel(s, dg.panel.top, dg.panel.bottom, dg.panel.left, dg.panel.right, dg.panel.row_prt, dg.panel.col_prt);
    }
    mu_vt_map_drawing(s, begin ? 1 : -1);
}

void muMapTile(bool begin) {
    mu_vt_map_drawing(mu_vt_current, begin ? 1 : -1);
}
