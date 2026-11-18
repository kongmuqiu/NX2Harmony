// Game settings for the native runtime (libtotk_ohos.so). Stored as files/android/settings.json and written out as
// files/android/env.txt, the KEY=VALUE file the runtime reads before it starts (native-harmonyos/ohos/totk_napi.c):
// every change applies at the next start. Options follow the Android app's Settings.java (games/totk/android/java/
// com/aure/totk/setup), plus HarmonyOS-only ones (frame-rate cap up to 120 through the panel's vsync, HarmonyOS spatial
// upscaling, core lock, full-screen camera, swipe-camera speed) and the 1.4.3 renderer reductions the runtime exports
// as switchable veneers (native-harmonyos/overlay/runtime/game_patches_v143.c patch_ohos_render_switches).
import { fileIo as fs } from '@kit.CoreFileKit';

export class Choice {
  value: string;
  label: string;
  constructor(value: string, label: string) { this.value = value; this.label = label; }
}

export const V_ALL = 0;    // every game version
export const V_143 = 1;    // 1.4.3 only (the runtime's camera / renderer patches for that version)
export const V_WIDE = 2;   // a version with the wide camera patch: TOTK 1.4.3 and Pokémon Legends: Z-A 2.0.2

export class SettingOption {
  key: string;
  group: string;
  title: string;
  fallback: string;
  choices: Choice[];
  note: string;
  versions: number;
  constructor(key: string, group: string, title: string, fallback: string, choices: Choice[], note: string = '',
    versions: number = V_ALL) {
    this.key = key; this.group = group; this.title = title; this.fallback = fallback; this.choices = choices;
    this.note = note; this.versions = versions;
  }
  shownFor(gameVersion: string): boolean {
    if (this.versions === V_WIDE) return gameVersion === '1.4.3' || gameVersion === '2.0.2';
    return this.versions === V_ALL || gameVersion === '1.4.3';
  }
}

export const GROUPS: string[] = ['性能', '渲染', '画质', '画面', '操作'];

export const OPTIONS: SettingOption[] = [
  // 性能
  new SettingOption('fps_cap', '性能', '帧率上限', '30',
    [new Choice('30', '30'), new Choice('60', '60'), new Choice('90', '90'), new Choice('120', '120')],
    '60 及以上用游戏的 60 帧补丁；性能不够时达不到上限'),
  new SettingOption('render_res', '性能', '渲染分辨率', 'game',
    [new Choice('game', '游戏默认'), new Choice('1280x720', '720p'), new Choice('1152x648', '648p'),
      new Choice('960x540', '540p')], '场景的渲染尺寸，界面不受影响；越低 GPU 越轻、越糊'),
  new SettingOption('console_mode', '性能', '主机模式', 'handheld',
    [new Choice('handheld', '掌机'), new Choice('docked', '底座')], '底座：场景 1600×900，更清晰、更吃 GPU'),
  new SettingOption('core_lock', '性能', '锁核调度', 'on',
    [new Choice('on', '开'), new Choice('off', '关')], '主线程、录制线程放到超大核 / 大核'),
  new SettingOption('frame_gen', '性能', '鸿蒙超级插帧', 'off',
    [new Choice('off', '关'), new Choice('on', '开')], '开发中，暂不生效'),
  // 渲染（1.4.3 的渲染精简：减少要录制的绘制，CPU 录制线程是现在的瓶颈）
  new SettingOption('far_clamp', '渲染', '远景距离', 'game',
    [new Choice('game', '游戏'), new Choice('12000', '12 km'), new Choice('8000', '8 km'), new Choice('5000', '5 km')],
    '限制渲染远平面：远处物体少画，录制更轻（参考实测约少 16% 绘制）；地平线附近会提前消失', V_143),
  new SettingOption('shadow_cascades', '渲染', '阴影级数', 'game',
    [new Choice('game', '游戏'), new Choice('3', '3'), new Choice('2', '2'), new Choice('1', '1')],
    '方向光阴影的级联数：每少一级少一遍投影绘制；远处阴影会变少', V_143),
  new SettingOption('point_shadows', '渲染', '点光源阴影', 'game',
    [new Choice('game', '游戏'), new Choice('4', '4'), new Choice('2', '2'), new Choice('1', '1')],
    '同时投影的点光源数上限：每个是一组单独的投影绘制', V_143),
  new SettingOption('bc_etc', '渲染', 'BC 贴图转 ETC2', 'fast',
    [new Choice('fast', '开（快）'), new Choice('quality', '开（高质量）'), new Choice('off', '关')],
    '鸿蒙 GPU 没有 BC 格式：上传时把 BC 颜色贴图实时转成 ETC2（显存和采样带宽回到压缩量级），关则解码成 RGBA8（4–8 倍）'),
  new SettingOption('bc_half', '渲染', '大贴图减半', '0',
    [new Choice('0', '关'), new Choice('4', '1024² 以上'), new Choice('1', '512² 以上')],
    '颜色 BC 贴图已实时转成 ETC2，显存不再紧张，默认关。开：BC4/BC5/BC6H 大贴图从第二级 mip 开始建，省显存但近看略糊、按级采样的效果可能偏一级'),
  new SettingOption('shadows', '渲染', '阴影分辨率', 'high',
    [new Choice('high', '高'), new Choice('medium', '中'), new Choice('low', '低')],
    '阴影贴图大小（中 288 / 低 256）：省 GPU，绘制数不变；阴影更软更糙', V_143),
  // 画质
  new SettingOption('upscaler', '画质', '超分辨率', 'off',
    [new Choice('off', '关'), new Choice('fsr', 'FSR'), new Choice('sgsr', 'SGSR'), new Choice('xeg', '鸿蒙空域超分')],
    '把渲染画面放大到输出分辨率；鸿蒙空域超分是系统 XEngine 的 GPU 超分，不支持时改用 SGSR'),
  new SettingOption('sharpness', '画质', '锐化', '20',
    [new Choice('0', '0'), new Choice('20', '20'), new Choice('40', '40'), new Choice('60', '60'), new Choice('80', '80'),
      new Choice('100', '100')], '超分时的锐化强度'),
  new SettingOption('aa', '画质', '抗锯齿', 'fxaa',
    [new Choice('fxaa', '游戏 FXAA'), new Choice('smaa', 'SMAA'), new Choice('off', '关')],
    'SMAA 代替游戏的 FXAA，边缘更干净；关：最省，闪烁更明显'),
  // 画面
  new SettingOption('aspect', '画面', '画面比例', 'fill',
    [new Choice('fill', '全面屏'), new Choice('16:9', '16:9')],
    '全面屏：相机视野加宽到屏幕比例，铺满屏幕、没有左右黑边，界面仍按 16:9 摆放', V_WIDE),
  new SettingOption('output_res', '画面', '输出分辨率', 'native',
    [new Choice('1280', '1280'), new Choice('1920', '1920'), new Choice('native', '屏幕原生')], '上屏图像的宽度，开超分时由超分放大'),
  new SettingOption('overlay', '画面', '性能浮窗', 'full',
    [new Choice('off', '关'), new Choice('fps', '帧率'), new Choice('full', '完整')]),
  new SettingOption('overlay_pos', '画面', '浮窗位置', 'tr',
    [new Choice('tl', '左上'), new Choice('tr', '右上'), new Choice('bl', '左下'), new Choice('br', '右下')]),
  // 操作
  new SettingOption('touch_controls', '操作', '触屏按键', 'auto',
    [new Choice('auto', '连手柄时隐藏'), new Choice('always', '一直显示')], '蓝牙或有线手柄连上后自动隐藏屏幕按键，断开后恢复'),
  new SettingOption('touch_layout', '操作', '触屏按键布局', 'custom',
    [new Choice('custom', '游戏定制'), new Choice('standard', '标准手柄')],
    '游戏定制：按这个游戏的动作标好的按键；标准手柄：Switch 原样的 A B X Y、十字键、肩键和 + −（游戏菜单里也能切换）'),
  new SettingOption('face_buttons', '操作', '手柄按键布局', 'label',
    [new Choice('label', '按标签（任天堂）'), new Choice('position', '按位置（Xbox）')],
    '按标签：手柄上印着 A 的键就是游戏里的 A；按位置：最下面的键是 B、右边是 A，适合 Xbox 布局的手柄'),
  new SettingOption('rumble', '操作', '震动', 'mid',
    [new Choice('off', '关'), new Choice('low', '弱'), new Choice('mid', '中'), new Choice('high', '强')],
    '游戏的震动用手机马达表现；手柄自身的马达系统暂不支持'),
  new SettingOption('gyro', '操作', '体感瞄准', 'on',
    [new Choice('on', '开'), new Choice('flip', '开（方向反了选这个）'), new Choice('off', '关')],
    '用手机的陀螺仪瞄准，和 Switch 掌机模式一样；只在游戏画面在前台时打开传感器'),
  new SettingOption('look_speed', '操作', '划屏视角速度', 'mid',
    [new Choice('low', '慢'), new Choice('mid', '中'), new Choice('high', '快'), new Choice('max', '很快')],
    '右侧空白处滑动转视角：画面跟着手指走，停手即停'),
  new SettingOption('audio_latency', '操作', '音频延迟', 'balanced',
    [new Choice('low', '低'), new Choice('balanced', '均衡'), new Choice('safe', '稳定')]),
  new SettingOption('game_language', '操作', '游戏语言', 'auto',
    [new Choice('auto', '跟随系统'), new Choice('zh-Hans', '简体中文'), new Choice('zh-Hant', '繁體中文'), new Choice('en-US', 'English'),
      new Choice('ja', '日本語')]),
];

export class Preset {
  name: string;
  caption: string;
  keys: string[];
  values: string[];
  constructor(name: string, caption: string, keys: string[], values: string[]) {
    this.name = name; this.caption = caption; this.keys = keys; this.values = values;
  }
}

// Measured on the SGU-AL10 (2026-11-08, 1.4.3, Great Sky Island start): 流畅 26-27 fps with the GPU throttled to
// 750 MHz, 均衡 25 fps at 930 MHz; the GPU's cost is the game's ~320 render passes a frame, so shadow and scene size
// move it little, while the 1920-wide output saves the upscale and present passes about 1.5 ms. Colour BC textures are
// transcoded to ETC2 at upload (GL 1.85 GB against the system's 3.1 GB kill line), so no preset halves textures any more;
// the game's FXAA stays on in every preset: without it 540p shimmers (reported as flicker). Shadows: 256 / 1 cascade /
// 1 point shadow bought about 1 fps over 288 / 2 cascades / the game's point budget (a18 A/B) at a visible cost in
// shadow edges, cascade pops and point-shadow swapping, so 流畅 keeps the medium setting.
const PRESET_KEYS: string[] = ['fps_cap', 'render_res', 'console_mode', 'upscaler', 'aa', 'shadows', 'shadow_cascades',
  'point_shadows', 'far_clamp', 'output_res', 'bc_half', 'frame_gen'];
export const PRESETS: Preset[] = [
  new Preset('流畅', '540p · 游戏 FXAA · 阴影中/2 级 · 输出 1920 · SGSR', PRESET_KEYS,
    ['30', '960x540', 'handheld', 'sgsr', 'fxaa', 'medium', '2', 'game', '8000', '1920', '0', 'off']),
  new Preset('均衡', '游戏分辨率 · 阴影中 · 原生输出 · SGSR', PRESET_KEYS,
    ['30', 'game', 'handheld', 'sgsr', 'fxaa', 'medium', '3', 'game', '12000', 'native', '0', 'off']),
  new Preset('画质', '底座 · 完整渲染 · FSR + SMAA', PRESET_KEYS,
    ['30', 'game', 'docked', 'fsr', 'smaa', 'high', 'game', 'game', 'game', 'native', '0', 'off']),
  new Preset('高帧率', '60 帧 · 540p · 游戏 FXAA · 阴影中/2 级 · 输出 1920', PRESET_KEYS,
    ['60', '960x540', 'handheld', 'sgsr', 'fxaa', 'medium', '2', 'game', '8000', '1920', '0', 'off']),
];

export class TotkSettings {
  values: Map<string, string> = new Map<string, string>();

  static dir(filesDir: string): string { return filesDir + '/android'; }

  static load(filesDir: string): TotkSettings {
    const s = new TotkSettings();
    for (const o of OPTIONS) s.values.set(o.key, o.fallback);
    try {
      const path = TotkSettings.dir(filesDir) + '/settings.json';
      if (fs.accessSync(path)) {
        const stored = JSON.parse(fs.readTextSync(path)) as Record<string, string>;
        for (const o of OPTIONS) {
          const v = stored[o.key];
          if (typeof v === 'string' && o.choices.some((c: Choice) => c.value === v)) s.values.set(o.key, v);
        }
      }
    } catch (e) {
      // unreadable settings: the defaults
    }
    return s;
  }

  get(key: string): string { return this.values.get(key) ?? ''; }

  set(key: string, value: string): void { this.values.set(key, value); }

  applyPreset(p: Preset): void {
    for (let i = 0; i < p.keys.length; i++) this.values.set(p.keys[i], p.values[i]);
  }

  // The preset whose values all match the current ones (options hidden for this game version are not compared),
  // else '' (custom).
  matchingPreset(gameVersion: string): string {
    for (const p of PRESETS) {
      let all = true;
      for (let i = 0; i < p.keys.length && all; i++) {
        const o = OPTIONS.find((x: SettingOption) => x.key === p.keys[i]);
        if (o !== undefined && o.shownFor(gameVersion) && this.get(p.keys[i]) !== p.values[i]) all = false;
      }
      if (all) return p.name;
    }
    return '';
  }

  // The env.txt lines for these settings; screenWidth / screenHeight: the panel's long and short side in pixels
  // (output_res 'native'; aspect 'fill': the camera's ratio, runtime/aspect_patch.h TOTK_ASPECT).
  envLines(screenWidth: number, screenHeight: number = 0): string[] {
    const out: string[] = ['# written by the settings page (entry/src/main/ets/components/Settings)'];
    out.push('TOTK_OHOS_FPS_CAP=' + this.get('fps_cap'));
    if (this.get('render_res') !== 'game') out.push('TOTK_RENDER_RES=' + this.get('render_res'));
    out.push('TOTK_OPERATION_MODE=' + this.get('console_mode'));
    out.push('TOTK_OHOS_FG=' + (this.get('frame_gen') === 'on' ? '1' : '0'));
    if (this.get('core_lock') === 'off') { out.push('TOTK_OHOS_PIN=0'); out.push('TOTK_OHOS_SELF=0'); }
    // 1.4.3 renderer reductions (the 1.0.0 runtime has none of these sites and ignores the variables)
    if (this.get('far_clamp') !== 'game') out.push('TOTK_FAR_CLAMP=' + this.get('far_clamp'));
    if (this.get('shadow_cascades') !== 'game') out.push('TOTK_SHADOW_CASCADES=' + this.get('shadow_cascades'));
    if (this.get('point_shadows') !== 'game') out.push('TOTK_POINT_SHADOWS=' + this.get('point_shadows'));
    const shadows = this.get('shadows');
    if (shadows === 'medium') out.push('TOTK_SHADOW_RES=288');
    if (shadows === 'low') out.push('TOTK_SHADOW_RES=256');
    out.push('TOTK_OHOS_BC_HALF_MB=' + this.get('bc_half'));
    const etc = this.get('bc_etc');
    out.push('TOTK_OHOS_BC_ETC=' + (etc === 'off' ? '0' : '1'));
    out.push('TOTK_OHOS_ETC_QUALITY=' + (etc === 'quality' ? '1' : '0'));
    const up = this.get('upscaler');
    out.push('TOTK_POST_SCALE=' + up);
    const sharp = Number(this.get('sharpness')) / 100;
    if (up === 'fsr') out.push('TOTK_POST_SHARPNESS=' + sharp.toFixed(2));
    if (up === 'sgsr') out.push('TOTK_POST_SGSR_SHARPNESS=' + (sharp * 4).toFixed(2));
    if (up === 'xeg') out.push('TOTK_XEG_SHARPNESS=' + sharp.toFixed(2));
    const aa = this.get('aa');   // as the Android app: SMAA replaces the game's FXAA
    if (aa === 'smaa') { out.push('TOTK_FXAA=0'); out.push('TOTK_POST_AA=smaa'); }
    if (aa === 'off') out.push('TOTK_FXAA=0');
    const res = this.get('output_res');
    out.push('TOTK_APP_WIDTH=' + (res === 'native' ? String(Math.max(1280, Math.round(screenWidth))) : res));
    const fill = this.get('aspect') === 'fill' && screenHeight > 0 && screenWidth / screenHeight > 16 / 9 + 0.01;
    out.push('TOTK_ASPECT=' + (fill ? Math.round(screenWidth) + ':' + Math.round(screenHeight) : '0'));
    out.push('TOTK_OVERLAY=' + this.get('overlay'));
    out.push('TOTK_OVERLAY_POS=' + this.get('overlay_pos'));
    const look = this.get('look_speed');
    out.push('TOTK_OHOS_LOOK_SPEED=' + (look === 'low' ? '0.8' : look === 'high' ? '1.8' : look === 'max' ? '2.6' : '1.2'));
    const lat = this.get('audio_latency');
    out.push('TOTK_AUDIO_PREFILL_MS=' + (lat === 'low' ? '10' : lat === 'safe' ? '40' : '20'));
    if (this.get('game_language') !== 'auto') out.push('TOTK_LANG=' + this.get('game_language'));
    out.push('TOTK_FACE_BUTTONS=' + (this.get('face_buttons') === 'position' ? 'position' : 'label'));
    const rumble = this.get('rumble');
    out.push('TOTK_RUMBLE=' + (rumble === 'off' ? '0' : '1'));
    if (rumble !== 'off') out.push('TOTK_RUMBLE_GAIN=' + (rumble === 'low' ? '0.5' : rumble === 'high' ? '2' : '1'));
    const gyro = this.get('gyro');
    out.push('TOTK_GYRO=' + (gyro === 'off' ? '0' : '1'));
    out.push('TOTK_OHOS_GYRO_FLIP=' + (gyro === 'flip' ? '1' : '0'));
    return out;
  }

  save(filesDir: string, screenWidth: number, screenHeight: number = 0): void {
    const dir = TotkSettings.dir(filesDir);
    if (!fs.accessSync(dir)) fs.mkdirSync(dir);
    const obj: Record<string, string> = {};
    this.values.forEach((v: string, k: string) => { obj[k] = v; });
    TotkSettings.writeText(dir + '/settings.json', JSON.stringify(obj, null, 1) + '\n');
    TotkSettings.writeText(dir + '/env.txt', this.envLines(screenWidth, screenHeight).join('\n') + '\n');
  }

  private static writeText(path: string, text: string): void {
    const f = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    fs.writeSync(f.fd, text);
    fs.closeSync(f);
  }
}
