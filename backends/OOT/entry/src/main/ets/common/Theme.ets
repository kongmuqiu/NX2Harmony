// The shell's look: a dark game launcher. Colours here; the game's accent is in GameProfile.ets. The menu icons are
// the coloured badges (common/Icons.ets badge(), drawn by tools/ui_icons.py), the small ones white glyphs (icon()).
export class Ui {
  static readonly bg: string = '#090C13';
  static readonly bg2: string = '#121A2A';
  static readonly card: string = '#151C29';
  static readonly card2: string = '#1D2636';
  static readonly line: string = '#2A3448';
  static readonly text: string = '#F3F6FB';
  static readonly text2: string = '#B4BED0';
  static readonly text3: string = '#717D93';
  static readonly ok: string = '#35D07F';
  static readonly warn: string = '#FFB443';
  static readonly bad: string = '#FF5D6C';
  static readonly glass: string = '#B3121826';   // ARGB: the game page's overlays over the picture
}

// '#RRGGBB' with an opacity (0..1) as ArkUI's '#AARRGGBB'.
export function alpha(hex: string, a: number): string {
  const v = Math.max(0, Math.min(255, Math.round(a * 255))).toString(16).toUpperCase();
  return '#' + (v.length < 2 ? '0' + v : v) + hex.substring(hex.length - 6);
}

export function formatBytes(n: number): string {
  if (n >= 1024 * 1024 * 1024) return (n / 1024 / 1024 / 1024).toFixed(1) + ' GB';
  if (n >= 1024 * 1024) return (n / 1024 / 1024).toFixed(0) + ' MB';
  return Math.round(n / 1024) + ' KB';
}
