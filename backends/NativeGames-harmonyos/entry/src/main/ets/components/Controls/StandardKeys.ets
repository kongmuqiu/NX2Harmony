// The standard on-screen controller, the same for every game: the Switch's own buttons where they sit on the console,
// R / ZR at the top right and L / ZL just right of the hearts (the 1.4.3 edge HUD holds the top-left corner), the D-pad over the left stick, A B X Y in their diamond, − and + at the top centre,
// the stick clicks under the sticks (B a little higher than the console's, off the 1.4.3 edge HUD's sensor icons
// beside the minimap). Chosen with 触屏按键布局 = 标准手柄 (or the game menu) instead of the game's own
// labelled layout (common/GameProfile.ets ACTION_KEYS).
import { KeySpec } from './KeySpec';

export const STANDARD_KEYS: KeySpec[] = [
  new KeySpec('ZL', '', 256, 0.33, 0.30, 1, '#8A94A6'),   // L / ZL right of the hearts and the stamina wheel (edge HUD)
  new KeySpec('L', '', 64, 0.33, 0.12, 1, '#8A94A6'),
  new KeySpec('上', 'dpad_up', 8192, 0.13, 0.30, 0.9),
  new KeySpec('左', 'dpad_left', 4096, 0.06, 0.42, 0.9),
  new KeySpec('右', 'dpad_right', 16384, 0.20, 0.42, 0.9),
  new KeySpec('下', 'dpad_down', 32768, 0.13, 0.54, 0.9),
  new KeySpec('L3', '', 16, 0.29, 0.92, 0.8),
  new KeySpec('R3', '', 32, 0.70, 0.92, 0.8),
  new KeySpec('ZR', '', 512, 0.86, 0.17, 1, '#8A94A6'),
  new KeySpec('R', '', 128, 0.945, 0.17, 1, '#8A94A6'),
  new KeySpec('X', '', 4, 0.86, 0.36, 1, '#5B8CFF'),
  new KeySpec('Y', '', 8, 0.775, 0.52, 1, '#35D07F'),
  new KeySpec('A', '', 1, 0.945, 0.52, 1, '#FF5D6C'),
  new KeySpec('B', '', 2, 0.86, 0.66, 1, '#FFC53D'),
  new KeySpec('−', '', 2048, 0.40, 0.085, 0.75, '', 1.12),
  new KeySpec('+', '', 1024, 0.60, 0.085, 0.75, '', 1.12),
];
