// What differs between the games this shell runs: names, colours, where the game data lies and the on-screen keys.
// Everything else in ets/ is the same in every game's project.
import { KeySpec } from '../components/Controls/KeySpec';

export class GameProfile {
  static readonly title: string = '塞尔达传说 王国之泪';
  static readonly short: string = '王国之泪';
  static readonly version: string = '1.4.3';
  static readonly accent: string = '#43D3C3';
  static readonly accent2: string = '#E8C468';
  static readonly dataDir: string = 'game-v143';                         // files/<dataDir>/romfs.bin
  static readonly seed: string = 'android/shaders/v143/seed.vkpack';    // the Vulkan shader seed under files/
  static readonly firstBoot: string = '第一次启动会校验约 17 GB 游戏数据，大约 3 分钟；之后启动会快很多。';
  // Saves (components/Saves): each slot's progress file under the save root, as a pattern whose one group is the
  // slot number (no group: the game keeps one save at the root); other files that mark a save root in an imported
  // archive; an example for messages; the folder an export is zipped under.
  static readonly saveSlot: string = 'slot_(\\d\\d)/progress\\.sav';
  static readonly saveMarkers: string[] = ['option.sav'];
  static readonly saveExample: string = 'slot_00/progress.sav';
  static readonly saveZip: string = 'TotK saves';
}

// The on-screen keys: Switch button bits (nn::hid), place as fractions of the game area, size factor, ring colour.
// A mask of 0 marks an action that has no verified hook yet (shown dimmed).
// The lower right keys sit off the 1.4.3 edge HUD (界面位置 贴边, measured at 2848x1320: the minimap, the sensor and
// temperature icons beside it, the time and weather row under them) and off the bottom-centre prompt and the cutscene
// subtitles: 攻击 left of the minimap, 冲刺 under 跳跃, 锁定 where 切换 was; 切换 also stays off the right stick (双摇杆).
export const ACTION_KEYS: KeySpec[] = [
  new KeySpec('盾牌', 'shield', 4096, 0.055, 0.28, 1, '#5B8CFF'),
  new KeySpec('材料', 'pouch', 8192, 0.13, 0.28, 1, '#FFB443'),
  new KeySpec('口哨', 'note', 32768, 0.205, 0.28, 1, '#2FD3B5'),
  new KeySpec('武器', 'sword', 16384, 0.28, 0.28, 1, '#FF9F43'),
  new KeySpec('蹲下', 'crouch', 16, 0.055, 0.46, 1, ''),
  new KeySpec('望远', 'scope', 32, 0.13, 0.46, 1, ''),
  new KeySpec('拆解', 'blocks', 0, 0.205, 0.46, 1, '', 0, false),
  new KeySpec('能力', 'ability', 64, 0.63, 0.34, 1, '#9B7BFF'),
  new KeySpec('投掷', 'throw', 128, 0.72, 0.49, 1, '#FF9F43'),
  new KeySpec('弓箭', 'bow', 512, 0.83, 0.49, 1, '#FFB443'),
  new KeySpec('跳跃', 'jump', 4, 0.945, 0.49, 1, '#35D07F'),
  new KeySpec('攻击', 'attack', 8, 0.78, 0.69, 1.32, '#FF5D6C'),
  new KeySpec('切换', 'swap', 0, 0.66, 0.63, 1, '', 0, false),
  new KeySpec('锁定', 'target', 256, 0.72, 0.90, 1, '#38C8F0'),
  new KeySpec('冲刺', 'dash', 2, 0.955, 0.65, 1, '#FFC53D'),   // ~32 px from 跳跃 and from the minimap (2026-11-15)
  new KeySpec('确定', 'check', 1, 0.875, 0.28, 0.75, '#35D07F', 2.2),
  new KeySpec('返回', 'undo', 2, 0.37, 0.085, 0.75, '', 1.12),
  new KeySpec('地图', 'map', 2048, 0.5, 0.115, 0.75, '', 1.12),
  new KeySpec('菜单', 'menu', 1024, 0.63, 0.085, 0.75, '', 1.12),
];

// Shown while the game loads, one at a time.
export const TIPS: string[] = [
  '游戏中点屏幕顶部中间的小标签（或用返回手势）打开游戏菜单：继续、设置、触屏按键、按键布局、视角操作',
  '连接手柄后触屏按键会自动隐藏；在游戏菜单里可以强制显示',
  '每次开始游戏前都会自动备份存档，在“存档”页可以恢复',
  '“设置”里的预设可以一键切换流畅、均衡、画质、高帧率',
  '划屏视角：在屏幕右侧滑动转动镜头；也可以在游戏菜单里改成双摇杆',
  '不习惯游戏定制的按键？在游戏菜单里把按键布局换成标准手柄',
];
