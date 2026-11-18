// What differs between the games this shell runs: names, colours, where the game data lies and the on-screen keys.
// Everything else in ets/ is the same in every game's project.
import { KeySpec } from '../components/Controls/KeySpec';
import { STANDARD_KEYS } from '../components/Controls/StandardKeys';

export class GameProfile {
  static readonly title: string = '宝可梦传说 Z-A';
  static readonly short: string = '宝可梦 Z-A';
  static readonly version: string = '2.0.2';
  static readonly accent: string = '#8C7CFF';
  static readonly accent2: string = '#FF7EB6';
  static readonly dataDir: string = 'game-za';                           // files/<dataDir>/romfs.bin
  static readonly seed: string = 'android/shaders/za202/seed.vkpack';   // the Vulkan shader seed under files/
  static readonly firstBoot: string = '第一次启动会校验约 6 GB 游戏数据，大约 1 分钟；之后启动会快很多。';
  // Saves (components/Saves): each slot's progress file under the save root, as a pattern whose one group is the
  // slot number (no group: the game keeps one save at the root); other files that mark a save root in an imported
  // archive; an example for messages; the folder an export is zipped under.
  static readonly saveSlot: string = 'main';
  static readonly saveMarkers: string[] = ['backup'];
  static readonly saveExample: string = 'main';
  static readonly saveZip: string = 'Z-A saves';
}

// Pokémon Legends: Z-A uses the whole pad: its own layout is the Switch's (components/Controls/StandardKeys.ets).
export const ACTION_KEYS: KeySpec[] = STANDARD_KEYS;

// Shown while the game loads, one at a time.
export const TIPS: string[] = [
  '游戏中点屏幕顶部中间的小标签（或用返回手势）打开游戏菜单：继续、设置、触屏按键、视角操作',
  '连接手柄后触屏按键会自动隐藏；在游戏菜单里可以强制显示',
  '每次开始游戏前都会自动备份存档，在“存档”页可以恢复',
  '“设置”里的预设可以一键切换流畅、均衡、画质、高帧率',
  '划屏视角：在屏幕右侧滑动转动镜头；也可以在游戏菜单里改成双摇杆',
];
