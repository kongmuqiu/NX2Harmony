// The app's start page, a game launcher: the game's art and title, what is ready on this phone (game data, shader
// seed, saves), the big start button, and the 设置 / 存档 / 关于 pages. Settings are saved as they change (env.txt for
// the next start); 开始游戏 backs the saves up when they changed, turns the window to full-screen landscape and opens
// the game page (pages/Game, the native runtime). Landscape and full screen, as the game: the art and the start on
// the left, the pages on the right.
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { TotkSettings } from '../components/Settings/SettingsModel';
import { SettingsPanel } from '../components/Settings/SettingsPanel';
import { SavesPanel } from '../components/Saves/SavesPanel';
import { AboutPanel } from '../components/About/AboutPanel';
import { SaveStore } from '../components/Saves/SaveStore';
import { GameProfile } from '../common/GameProfile';
import { Ui, alpha, formatBytes } from '../common/Theme';
import { GameButton, StatusChip } from '../common/Widgets';
import { badge } from '../common/Icons';
import { runtimeGameVersion, screenLongSide, screenShortSide } from '../common/Device';

class TabItem {
  key: string;
  label: string;
  badgeName: string;
  constructor(key: string, label: string, badgeName: string) { this.key = key; this.label = label; this.badgeName = badgeName; }
}

const TABS: TabItem[] = [new TabItem('settings', '设置', 'settings'), new TabItem('saves', '存档', 'save'),
  new TabItem('about', '关于', 'info')];

@Entry
@Component
struct Launcher {
  @State settings: TotkSettings = new TotkSettings();
  @State message: string = '';
  @State tab: string = 'settings';
  @State dataSize: number = -1;   // files/<dataDir>/romfs.bin in bytes, -1 when it is not there
  @State seedReady: boolean = false;
  @State saveSlots: number = 0;
  @State glow: number = 0.25;     // the start button's breathing light

  private context(): common.UIAbilityContext {
    return this.getUIContext().getHostContext() as common.UIAbilityContext;
  }

  aboutToAppear(): void {
    const filesDir = this.context().filesDir;
    this.settings = TotkSettings.load(filesDir);
    this.checkData();
    this.getUIContext().animateTo({ duration: 1600, iterations: -1, playMode: PlayMode.Alternate, curve: Curve.EaseInOut },
      () => { this.glow = 0.7; });
  }

  onPageShow(): void { this.checkData(); }

  // What is on this phone already: the game data NX2Harmony sends, the shader seed and the save slots.
  private checkData(): void {
    const filesDir = this.context().filesDir;
    try {
      this.dataSize = fs.statSync(filesDir + '/' + GameProfile.dataDir + '/romfs.bin').size;
    } catch (e) {
      this.dataSize = -1;
    }
    try {
      this.seedReady = fs.statSync(filesDir + '/' + GameProfile.seed).size > 0;
    } catch (e) {
      this.seedReady = false;
    }
    try {
      this.saveSlots = new SaveStore(filesDir, this.context().cacheDir).currentSlots().length;
    } catch (e) {
      this.saveSlots = 0;
    }
  }

  private saveSettings(): boolean {
    try {
      this.settings.save(this.context().filesDir, screenLongSide(), screenShortSide());
      return true;
    } catch (e) {
      this.message = '保存设置失败：' + String(e);
      return false;
    }
  }

  private askStart(): void {
    if (this.dataSize >= 0) {
      this.start();
      return;
    }
    this.getUIContext().showAlertDialog({
      title: '手机上还没有游戏数据',
      message: '在电脑上用 NX2Harmony 构建一次，它会把游戏数据推送到这台手机。没有数据时游戏无法启动。',
      primaryButton: { value: '知道了', action: () => {} },
      secondaryButton: { value: '仍然开始', action: () => { this.start(); } }
    });
  }

  private async start(): Promise<void> {
    const context = this.context();
    if (!this.saveSettings()) return;
    try {
      const kept = new SaveStore(context.filesDir, context.cacheDir).autoBackup();
      if (kept.length > 0) hilog.info(0x4D47, 'NGLauncher', 'saves backed up: %{public}s', kept);
    } catch (e) {
      hilog.error(0x4D47, 'NGLauncher', 'save backup: %{public}s', String(e));
    }
    this.getUIContext().getRouter().replaceUrl({ url: 'pages/Game' });
  }

  @Builder
  private hero() {
    Row() {
      Stack() {
        Image($r('app.media.game_icon')).width(72).height(72).borderRadius(19)
      }
      .padding(3)
      .borderRadius(25)
      .linearGradient({ angle: 135, colors: [[GameProfile.accent, 0], [GameProfile.accent2, 1]] })
      .shadow({ radius: 26, color: alpha(GameProfile.accent, 0.5), offsetY: 6 })
      Column() {
        Text(GameProfile.short.split('').join(' '))
          .fontSize(12).fontWeight(FontWeight.Bolder).fontColor(GameProfile.accent).letterSpacing(2)
        Text(GameProfile.title).fontSize(22).fontWeight(FontWeight.Bolder).fontColor(Ui.text).margin({ top: 2 })
          .maxLines(2)
        Row() {
          Text('v' + GameProfile.version).fontSize(11).fontWeight(FontWeight.Bold).fontColor('#0B1118')
            .backgroundColor(GameProfile.accent).borderRadius(8).padding({ left: 8, right: 8, top: 2, bottom: 2 })
          Text('鸿蒙原生运行').fontSize(11).fontColor(Ui.text2)
            .backgroundColor(alpha(Ui.card2, 0.9)).borderRadius(8).padding({ left: 8, right: 8, top: 2, bottom: 2 })
            .margin({ left: 6 })
        }.margin({ top: 6 })
      }.alignItems(HorizontalAlign.Start).margin({ left: 16 }).layoutWeight(1)
    }.width('100%').alignItems(VerticalAlign.Center)
  }

  @Builder
  private chips() {
    Row({ space: 8 }) {
      StatusChip({ badgeName: 'data', label: '游戏数据', value: this.dataSize >= 0 ? formatBytes(this.dataSize) : '未推送',
        state: this.dataSize >= 0 ? 0 : 2 }).layoutWeight(1)
      StatusChip({ badgeName: 'shader', label: '着色器', value: this.seedReady ? '已就绪' : '首次生成',
        state: this.seedReady ? 0 : 1 }).layoutWeight(1)
      StatusChip({ badgeName: 'slot', label: '存档', value: this.saveSlots > 0 ? this.saveSlots + ' 个' : '还没有',
        state: 0 }).layoutWeight(1)
    }.width('100%')
  }

  @Builder
  private startButton() {
    Stack() {
      Column().width('100%').height(62).borderRadius({ topLeft: 22, bottomRight: 22, topRight: 6, bottomLeft: 6 })
        .backgroundColor(GameProfile.accent).opacity(this.glow).blur(18)
      GameButton({ label: '开始游戏', badgeName: 'play', primary: true, big: true, fill: true,
        onTap: () => { this.askStart(); } })
    }.width('100%')
  }

  @Builder
  private tabBar() {
    Row() {
      ForEach(TABS, (t: TabItem) => {
        Row() {
          Image(badge(t.badgeName)).width(22).height(22).margin({ right: 6 })
            .opacity(this.tab === t.key ? 1 : 0.55)
          Text(t.label).fontSize(14).fontWeight(this.tab === t.key ? FontWeight.Bolder : FontWeight.Medium)
            .fontColor(this.tab === t.key ? Ui.text : Ui.text2)
        }
        .layoutWeight(1)
        .justifyContent(FlexAlign.Center)
        .height(40)
        .borderRadius(20)
        .backgroundColor(this.tab === t.key ? alpha(GameProfile.accent, 0.22) : Color.Transparent)
        .border({ width: this.tab === t.key ? 1 : 0, color: alpha(GameProfile.accent, 0.8) })
        .animation({ duration: 180, curve: Curve.Friction })
        .onClick(() => { this.tab = t.key; })
      }, (t: TabItem) => t.key)
    }
    .width('100%')
    .padding(4)
    .borderRadius(24)
    .backgroundColor(alpha(Ui.card, 0.92))
    .border({ width: 1, color: Ui.line })
    .shadow({ radius: 14, color: '#80000000', offsetY: 4 })
  }

  @Builder
  private page() {
    Column() {
      if (this.tab === 'saves') {
        SavesPanel({ onChanged: () => { this.checkData(); } })
      } else if (this.tab === 'about') {
        AboutPanel()
      } else {
        SettingsPanel({ settings: $settings, gameVersion: runtimeGameVersion(), applyLabel: '',
          onChanged: () => { this.saveSettings(); } })
      }
    }
    .layoutWeight(1)
    .height('100%')
    .padding(12)
    .borderRadius({ topLeft: 22, bottomRight: 22, topRight: 8, bottomLeft: 8 })
    .backgroundColor(alpha(Ui.card, 0.9))
    .border({ width: 1, color: Ui.line })
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      // the game's art across the whole screen, blurred, darkening towards the pages on the right
      Image($r('app.media.game_icon')).width('100%').height('100%').objectFit(ImageFit.Cover).blur(48).opacity(0.45)
      Column().width('100%').height('100%')
        .linearGradient({ angle: 90, colors: [[alpha(Ui.bg, 0.45), 0], [alpha(Ui.bg, 0.8), 0.42], [Ui.bg, 0.75]] })
      Row({ space: 18 }) {
        Column({ space: 12 }) {
          this.hero()
          this.chips()
          Blank()
          if (this.message.length > 0) {
            Text(this.message).fontSize(12).fontColor(Ui.bad)
          }
          this.startButton()
          this.tabBar()
        }.width('40%').height('100%')
        this.page()
      }.width('100%').height('100%').padding({ left: 36, right: 36, top: 22, bottom: 18 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Ui.bg)
  }
}
