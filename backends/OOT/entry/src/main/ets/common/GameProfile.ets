// What differs between the games this shell runs: names, colours, where the game data lies and the on-screen keys.
// Everything else in ets/ is the same in every game's project.
import { KeySpec } from '../components/Controls/KeySpec';
import { STANDARD_KEYS } from '../components/Controls/StandardKeys';

export class GameProfile {
  static readonly title: string = '塞尔达传说 时之笛';
  static readonly short: string = '时之笛';
  static readonly version: string = '131072';                           // the NUP has no display version, only the update's number (shown as v131072)
  static readonly accent: string = '#58B947';
  static readonly accent2: string = '#E8C468';
  static readonly dataDir: string = 'game-oot';                          // files/<dataDir>/romfs.bin
  static readonly seed: string = 'android/shaders/oot131072/seed.vkpack';   // the Vulkan shader seed under files/
  static readonly firstBoot: string = '第一次启动会校验约 25 GB 游戏数据，大约 4 分钟；之后启动会快很多。';
  // Saves (components/Saves): each slot's progress file under the save root, as a pattern whose one group is the
  // slot number (no group: the game keeps one save at the root); other files that mark a save root in an imported
  // archive; an example for messages; the folder an export is zipped under. Ocarina of Time is Tears of the Kingdom's
  // engine and autosaves into slot_00 / slot_01 beside option.sav and account.sav (nxnative's notes); the file in a
  // slot is taken as Tears of the Kingdom's until a save made here shows it.
  static readonly saveSlot: string = 'slot_(\\d\\d)/progress\\.sav';
  static readonly saveMarkers: string[] = ['option.sav', 'account.sav'];
  static readonly saveExample: string = 'slot_00/progress.sav';
  static readonly saveZip: string = 'OoT saves';
}

// Ocarina of Time uses the whole pad (a 3D field and both sticks): its own layout is the Switch's
// (components/Controls/StandardKeys.ets).
export const ACTION_KEYS: KeySpec[] = STANDARD_KEYS;

// Shown while the game loads, one at a time.
export const TIPS: string[] = [
  '游戏中点屏幕顶部中间的小标签（或用返回手势）打开游戏菜单：继续、设置、触屏按键、视角操作',
  '连接手柄后触屏按键会自动隐藏；在游戏菜单里可以强制显示',
  '每次开始游戏前都会自动备份存档，在“存档”页可以恢复',
  '“设置”里的预设可以一键切换流畅、均衡、画质、高帧率',
  '划屏视角：在屏幕右侧滑动转动镜头；也可以在游戏菜单里改成双摇杆',
];
