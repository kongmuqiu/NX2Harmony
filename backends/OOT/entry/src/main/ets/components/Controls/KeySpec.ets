// One on-screen key (common/GameProfile.ets ACTION_KEYS): the label (also its accessibility name), the glyph
// (common/Icons.ets icon(); '' shows the label instead), the Switch button bits, its centre as fractions of the game
// area, a size factor, a ring colour ('' neutral), a pill width in key sizes (0 for a round key), and whether the
// action works yet.
export class KeySpec {
  label: string;
  icon: string;
  mask: number;
  x: number;
  y: number;
  factor: number;
  tint: string;
  pill: number;
  available: boolean;

  constructor(label: string, icon: string, mask: number, x: number, y: number, factor: number = 1, tint: string = '',
    pill: number = 0, available: boolean = true) {
    this.label = label; this.icon = icon; this.mask = mask; this.x = x; this.y = y; this.factor = factor;
    this.tint = tint; this.pill = pill; this.available = available;
  }
}
