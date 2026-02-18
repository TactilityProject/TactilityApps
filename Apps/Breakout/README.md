# Breakout

A classic brick-breaking arcade game inspired by Arkanoid, built for Tactility devices. Features power-up capsules, multi-hit bricks, multi-ball action, and responsive scaling across all screen sizes.

## Features

- **8 brick colors** with per-color scoring (50-120 points)
- **Silver bricks** that take multiple hits to break (level 3+)
- **Gold bricks** that are indestructible (level 5+)
- **7 power-up capsules** that drop from destroyed bricks
- **12 handcrafted level patterns** + seeded procedural generation (level 13+)
- **High score** persistence across sessions
- **Sound toggle** with persistent setting
- **Responsive scaling** for all Tactility devices (Cardputer to 800x480+ panels)
- Touch, Trackball and Keyboard input support

## Levels

Each level has a unique brick layout pattern. Colors rotate each level, and Silver/Gold bricks are added as difficulty increases.

```
 Level 1: Full Grid          Level 2: Checkerboard       Level 3: Diamond
 [][][][][][][][]            [] [] [] [] []              [][][][][][]
 [][][][][][][][]            [] [] [] [] []                [][][][]
 [][][][][][][][]            [] [] [] [] []              [][][][][][]
 [][][][][][][][]            [] [] [] [] []                [][][][]

 Level 4: Stripes            Level 5: Pyramid            Level 6: Inverted V
 [][][][][][][][]            [][][][][][][][]                  []
                             . [][][][][]  .                [][][][]
 [][][][][][][][]            .   [][][]    .              [][][][][][]
                             .     []      .            [][][][][][][][]

 Level 7: Vert Stripes       Level 8: Zigzag             Level 9: Blocks
 [] [] [] [] []              [][] .  [][] .              [][] .  [][] .
 [] [] [] [] []              .  [][] .  [][]             [][] .  [][] .
 [] [] [] [] []              [][] .  [][] .              .  [][] .  [][]
 [] [] [] [] []              .  [][] .  [][]             .  [][] .  [][]

 Level 10: Double Diamond   Level 11: Border Frame      Level 12: Center Cross
   [][]    [][]              [][][][][][][][]                 [] []
  [][][]  [][][]                                          [][][][][][]
   [][]    [][]              [][][][][][][][]                 [] []
  [][][]  [][][]                                          [][][][][][]
```

After level 12, layouts are **procedurally generated** using a seeded algorithm — the same level number always produces the same layout.

## Power-Up Capsules

Capsules drop from destroyed bricks (~15% chance) and fall toward the paddle. Catch them to activate!

```
  Capsule drops from brick:          Catch with paddle:

        [brick]                            |C|
          |C|                         =============
          |C|                          ^^^paddle^^^
          |C|
          v
    =============
```

| Capsule | Color | Letter | Effect |
|---------|-------|--------|--------|
| Laser | `Red` | **L** | Paddle auto-fires lasers upward, destroying bricks on contact |
| Extend | `Blue` | **E** | Paddle width increases by 50% |
| Catch | `Green` | **C** | Ball sticks to paddle on contact, auto-releases after 3 seconds |
| Slow | `Orange` | **S** | Ball speed reduced to 60%, gradually recovers over 5 seconds |
| BreakOut | `Pink` | **B** | Opens exit on right wall — move paddle to exit for +10,000 pts |
| Split | `Cyan` | **D** | Splits ball into 3! Life lost only when ALL balls are gone |
| Extra Life | `Grey` | **+** | +1 life (rarest drop) |

Power-ups reset on life lost or level clear. No capsules drop while multiple balls are active.

## Brick Types

```
 Normal bricks:     Silver bricks:      Gold bricks:
 +-----------+      +===========+       +###########+
 |  colored  |      | multi-hit |       | unbreakable|
 +-----------+      +===========+       +###########+
  1 hit, gone        2-3 hits            indestructible
  50-120 pts         50 x level          bounces ball
```

| Type | Appears | Hits | Score | Visual |
|------|---------|------|-------|--------|
| Normal | All levels | 1 | 50-120 (by color) | Colored, no border |
| Silver | Level 3+ | 2 (lvl 3-6), 3 (lvl 7+) | 50 x level | Grey with white border, darkens on hit |
| Gold | Level 5+ | Indestructible | - | Amber with gold border |

## Scoring

| Source | Points |
|--------|--------|
| Purple brick | 50 |
| Orange brick | 60 |
| Cyan brick | 70 |
| Green brick | 80 |
| Red brick | 90 |
| Blue brick | 100 |
| Pink brick | 110 |
| Yellow brick | 120 |
| Silver brick | 50 x level |
| BreakOut exit | 10,000 |

Ball speed increases by +0.15 for every 5 bricks destroyed, and +0.3 per level.

## Controls

**Touchscreen:** Touch/drag to move paddle, tap to launch ball or release caught ball
**Trackball:** Move trackball left and right to move paddle, press to launch ball or release caught ball

**Keyboard:**
| Key | Action |
|-----|--------|
| `LEFT` / `a` / `,` | Move paddle left |
| `RIGHT` / `d` / `/` | Move paddle right |
| `SPACE` / `ENTER` | Launch ball / release caught ball / pause |

**Toolbar:** Pause button and sound toggle button

## Requirements

- Tactility SDK 0.7.0-dev
- ESP32, ESP32-S3, ESP32-C6, or ESP32-P4

## Building

```bash
python tactility.py build
# or build + install + run:
python tactility.py bir <device-ip>
```
